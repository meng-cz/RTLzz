#include "s0clang18/s005semantic.hpp"

#include <clang/AST/ASTLambda.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <algorithm>
#include <optional>
#include <sstream>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const Clang18Session& session,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.5";
    diagnostic.context.source_file = session.main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

std::string entityName(const clang::Decl* decl) {
    if (const auto* named = llvm::dyn_cast_or_null<clang::NamedDecl>(decl)) {
        std::string name = named->getNameAsString();
        if (!name.empty()) return name;
        return named->getQualifiedNameAsString();
    }
    return {};
}

const clang::Decl* canonicalDecl(const clang::Decl* decl) {
    return decl ? decl->getCanonicalDecl() : nullptr;
}

std::optional<SemanticEntityKind> classifyDecl(const clang::Decl* decl) {
    if (!decl) return std::nullopt;

    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        if (clang::isLambdaCallOperator(method)) {
            return SemanticEntityKind::LambdaCallOperator;
        }
        return SemanticEntityKind::Method;
    }
    if (llvm::isa<clang::FunctionTemplateDecl>(decl)) {
        return SemanticEntityKind::FunctionTemplate;
    }
    if (llvm::isa<clang::FunctionDecl>(decl)) {
        return SemanticEntityKind::Function;
    }
    if (llvm::isa<clang::VarDecl>(decl)) {
        return SemanticEntityKind::Variable;
    }
    if (llvm::isa<clang::FieldDecl>(decl)) {
        return SemanticEntityKind::Field;
    }
    if (llvm::isa<clang::RecordDecl>(decl)) {
        return SemanticEntityKind::Record;
    }
    return std::nullopt;
}

bool hasMainFileLocation(const Clang18Session& session, const clang::Decl* decl) {
    if (!decl) return false;
    clang::SourceLocation loc = decl->getLocation();
    if (loc.isInvalid()) loc = decl->getBeginLoc();
    if (loc.isInvalid()) return false;
    return isInMainSourceFile(session, loc);
}

class SemanticIndexBuilder
    : public clang::RecursiveASTVisitor<SemanticIndexBuilder> {
public:
    SemanticIndexBuilder(const Clang18Session& session,
                         SourceLocPolicy loc_policy)
        : session_(session), loc_policy_(std::move(loc_policy)) {}

    bool VisitNamedDecl(clang::NamedDecl* decl) {
        addDecl(decl);
        return true;
    }

    bool VisitLambdaExpr(clang::LambdaExpr* expr) {
        if (!expr || !isInMainSourceFile(session_, expr->getBeginLoc())) return true;
        addDecl(expr->getCallOperator(),
                debugLocForRange(session_, expr->getSourceRange(), loc_policy_));
        return true;
    }

    void addDecl(const clang::Decl* decl, std::optional<DebugLoc> override_loc = std::nullopt) {
        std::optional<SemanticEntityKind> kind = classifyDecl(decl);
        if (!kind) return;
        if (!override_loc && !hasMainFileLocation(session_, decl)) return;

        const clang::Decl* canonical = canonicalDecl(decl);
        if (!canonical) canonical = decl;

        auto existing = index.entity_by_decl.find(canonical);
        if (existing != index.entity_by_decl.end()) {
            index.entity_by_decl.emplace(decl, existing->second);
            return;
        }

        SemanticEntity entity;
        entity.id = static_cast<SemanticEntityId>(index.entities.size());
        entity.kind = *kind;
        entity.name = entityName(decl);
        entity.decl = decl;
        entity.loc = override_loc
            ? *override_loc
            : debugLocForRange(session_, decl->getSourceRange(), loc_policy_);

        index.entity_by_decl.emplace(decl, entity.id);
        index.entity_by_decl.emplace(canonical, entity.id);
        if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            if (const clang::FunctionDecl* definition = function->getDefinition()) {
                index.entity_by_decl.emplace(definition, entity.id);
            }
            if (clang::FunctionTemplateDecl* templ =
                    function->getDescribedFunctionTemplate()) {
                index.entity_by_decl.emplace(templ, entity.id);
            }
        }
        if (const auto* function_template =
                llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
            index.entity_by_decl.emplace(function_template->getTemplatedDecl(),
                                         entity.id);
        }

        if (!entity.name.empty()) {
            index.entities_by_name[entity.name].push_back(entity.id);
        }
        index.entities.push_back(std::move(entity));
    }

    SemanticIndex index;

private:
    const Clang18Session& session_;
    SourceLocPolicy loc_policy_;
};

bool containsError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

std::string portNames(const PortDeclTable& ports) {
    std::ostringstream os;
    for (const auto& port : ports.ports) os << " " << port.name;
    return os.str();
}

void validateRequiredEntities(const Clang18Session& session,
                              const TopFunctionSelection& top,
                              const PortDeclTable& ports,
                              const SemanticIndex& index,
                              std::vector<Diagnostic>& diagnostics) {
    if (!findEntity(index, top.function_decl)) {
        diagnostics.push_back(makeError(
            session, top.loc,
            "Semantic index did not include selected top function '" +
                top.resolved_name + "'"));
    }
    for (const auto& port : ports.ports) {
        if (findEntity(index, port.var_decl)) continue;
        diagnostics.push_back(makeError(
            session, port.decl_loc,
            "Semantic index did not include global port declaration '" +
                port.name + "'"));
    }
}

} // namespace

StepResult<SemanticIndex> buildSemanticIndex(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const PortDeclTable& ports,
    const SourceLocPolicy& loc_policy) {
    StepResult<SemanticIndex> result;
    if (!session.translation_unit || !session.ast_context) {
        result.diagnostics.push_back(makeError(
            session, {}, "S0Clang18 semantic index requires a valid Clang18Session"));
        return result;
    }
    if (!top.function_decl) {
        result.diagnostics.push_back(makeError(
            session, top.loc, "S0Clang18 semantic index requires a selected top function"));
        return result;
    }

    SemanticIndexBuilder builder(session, loc_policy);
    builder.TraverseDecl(session.translation_unit);
    validateRequiredEntities(session, top, ports, builder.index, result.diagnostics);
    (void)portNames;

    if (containsError(result.diagnostics)) return result;
    result.value = std::move(builder.index);
    return result;
}

const SemanticEntity* findEntity(const SemanticIndex& index, const clang::Decl* decl) {
    if (!decl) return nullptr;
    auto it = index.entity_by_decl.find(decl);
    if (it == index.entity_by_decl.end()) {
        it = index.entity_by_decl.find(canonicalDecl(decl));
    }
    if (it == index.entity_by_decl.end()) return nullptr;
    if (it->second < 0 ||
        static_cast<std::size_t>(it->second) >= index.entities.size()) {
        return nullptr;
    }
    return &index.entities[static_cast<std::size_t>(it->second)];
}

} // namespace pred::s0clang18
