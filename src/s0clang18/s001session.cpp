#include "s0clang18/s001session.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace pred::s0clang18 {
namespace {

Severity severityFromClang(clang::DiagnosticsEngine::Level level) {
    switch (level) {
    case clang::DiagnosticsEngine::Ignored:
    case clang::DiagnosticsEngine::Note:
    case clang::DiagnosticsEngine::Remark:
        return Severity::Note;
    case clang::DiagnosticsEngine::Warning:
        return Severity::Warning;
    case clang::DiagnosticsEngine::Error:
    case clang::DiagnosticsEngine::Fatal:
        return Severity::Error;
    }
    return Severity::Error;
}

bool hasErrorDiagnostic(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

bool hasLanguageStandardArg(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-std" || arg == "--std") return true;
        if (arg.rfind("-std=", 0) == 0 || arg.rfind("--std=", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string normalizedPath(const std::string& path) {
    try {
        return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).string();
    } catch (const std::exception&) {
        try {
            return std::filesystem::absolute(path).string();
        } catch (const std::exception&) {
            return path;
        }
    }
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> buildClangArgs(const Clang18Options& options) {
    std::vector<std::string> args = options.clang_args;
    if (!hasLanguageStandardArg(args)) {
        args.push_back("-std=" + options.cxx_standard);
    }
    if (!options.vullib_path.empty()) {
        args.push_back("-I" + options.vullib_path);
    }
    return args;
}

DebugLoc locFromSourceLocation(const clang::SourceManager& sm,
                               clang::SourceLocation loc) {
    DebugLoc out;
    if (loc.isInvalid()) return out;

    clang::SourceLocation expansion = sm.getExpansionLoc(loc);
    clang::PresumedLoc presumed = sm.getPresumedLoc(expansion);
    if (presumed.isValid()) {
        out.file = presumed.getFilename();
        out.line = static_cast<int>(presumed.getLine());
        out.column = static_cast<int>(presumed.getColumn());
        out.end_line = out.line;
        out.end_column = out.column;
        return out;
    }

    out.file = sm.getFilename(expansion).str();
    out.line = static_cast<int>(sm.getExpansionLineNumber(expansion));
    out.column = static_cast<int>(sm.getExpansionColumnNumber(expansion));
    out.end_line = out.line;
    out.end_column = out.column;
    return out;
}

ErrorContext makeContext(const std::string& source_name,
                         const DebugLoc& loc = {},
                         std::string note = {}) {
    ErrorContext context;
    context.stage = "s0clang18.1";
    context.source_file = source_name;
    context.loc = loc;
    context.note = std::move(note);
    return context;
}

std::string diagnosticMessage(const clang::Diagnostic& info) {
    llvm::SmallString<256> message;
    info.FormatDiagnostic(message);
    return std::string(message.str());
}

std::string diagnosticMessage(const clang::StoredDiagnostic& diagnostic) {
    return diagnostic.getMessage().str();
}

struct DiagnosticKey {
    Severity severity = Severity::Error;
    std::string message;
    std::string file;
    int line = 0;
    int column = 0;

    bool operator==(const DiagnosticKey& rhs) const {
        return severity == rhs.severity &&
               message == rhs.message &&
               file == rhs.file &&
               line == rhs.line &&
               column == rhs.column;
    }
};

struct DiagnosticKeyHash {
    std::size_t operator()(const DiagnosticKey& key) const {
        std::size_t h = std::hash<int>{}(static_cast<int>(key.severity));
        h ^= std::hash<std::string>{}(key.message) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(key.file) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.line) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.column) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

DiagnosticKey keyForDiagnostic(const Diagnostic& diagnostic) {
    return DiagnosticKey{
        diagnostic.severity,
        diagnostic.message,
        diagnostic.context.loc.file,
        diagnostic.context.loc.line,
        diagnostic.context.loc.column,
    };
}

void appendUniqueDiagnostic(std::vector<Diagnostic>& diagnostics,
                            std::unordered_set<DiagnosticKey, DiagnosticKeyHash>& seen,
                            Diagnostic diagnostic) {
    DiagnosticKey key = keyForDiagnostic(diagnostic);
    if (!seen.insert(std::move(key)).second) return;
    diagnostics.push_back(std::move(diagnostic));
}

class CaptureDiagnosticConsumer : public clang::DiagnosticConsumer {
public:
    explicit CaptureDiagnosticConsumer(std::string source_name)
        : source_name_(std::move(source_name)) {}

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& info) override {
        clang::DiagnosticConsumer::HandleDiagnostic(level, info);
        Diagnostic diagnostic;
        diagnostic.severity = severityFromClang(level);
        diagnostic.message = diagnosticMessage(info);
        DebugLoc loc;
        if (info.hasSourceManager()) {
            loc = locFromSourceLocation(info.getSourceManager(), info.getLocation());
        }
        diagnostic.context = makeContext(source_name_, loc, "clang diagnostic");
        diagnostics.push_back(std::move(diagnostic));
    }

    std::vector<Diagnostic> diagnostics;

private:
    std::string source_name_;
};

void appendStoredDiagnostics(const Clang18Options& options,
                             const clang::ASTUnit& ast_unit,
                             std::vector<Diagnostic>& diagnostics) {
    std::unordered_set<DiagnosticKey, DiagnosticKeyHash> seen;
    std::vector<Diagnostic> merged;
    for (auto& diagnostic : diagnostics) {
        appendUniqueDiagnostic(merged, seen, std::move(diagnostic));
    }

    for (auto it = ast_unit.stored_diag_begin(); it != ast_unit.stored_diag_end(); ++it) {
        Diagnostic diagnostic;
        diagnostic.severity = severityFromClang(it->getLevel());
        diagnostic.message = diagnosticMessage(*it);
        DebugLoc loc;
        if (it->getLocation().isValid()) {
            loc = locFromSourceLocation(ast_unit.getSourceManager(), it->getLocation());
        }
        diagnostic.context = makeContext(options.source_name, loc, "clang stored diagnostic");
        appendUniqueDiagnostic(merged, seen, std::move(diagnostic));
    }

    diagnostics = std::move(merged);
}

void appendSyntheticError(std::vector<Diagnostic>& diagnostics,
                          const Clang18Options& options,
                          std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.context = makeContext(options.source_name);
    diagnostic.message = std::move(message);
    diagnostics.push_back(std::move(diagnostic));
}

} // namespace

Clang18Session::Clang18Session() = default;
Clang18Session::~Clang18Session() = default;
Clang18Session::Clang18Session(Clang18Session&&) noexcept = default;
Clang18Session& Clang18Session::operator=(Clang18Session&&) noexcept = default;

StepResult<Clang18Session> createClang18Session(const Clang18Options& options) {
    StepResult<Clang18Session> result;

    std::optional<std::string> source_text = options.source_text;
    if (!source_text) source_text = readFile(options.source_name);
    if (!source_text) {
        appendSyntheticError(result.diagnostics, options,
                             "failed to read source file '" + options.source_name + "'");
        return result;
    }

    CaptureDiagnosticConsumer diagnostic_consumer(options.source_name);
    std::vector<std::string> args = buildClangArgs(options);
    std::string tool_name = "clang++-18";

    std::unique_ptr<clang::ASTUnit> ast_unit =
        clang::tooling::buildASTFromCodeWithArgs(
            *source_text,
            args,
            options.source_name,
            tool_name,
            std::make_shared<clang::PCHContainerOperations>(),
            clang::tooling::getClangStripDependencyFileAdjuster(),
            clang::tooling::FileContentMappings(),
            &diagnostic_consumer);

    result.diagnostics = std::move(diagnostic_consumer.diagnostics);
    if (!ast_unit) {
        if (result.diagnostics.empty()) {
            appendSyntheticError(result.diagnostics, options,
                                 "clang failed to build AST for '" + options.source_name + "'");
        }
        return result;
    }

    appendStoredDiagnostics(options, *ast_unit, result.diagnostics);
    if (hasErrorDiagnostic(result.diagnostics)) {
        return result;
    }

    Clang18Session session;
    session.ast_unit = std::move(ast_unit);
    session.ast_context = &session.ast_unit->getASTContext();
    session.source_manager = &session.ast_unit->getSourceManager();
    session.translation_unit = session.ast_context->getTranslationUnitDecl();
    session.main_file_id = session.source_manager->getMainFileID();
    session.main_file_path = normalizedPath(options.source_name);
    session.diagnostics = result.diagnostics;
    result.value = std::move(session);
    return result;
}

} // namespace pred::s0clang18
