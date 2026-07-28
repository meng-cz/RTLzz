#include "s0clang18/s011lambdas.hpp"

#include <clang/AST/ASTLambda.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const Clang18Session& session,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.11";
    diagnostic.context.source_file = session.main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

const clang::FunctionDecl* canonicalFunction(const clang::FunctionDecl* decl) {
    if (!decl) return nullptr;
    return llvm::dyn_cast<clang::FunctionDecl>(decl->getCanonicalDecl());
}

const clang::FunctionDecl* canonicalTemplatePattern(
    const clang::FunctionDecl* function) {
    if (!function) return nullptr;
    if (const clang::FunctionTemplateDecl* primary = function->getPrimaryTemplate()) {
        return canonicalFunction(primary->getTemplatedDecl());
    }
    if (const clang::FunctionTemplateDecl* described =
            function->getDescribedFunctionTemplate()) {
        return canonicalFunction(described->getTemplatedDecl());
    }
    return nullptr;
}

bool isLambdaFunctionKind(FunctionEntityKind kind) {
    return kind == FunctionEntityKind::Lambda ||
           kind == FunctionEntityKind::GenericLambdaSpecialization;
}

std::string declName(const clang::Decl* decl) {
    if (const auto* named = llvm::dyn_cast_or_null<clang::NamedDecl>(decl)) {
        std::string name = named->getNameAsString();
        if (!name.empty()) return name;
        return named->getQualifiedNameAsString();
    }
    return {};
}

std::string loweredCaptureName(const std::string& source_name,
                               const clang::LambdaExpr* lambda_expr) {
    if (source_name == "this") return "__capture_this";
    if (source_name.empty()) return "__capture";
    (void)lambda_expr;
    return source_name;
}

class LambdaExprCollector
    : public clang::RecursiveASTVisitor<LambdaExprCollector> {
public:
    LambdaExprCollector(const Clang18Session& session,
                        SourceLocPolicy loc_policy)
        : session_(session), loc_policy_(std::move(loc_policy)) {}

    bool VisitLambdaExpr(clang::LambdaExpr* expr) {
        if (!expr || !isInMainSourceFile(session_, expr->getBeginLoc())) return true;
        const clang::CXXMethodDecl* call_operator = expr->getCallOperator();
        if (!call_operator) return true;

        const clang::FunctionDecl* canonical = canonicalFunction(call_operator);
        if (canonical) lambda_by_call_operator.emplace(canonical, expr);

        const clang::FunctionDecl* pattern = canonicalTemplatePattern(call_operator);
        if (pattern) lambda_by_template_pattern.emplace(pattern, expr);

        lambdas.push_back(expr);
        return true;
    }

    const clang::LambdaExpr* findForFunction(
        const clang::FunctionDecl* function) const {
        const clang::FunctionDecl* canonical = canonicalFunction(function);
        if (canonical) {
            auto found = lambda_by_call_operator.find(canonical);
            if (found != lambda_by_call_operator.end()) return found->second;
        }

        const clang::FunctionDecl* pattern = canonicalTemplatePattern(function);
        if (pattern) {
            auto found = lambda_by_call_operator.find(pattern);
            if (found != lambda_by_call_operator.end()) return found->second;
            found = lambda_by_template_pattern.find(pattern);
            if (found != lambda_by_template_pattern.end()) return found->second;
        }
        return nullptr;
    }

    std::vector<const clang::LambdaExpr*> lambdas;
    std::unordered_map<const clang::FunctionDecl*, const clang::LambdaExpr*>
        lambda_by_call_operator;
    std::unordered_map<const clang::FunctionDecl*, const clang::LambdaExpr*>
        lambda_by_template_pattern;

private:
    const Clang18Session& session_;
    SourceLocPolicy loc_policy_;
};

StepResult<pred::v2::TypeInfo> lowerCaptureType(
    const Clang18Session& session,
    const TypeLoweringContext& type_context,
    clang::QualType type,
    DebugLoc loc) {
    StepResult<pred::v2::TypeInfo> result;
    auto lowered = lowerQualType(type_context, type, loc);
    if (!lowered.ok()) {
        result.diagnostics = std::move(lowered.diagnostics);
        return result;
    }
    result.value = lowered.value->type;
    (void)session;
    return result;
}

StepResult<LambdaCapture> buildVariableCapture(
    const Clang18Session& session,
    const TypeLoweringContext& type_context,
    const clang::LambdaExpr* lambda_expr,
    const clang::LambdaCapture& capture,
    const clang::Expr* init,
    const SourceLocPolicy& loc_policy) {
    StepResult<LambdaCapture> result;
    const clang::ValueDecl* var = capture.getCapturedVar();
    DebugLoc loc = debugLocForLocation(session, capture.getLocation(), loc_policy);
    if (!loc.valid() && var) {
        loc = debugLocForRange(session, var->getSourceRange(), loc_policy);
    }

    clang::QualType capture_type = init ? init->getType() : clang::QualType();
    if (capture_type.isNull() && var) capture_type = var->getType();
    auto lowered = lowerCaptureType(session, type_context, capture_type, loc);
    if (!lowered.ok()) {
        result.diagnostics = std::move(lowered.diagnostics);
        return result;
    }

    LambdaCapture out;
    out.kind = capture.getCaptureKind() == clang::LCK_ByRef
        ? LambdaCaptureKind::ByReference
        : LambdaCaptureKind::ByCopy;
    out.source_name = declName(var);
    out.lowered_param_name = loweredCaptureName(out.source_name, lambda_expr);
    out.type = *lowered.value;
    out.captured_decl = var;
    out.loc = loc;

    if (out.kind == LambdaCaptureKind::ByReference) {
        out.type.is_reference = true;
        out.type.is_mutable = true;
    } else {
        out.type.is_reference = false;
        out.type.is_mutable = false;
        out.type.is_const = true;
    }

    result.value = std::move(out);
    return result;
}

