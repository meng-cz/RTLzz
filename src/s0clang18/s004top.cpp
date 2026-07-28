#include "s0clang18/s004top.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>

#include <algorithm>
#include <sstream>

namespace pred::s0clang18 {
namespace {

bool wildcardMatch(const std::string& pattern, const std::string& text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string::npos;
    std::size_t star_text = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

Diagnostic makeError(const Clang18Session& session,
                     const SourceLocPolicy& loc_policy,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.4";
    diagnostic.context.source_file = session.main_file_path;
    diagnostic.context.loc = std::move(loc);
    (void)loc_policy;
    return diagnostic;
}

bool isOrdinarySourceFunction(const Clang18Session& session,
                              const clang::FunctionDecl* decl) {
    if (!decl) return false;
    if (llvm::isa<clang::CXXMethodDecl>(decl)) return false;
    if (!decl->isThisDeclarationADefinition()) return false;
    if (!isInMainSourceFile(session, decl->getLocation())) return false;
    return true;
}

class TopCandidateVisitor
    : public clang::RecursiveASTVisitor<TopCandidateVisitor> {
public:
    TopCandidateVisitor(const Clang18Session& session,
                        std::string pattern,
                        SourceLocPolicy loc_policy)
        : session_(session),
          pattern_(std::move(pattern)),
          loc_policy_(std::move(loc_policy)) {}

    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if (!isOrdinarySourceFunction(session_, decl)) return true;
        std::string name = decl->getNameAsString();
        if (!wildcardMatch(pattern_, name)) return true;

        TopFunctionCandidate candidate;
        candidate.name = std::move(name);
        candidate.function_decl = decl;
        candidate.loc = debugLocForRange(session_, decl->getSourceRange(), loc_policy_);
        candidates.push_back(std::move(candidate));
        return true;
    }

    std::vector<TopFunctionCandidate> candidates;

private:
    const Clang18Session& session_;
    std::string pattern_;
    SourceLocPolicy loc_policy_;
};

std::string joinCandidateNames(const std::vector<TopFunctionCandidate>& candidates) {
    std::ostringstream os;
    for (const auto& candidate : candidates) {
        os << " " << candidate.name;
    }
    return os.str();
}

std::optional<Diagnostic> validateTopSignature(const Clang18Session& session,
                                               const SourceLocPolicy& loc_policy,
                                               const TopFunctionCandidate& candidate) {
    const clang::FunctionDecl* decl = candidate.function_decl;
    if (!decl) {
        return makeError(session, loc_policy, candidate.loc,
                         "Internal error: selected top function has no FunctionDecl");
    }

    if (decl->getNumParams() != 0) {
        return makeError(session, loc_policy, candidate.loc,
                         "Top function '" + candidate.name +
                             "' must not have parameters");
    }

    if (!decl->getReturnType()->isVoidType()) {
        return makeError(session, loc_policy, candidate.loc,
                         "Top function '" + candidate.name +
                             "' must return void");
    }

    return std::nullopt;
}

} // namespace

std::vector<TopFunctionCandidate> collectTopFunctionCandidates(
    const Clang18Session& session,
    const std::string& top_pattern,
    const SourceLocPolicy& loc_policy) {
    if (!session.translation_unit || top_pattern.empty()) return {};
    TopCandidateVisitor visitor(session, top_pattern, loc_policy);
    visitor.TraverseDecl(session.translation_unit);
    return std::move(visitor.candidates);
}

StepResult<TopFunctionSelection> selectTopFunction(
    const Clang18Session& session,
    const std::string& top_pattern,
    const SourceLocPolicy& loc_policy) {
    StepResult<TopFunctionSelection> result;

    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, loc_policy, {},
            "S0Clang18 top selection requires a valid Clang18Session"));
        return result;
    }
    if (top_pattern.empty()) {
        result.diagnostics.push_back(makeError(
            session, loc_policy, {}, "Top function name must not be empty"));
        return result;
    }

    std::vector<TopFunctionCandidate> candidates =
        collectTopFunctionCandidates(session, top_pattern, loc_policy);

    if (candidates.empty()) {
        result.diagnostics.push_back(makeError(
            session, loc_policy, {},
            "Function '" + top_pattern + "' not found in " +
                session.main_file_path));
        return result;
    }

    if (candidates.size() > 1) {
        result.diagnostics.push_back(makeError(
            session, loc_policy, candidates.front().loc,
            "Function pattern '" + top_pattern +
                "' matched multiple functions:" + joinCandidateNames(candidates)));
        return result;
    }

    if (auto diagnostic = validateTopSignature(
            session, loc_policy, candidates.front())) {
        result.diagnostics.push_back(std::move(*diagnostic));
        return result;
    }

    TopFunctionSelection selection;
    selection.requested_name = top_pattern;
    selection.resolved_name = candidates.front().name;
    selection.function_decl = candidates.front().function_decl;
    selection.loc = candidates.front().loc;
    result.value = std::move(selection);
    return result;
}

} // namespace pred::s0clang18

