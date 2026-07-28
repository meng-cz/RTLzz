#include "s0clang18/s002sourceloc.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <filesystem>

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

clang::SourceLocation primaryLocation(const clang::SourceManager& sm,
                                      clang::SourceLocation loc,
                                      const SourceLocPolicy& policy) {
    if (loc.isInvalid()) return loc;
    switch (policy.primary_kind) {
    case LocKind::Expansion:
        return sm.getExpansionLoc(loc);
    case LocKind::Spelling:
        return sm.getSpellingLoc(loc);
    case LocKind::Presumed:
        return sm.getExpansionLoc(loc);
    }
    return sm.getExpansionLoc(loc);
}

std::string filenameForLocation(const clang::SourceManager& sm,
                                clang::SourceLocation loc,
                                const SourceLocPolicy& policy) {
    if (loc.isInvalid()) return {};
    if (policy.primary_kind == LocKind::Presumed) {
        clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
        if (presumed.isValid()) {
            return canonicalPath(presumed.getFilename(), policy.canonicalize_paths);
        }
    }
    return canonicalPath(sm.getFilename(loc).str(), policy.canonicalize_paths);
}

int lineForLocation(const clang::SourceManager& sm,
                    clang::SourceLocation loc,
                    const SourceLocPolicy& policy) {
    if (loc.isInvalid()) return 0;
    if (policy.primary_kind == LocKind::Presumed) {
        clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
        return presumed.isValid() ? static_cast<int>(presumed.getLine()) : 0;
    }
    return static_cast<int>(sm.getSpellingLineNumber(loc));
}

int columnForLocation(const clang::SourceManager& sm,
                      clang::SourceLocation loc,
                      const SourceLocPolicy& policy) {
    if (loc.isInvalid()) return 0;
    if (policy.primary_kind == LocKind::Presumed) {
        clang::PresumedLoc presumed = sm.getPresumedLoc(loc);
        return presumed.isValid() ? static_cast<int>(presumed.getColumn()) : 0;
    }
    return static_cast<int>(sm.getSpellingColumnNumber(loc));
}

clang::SourceLocation endOfToken(const Clang18Session& session,
                                 clang::SourceLocation loc,
                                 const SourceLocPolicy& policy) {
    if (!session.source_manager || !session.ast_context || loc.isInvalid()) {
        return loc;
    }
    clang::SourceLocation primary = primaryLocation(*session.source_manager, loc, policy);
    clang::SourceLocation end = clang::Lexer::getLocForEndOfToken(
        primary, 0, *session.source_manager, session.ast_context->getLangOpts());
    return end.isValid() ? end : primary;
}

} // namespace

DebugLoc debugLocForLocation(const Clang18Session& session,
                             clang::SourceLocation loc,
                             const SourceLocPolicy& policy) {
    DebugLoc out;
    if (!session.source_manager || loc.isInvalid()) return out;

    clang::SourceLocation primary = primaryLocation(*session.source_manager, loc, policy);
    if (primary.isInvalid()) return out;

    out.file = filenameForLocation(*session.source_manager, primary, policy);
    out.line = lineForLocation(*session.source_manager, primary, policy);
    out.column = columnForLocation(*session.source_manager, primary, policy);
    out.end_line = out.line;
    out.end_column = out.column;
    return out;
}

DebugLoc debugLocForRange(const Clang18Session& session,
                          clang::SourceRange range,
                          const SourceLocPolicy& policy) {
    DebugLoc out;
    if (!session.source_manager || range.isInvalid()) return out;

    clang::SourceLocation begin =
        primaryLocation(*session.source_manager, range.getBegin(), policy);
    clang::SourceLocation end = endOfToken(session, range.getEnd(), policy);
    if (begin.isInvalid()) return out;
    if (end.isInvalid()) end = begin;

    out.file = filenameForLocation(*session.source_manager, begin, policy);
    out.line = lineForLocation(*session.source_manager, begin, policy);
    out.column = columnForLocation(*session.source_manager, begin, policy);
    out.end_line = lineForLocation(*session.source_manager, end, policy);
    out.end_column = columnForLocation(*session.source_manager, end, policy);
    return out;
}

std::optional<SourceTextSlice> sourceTextForRange(const Clang18Session& session,
                                                  clang::SourceRange range) {
    if (!session.source_manager || !session.ast_context || range.isInvalid()) {
        return std::nullopt;
    }

    bool invalid = false;
    llvm::StringRef text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(range),
        *session.source_manager,
        session.ast_context->getLangOpts(),
        &invalid);
    if (invalid) return std::nullopt;

    SourceTextSlice out;
    out.text = text.str();
    out.loc = debugLocForRange(session, range);
    return out;
}

bool isInMainSourceFile(const Clang18Session& session, clang::SourceLocation loc) {
    if (!session.source_manager || loc.isInvalid()) return false;
    return session.source_manager->isWrittenInMainFile(
        session.source_manager->getExpansionLoc(loc));
}

} // namespace pred::s0clang18