StepResult<LambdaCapture> buildThisCapture(
    const Clang18Session& session,
    const TypeLoweringContext& type_context,
    const clang::LambdaExpr* lambda_expr,
    const clang::LambdaCapture& capture,
    const clang::Expr* init,
    const SourceLocPolicy& loc_policy) {
    StepResult<LambdaCapture> result;
    DebugLoc loc = debugLocForLocation(session, capture.getLocation(), loc_policy);
    clang::QualType this_type = init ? init->getType() : clang::QualType();
    if (this_type.isNull()) {
        result.diagnostics.push_back(makeError(
            session, loc, "Unable to determine type of lambda this capture"));
        return result;
    }

    TypeLoweringContext pointer_context = type_context;
    pointer_context.options.allow_pointer_types = true;
    auto lowered = lowerCaptureType(session, pointer_context, this_type, loc);
    if (!lowered.ok()) {
        result.diagnostics = std::move(lowered.diagnostics);
        return result;
    }

    LambdaCapture out;
    out.kind = LambdaCaptureKind::This;
    out.source_name = "this";
    out.lowered_param_name = loweredCaptureName(out.source_name, lambda_expr);
    out.type = *lowered.value;
    out.captured_decl = nullptr;
    out.loc = loc;
    result.value = std::move(out);
    return result;
}

StepResult<LambdaInfo> collectOneLambda(
    const Clang18Session& session,
    const FunctionEntity& function,
    const clang::LambdaExpr* lambda_expr,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy) {
    StepResult<LambdaInfo> result;
    LambdaInfo info;
    info.function_id = function.id;
    info.lambda_expr = lambda_expr;
    info.call_operator = lambda_expr ? lambda_expr->getCallOperator() : nullptr;
    info.loc = lambda_expr
        ? debugLocForRange(session, lambda_expr->getSourceRange(), loc_policy)
        : function.loc;

    if (!lambda_expr || !info.call_operator) {
        result.diagnostics.push_back(makeError(
            session, function.loc,
            "Reachable lambda function has no matching LambdaExpr"));
        return result;
    }

    auto init = lambda_expr->capture_init_begin();
    auto init_end = lambda_expr->capture_init_end();
    std::unordered_set<const clang::Decl*> seen;
    for (const clang::LambdaCapture& capture : lambda_expr->captures()) {
        const clang::Expr* init_expr = init != init_end ? *init : nullptr;
        if (init != init_end) ++init;

        StepResult<LambdaCapture> capture_result;
        if (capture.capturesThis()) {
            capture_result = buildThisCapture(
                session, type_context, lambda_expr, capture, init_expr, loc_policy);
        } else if (capture.capturesVariable()) {
            const clang::ValueDecl* var = capture.getCapturedVar();
            const clang::Decl* canonical = var ? var->getCanonicalDecl() : nullptr;
            if (canonical && !seen.insert(canonical).second) continue;
            capture_result = buildVariableCapture(
                session, type_context, lambda_expr, capture, init_expr, loc_policy);
        } else {
            result.diagnostics.push_back(makeError(
                session,
                debugLocForLocation(session, capture.getLocation(), loc_policy),
                "Unsupported lambda capture kind"));
            continue;
        }

        if (!capture_result.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      capture_result.diagnostics.begin(),
                                      capture_result.diagnostics.end());
            continue;
        }
        info.captures.push_back(std::move(*capture_result.value));
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(info);
    return result;
}

void addLambdaInfo(LambdaCaptureTable& table, LambdaInfo info) {
    std::size_t index = table.lambdas.size();
    table.lambda_by_function[info.function_id] = index;
    table.lambdas.push_back(std::move(info));
}

} // namespace

StepResult<LambdaCaptureTable> resolveLambdaCaptures(
    const Clang18Session& session,
    const FunctionReachabilityGraph& reachability,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy) {
    StepResult<LambdaCaptureTable> result;
    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, {}, "S0Clang18 lambda capture resolve requires a valid Clang18Session"));
        return result;
    }

    LambdaExprCollector collector(session, loc_policy);
    collector.TraverseDecl(session.translation_unit);

    LambdaCaptureTable table;
    for (const FunctionEntity& function : reachability.functions) {
        if (!isLambdaFunctionKind(function.kind)) continue;
        const clang::LambdaExpr* lambda_expr =
            collector.findForFunction(function.function_decl);
        auto info = collectOneLambda(session, function, lambda_expr, type_context,
                                     loc_policy);
        if (!info.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      info.diagnostics.begin(),
                                      info.diagnostics.end());
            continue;
        }
        addLambdaInfo(table, std::move(*info.value));
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(table);
    return result;
}

const LambdaInfo* findLambdaInfo(const LambdaCaptureTable& table,
                                 SemanticEntityId function_id) {
    auto found = table.lambda_by_function.find(function_id);
    if (found == table.lambda_by_function.end()) return nullptr;
    if (found->second >= table.lambdas.size()) return nullptr;
    return &table.lambdas[found->second];
}

} // namespace pred::s0clang18
