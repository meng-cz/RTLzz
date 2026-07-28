#include "s0clang18/s003ports.hpp"

#include "v2/V2Types.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/APInt.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace pred::s0clang18 {
namespace {

std::string canonicalPath(const std::string& path, bool enabled) {
    if (!enabled || path.empty()) return path;
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

std::string mainFileName(const Clang18Session& session,
                         const SourceLocPolicy& loc_policy) {
    if (!session.source_manager || !session.main_file_id.isValid()) {
        return canonicalPath(session.main_file_path, loc_policy.canonicalize_paths);
    }
    bool invalid = false;
    clang::SourceLocation start =
        session.source_manager->getLocForStartOfFile(session.main_file_id);
    std::string name = session.source_manager->getBufferName(start, &invalid).str();
    if (invalid || name.empty()) name = session.main_file_path;
    return canonicalPath(name, loc_policy.canonicalize_paths);
}

Diagnostic makeError(const Clang18Session& session,
                     const SourceLocPolicy& loc_policy,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.3";
    diagnostic.context.source_file = mainFileName(session, loc_policy);
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

std::optional<std::string> mainFileText(const Clang18Session& session) {
    if (!session.source_manager || !session.main_file_id.isValid()) return std::nullopt;
    bool invalid = false;
    llvm::StringRef text = session.source_manager->getBufferData(
        session.main_file_id, &invalid);
    if (invalid) return std::nullopt;
    return text.str();
}

std::vector<PortPragma> parsePortPragmas(const Clang18Session& session,
                                         const SourceLocPolicy& loc_policy,
                                         std::vector<Diagnostic>& diagnostics) {
    std::vector<PortPragma> pragmas;
    std::optional<std::string> source = mainFileText(session);
    if (!source) {
        DebugLoc loc;
        loc.file = mainFileName(session, loc_policy);
        diagnostics.push_back(makeError(session, loc_policy, loc,
                                        "failed to read main source buffer for port pragmas"));
        return pragmas;
    }

    const std::regex pragma_re(
        R"(^\s*#\s*pragma\s+(input_port|output_port)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$)");
    std::istringstream lines(*source);
    std::string line;
    int line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        std::smatch match;
        if (!std::regex_match(line, match, pragma_re)) continue;

        PortPragma pragma;
        pragma.direction = match[1].str() == "input_port"
            ? PortDirection::Input
            : PortDirection::Output;
        pragma.name = match[2].str();
        pragma.loc.file = mainFileName(session, loc_policy);
        pragma.loc.line = line_number;
        std::size_t pragma_col = line.find("#");
        pragma.loc.column = static_cast<int>((pragma_col == std::string::npos ? 0 : pragma_col) + 1);
        pragma.loc.end_line = line_number;
        pragma.loc.end_column = static_cast<int>(line.size() + 1);
        pragmas.push_back(std::move(pragma));
    }
    return pragmas;
}

bool hasDuplicatePragma(const std::vector<PortPragma>& pragmas,
                        const Clang18Session& session,
                        const SourceLocPolicy& loc_policy,
                        std::vector<Diagnostic>& diagnostics) {
    std::unordered_set<std::string> seen;
    bool duplicate = false;
    for (const auto& pragma : pragmas) {
        if (seen.insert(pragma.name).second) continue;
        diagnostics.push_back(makeError(
            session, loc_policy, pragma.loc,
            "Duplicate or conflicting port pragma for global variable '" +
                pragma.name + "'"));
        duplicate = true;
    }
    return duplicate;
}

std::optional<pred::v2::TypeInfo> lowerSupportedPortType(
    const clang::ASTContext& ast_context,
    clang::QualType type);

std::optional<int> templateIntegralArgument(const clang::TemplateArgument& arg) {
    if (arg.getKind() != clang::TemplateArgument::Integral) return std::nullopt;
    return static_cast<int>(arg.getAsIntegral().getExtValue());
}

std::optional<pred::v2::TypeInfo> lowerBuiltinPortType(
    const clang::ASTContext& ast_context,
    clang::QualType type) {
    if (type->isBooleanType()) return pred::v2::make_bool_type();
    if (!type->isIntegerType()) return std::nullopt;
    if (type->isEnumeralType()) return std::nullopt;

    int width = static_cast<int>(ast_context.getTypeSize(type));
    if (width <= 0 || width > 64) return std::nullopt;

    pred::v2::TypeInfo out;
    out.name = type.getAsString();
    out.width = width;
    out.is_signed = type->isSignedIntegerType();
    out.is_hw_int = true;
    out.hw_kind = "builtin";
    return out;
}

std::optional<pred::v2::TypeInfo> lowerRecordPortType(
    const clang::ASTContext& ast_context,
    clang::QualType type) {
    const auto* record_type = type->getAs<clang::RecordType>();
    if (!record_type) return std::nullopt;
    const clang::RecordDecl* record_decl = record_type->getDecl();
    const auto* specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record_decl);
    if (!specialization) return std::nullopt;

    std::string qualified_name =
        specialization->getSpecializedTemplate()->getQualifiedNameAsString();
    std::string template_name =
        specialization->getSpecializedTemplate()->getNameAsString();
    const clang::TemplateArgumentList& args = specialization->getTemplateArgs();

    if (template_name == "Int" &&
        (qualified_name == "Int" || qualified_name == "vulfixint::Int")) {
        if (args.size() < 1) return std::nullopt;
        std::optional<int> width = templateIntegralArgument(args[0]);
        if (!width || *width <= 0) return std::nullopt;
        return pred::v2::make_hw_type("Int", *width, false);
    }

    if (qualified_name == "std::array") {
        if (args.size() < 2 ||
            args[0].getKind() != clang::TemplateArgument::Type) {
            return std::nullopt;
        }
        std::optional<int> size = templateIntegralArgument(args[1]);
        if (!size || *size <= 0) return std::nullopt;
        std::optional<pred::v2::TypeInfo> elem =
            lowerSupportedPortType(ast_context, args[0].getAsType());
        if (!elem) return std::nullopt;

        pred::v2::TypeInfo out = *elem;
        out.name = type.getAsString();
        out.is_array = true;
        out.array_size = *size;
        out.array_dims.insert(out.array_dims.begin(), *size);
        return out;
    }

    return std::nullopt;
}

std::optional<pred::v2::TypeInfo> lowerSupportedPortType(
    const clang::ASTContext& ast_context,
    clang::QualType type) {
    if (type.isNull()) return std::nullopt;
    if (type.isConstQualified()) return std::nullopt;
    if (type->isPointerType() || type->isReferenceType()) return std::nullopt;

    clang::QualType canonical = type.getCanonicalType();
    if (canonical.isConstQualified()) return std::nullopt;
    if (canonical->isPointerType() || canonical->isReferenceType()) {
        return std::nullopt;
    }

    if (auto builtin = lowerBuiltinPortType(ast_context, canonical)) return builtin;
    if (auto record = lowerRecordPortType(ast_context, type)) return record;
    if (auto record = lowerRecordPortType(ast_context, canonical)) return record;
    return std::nullopt;
}

class FileScopeVarCollector
    : public clang::RecursiveASTVisitor<FileScopeVarCollector> {
public:
    explicit FileScopeVarCollector(const Clang18Session& session)
        : session_(session) {}

    bool VisitVarDecl(clang::VarDecl* decl) {
        if (!decl || decl->isStaticDataMember() || !decl->isFileVarDecl()) {
            return true;
        }
        if (!isInMainSourceFile(session_, decl->getLocation())) return true;
        vars.push_back(decl);
        return true;
    }

    std::vector<const clang::VarDecl*> vars;

private:
    const Clang18Session& session_;
};

const PortPragma* findPragma(const std::vector<PortPragma>& pragmas,
                             const std::string& name) {
    auto it = std::find_if(pragmas.begin(), pragmas.end(),
                           [&](const PortPragma& pragma) {
                               return pragma.name == name;
                           });
    return it == pragmas.end() ? nullptr : &*it;
}

bool hasSyntacticInitializer(const Clang18Session& session,
                             const clang::VarDecl* var,
                             const SourceLocPolicy& loc_policy) {
    std::optional<SourceTextSlice> source =
        sourceTextForRange(session, var->getSourceRange());
    if (!source) {
        return var->getInitStyle() != clang::VarDecl::CInit;
    }

    std::size_t name_pos = source->text.rfind(var->getNameAsString());
    std::string tail = name_pos == std::string::npos
        ? source->text
        : source->text.substr(name_pos + var->getNameAsString().size());
    (void)loc_policy;
    return tail.find('=') != std::string::npos ||
           tail.find('{') != std::string::npos ||
           tail.find('(') != std::string::npos;
}

bool tableHasErrors(const PortDeclTable& table,
                    const std::vector<Diagnostic>& diagnostics) {
    (void)table;
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

} // namespace

StepResult<PortDeclTable> collectPragmaAndPortDecls(
    const Clang18Session& session,
    const SourceLocPolicy& loc_policy) {
    StepResult<PortDeclTable> result;
    PortDeclTable table;

    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, loc_policy, {},
            "S0Clang18 port collection requires a valid Clang18Session"));
        return result;
    }

    table.pragmas = parsePortPragmas(session, loc_policy, result.diagnostics);
    hasDuplicatePragma(table.pragmas, session, loc_policy, result.diagnostics);

    FileScopeVarCollector collector(session);
    collector.TraverseDecl(session.translation_unit);

    std::unordered_set<std::string> seen_vars;
    for (const clang::VarDecl* var : collector.vars) {
        std::string name = var->getNameAsString();
        seen_vars.insert(name);

        const PortPragma* pragma = findPragma(table.pragmas, name);
        DebugLoc decl_loc = debugLocForRange(session, var->getSourceRange(), loc_policy);
        if (!pragma) {
            result.diagnostics.push_back(makeError(
                session, loc_policy, decl_loc,
                "File-scope global variable '" + name +
                    "' is not declared by #pragma input_port or #pragma output_port"));
            continue;
        }

        if (hasSyntacticInitializer(session, var, loc_policy)) {
            result.diagnostics.push_back(makeError(
                session, loc_policy, decl_loc,
                "Global port '" + name + "' must not have an initializer"));
            continue;
        }

        std::optional<pred::v2::TypeInfo> type =
            lowerSupportedPortType(*session.ast_context, var->getType());
        if (!type) {
            result.diagnostics.push_back(makeError(
                session, loc_policy, decl_loc,
                "Unsupported global port type for '" + name +
                    "': only bool, Int<N>, builtin integers, and std::array forms of those types are allowed"));
            continue;
        }

        RawPortDecl port;
        port.name = name;
        port.direction = pragma->direction;
        port.var_decl = var;
        port.type = *type;
        port.decl_loc = std::move(decl_loc);
        port.pragma_loc = pragma->loc;
        table.port_by_name[port.name] = table.ports.size();
        table.ports.push_back(std::move(port));
    }

    for (const auto& pragma : table.pragmas) {
        if (seen_vars.count(pragma.name)) continue;
        result.diagnostics.push_back(makeError(
            session, loc_policy, pragma.loc,
            "Port pragma names no file-scope global variable: '" +
                pragma.name + "'"));
    }

    if (tableHasErrors(table, result.diagnostics)) return result;
    result.value = std::move(table);
    return result;
}

pred::v2::ParamDirection toV2Direction(PortDirection direction) {
    return direction == PortDirection::Output
        ? pred::v2::ParamDirection::Output
        : pred::v2::ParamDirection::Input;
}

} // namespace pred::s0clang18
