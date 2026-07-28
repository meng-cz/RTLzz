#include "s0clang18/s015calls.hpp"

#include <clang/AST/ASTLambda.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>

#include <algorithm>
#include <optional>
#include <string>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const ExprBuildContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.15";
    if (context.session) diagnostic.context.source_file = context.session->main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

DebugLoc exprLoc(const ExprBuildContext& context, const clang::Expr* expr) {
    if (!context.session || !expr) return {};
    return debugLocForRange(*context.session, expr->getSourceRange(),
                            context.loc_policy);
}

const clang::FunctionDecl* canonicalFunction(const clang::FunctionDecl* decl) {
    if (!decl) return nullptr;
    return llvm::dyn_cast<clang::FunctionDecl>(decl->getCanonicalDecl());
}

const clang::FunctionDecl* directCallee(const clang::CallExpr* expr) {
    if (!expr) return nullptr;
    if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
        if (const clang::CXXMethodDecl* method = member->getMethodDecl()) {
            return method;
        }
    }
    return expr->getDirectCallee();
}

std::string declName(const clang::NamedDecl* decl) {
    if (!decl) return {};
    std::string qualified = decl->getQualifiedNameAsString();
    if (!qualified.empty()) return qualified;
    return decl->getNameAsString();
}

std::string unqualifiedName(const clang::NamedDecl* decl) {
    return decl ? decl->getNameAsString() : std::string{};
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

const FunctionEntity* reachableFunctionForDecl(
    const FunctionReachabilityGraph* graph,
    const clang::FunctionDecl* function) {
    if (!graph || !function) return nullptr;
    auto found = graph->function_by_decl.find(function);
    if (found == graph->function_by_decl.end()) {
        if (const clang::FunctionDecl* canonical = canonicalFunction(function)) {
            found = graph->function_by_decl.find(canonical);
        }
    }
    if (found == graph->function_by_decl.end()) return nullptr;
    return findFunctionEntity(*graph, found->second);
}

BoundCallKind boundKindForEntity(const FunctionEntity& entity,
                                 const clang::CallExpr* expr) {
    switch (entity.kind) {
    case FunctionEntityKind::Top:
    case FunctionEntityKind::Helper:
        return BoundCallKind::Helper;
    case FunctionEntityKind::FunctionTemplateSpecialization:
        return BoundCallKind::TemplateHelper;
    case FunctionEntityKind::Lambda:
        return BoundCallKind::Lambda;
    case FunctionEntityKind::GenericLambdaSpecialization:
        return BoundCallKind::GenericLambda;
    case FunctionEntityKind::Method:
        if (llvm::isa_and_nonnull<clang::CXXMemberCallExpr>(expr)) {
            return BoundCallKind::MemberHelper;
        }
        return BoundCallKind::Helper;
    }
    return BoundCallKind::Unknown;
}

std::vector<long long> templateValuesForFunction(
    const ExprBuildContext& context,
    const clang::FunctionDecl* function,
    const FunctionEntity* entity,
    DebugLoc loc,
    std::vector<Diagnostic>& diagnostics) {
    if (entity && !entity->key.template_values.empty()) {
        return entity->key.template_values;
    }

    std::vector<long long> values;
    const clang::TemplateArgumentList* args =
        function ? function->getTemplateSpecializationArgs() : nullptr;
    if (!args) return values;
    if (!context.const_eval) return values;

    for (const clang::TemplateArgument& arg : args->asArray()) {
        if (arg.getKind() == clang::TemplateArgument::Pack) {
            for (const clang::TemplateArgument& packed : arg.pack_elements()) {
                if (packed.getKind() != clang::TemplateArgument::Integral) continue;
                auto value = evalTemplateIntegralArgument(*context.const_eval, packed, loc);
                diagnostics.insert(diagnostics.end(),
                                   value.diagnostics.begin(),
                                   value.diagnostics.end());
                if (value.value) values.push_back(*value.value);
            }
            continue;
        }
        if (arg.getKind() != clang::TemplateArgument::Integral) continue;
        auto value = evalTemplateIntegralArgument(*context.const_eval, arg, loc);
        diagnostics.insert(diagnostics.end(),
                           value.diagnostics.begin(),
                           value.diagnostics.end());
        if (value.value) values.push_back(*value.value);
    }
    return values;
}

bool isFixintType(clang::QualType type) {
    type = type.getCanonicalType();
    if (type->isReferenceType()) type = type->getPointeeType().getCanonicalType();
    const auto* record = type->getAsCXXRecordDecl();
    if (!record) return false;
    std::string qualified = record->getQualifiedNameAsString();
    return qualified == "vulfixint::Int" ||
           qualified == "vulfixint::IntSliceRef" ||
           qualified == "vulfixint::IntConstSliceRef" ||
           qualified == "vulfixint::IntBitRef" ||
           qualified == "vulfixint::IntConstBitRef" ||
           qualified == "vulfixint::IntSignedView";
}

bool isFixintReceiver(const clang::CXXMemberCallExpr* expr) {
    if (!expr) return false;
    const clang::Expr* receiver = expr->getImplicitObjectArgument();
    return receiver && isFixintType(receiver->getType());
}

bool isSupportedFixintMemberAPI(const clang::CXXMemberCallExpr* expr,
                                std::string& api_name) {
    const clang::CXXMethodDecl* method = expr ? expr->getMethodDecl() : nullptr;
    api_name = unqualifiedName(method);
    if (!isFixintReceiver(expr)) return false;
    return api_name == "at" ||
           api_name == "pick" ||
           api_name == "to" ||
           api_name == "sint";
}

bool isSupportedRTLZZFreeAPI(const clang::FunctionDecl* function,
                             std::string& api_name) {
    api_name = unqualifiedName(function);
    return api_name == "Cat" ||
           api_name == "Repeat" ||
           api_name == "ReduceAnd" ||
           api_name == "ReduceOr" ||
           api_name == "ReduceXor" ||
           api_name == "ZExt" ||
           api_name == "zext" ||
           api_name == "Trunc" ||
           api_name == "trunc";
}

bool isSupportedOperatorAPI(const clang::CXXOperatorCallExpr* expr,
                            std::string& api_name) {
    if (!expr) return false;
    switch (expr->getOperator()) {
    case clang::OO_Plus: api_name = "+"; return true;
    case clang::OO_Minus: api_name = "-"; return true;
    case clang::OO_Star: api_name = "*"; return true;
    case clang::OO_Slash: api_name = "/"; return true;
    case clang::OO_Percent: api_name = "%"; return true;
    case clang::OO_Amp: api_name = "&"; return true;
    case clang::OO_Pipe: api_name = "|"; return true;
    case clang::OO_Caret: api_name = "^"; return true;
    case clang::OO_Tilde: api_name = "~"; return true;
    case clang::OO_Exclaim: api_name = "!"; return true;
    case clang::OO_EqualEqual: api_name = "=="; return true;
    case clang::OO_ExclaimEqual: api_name = "!="; return true;
    case clang::OO_Less: api_name = "<"; return true;
    case clang::OO_Greater: api_name = ">"; return true;
    case clang::OO_LessEqual: api_name = "<="; return true;
    case clang::OO_GreaterEqual: api_name = ">="; return true;
    case clang::OO_LessLess: api_name = "<<"; return true;
    case clang::OO_GreaterGreater: api_name = ">>"; return true;
    case clang::OO_AmpAmp: api_name = "&&"; return true;
    case clang::OO_PipePipe: api_name = "||"; return true;
    case clang::OO_Subscript: api_name = "[]"; return true;
    default: return false;
    }
}

bool isSTLRuntimeCall(const clang::FunctionDecl* function) {
    if (!function) return false;
    std::string qualified = declName(function);
    return qualified.rfind("std::", 0) == 0 ||
           qualified.find("::std::") != std::string::npos;
}

CallBindingResult unsupportedCall(const ExprBuildContext& context,
                                  const clang::CallExpr* expr,
                                  std::string message) {
    CallBindingResult result;
    BoundCall call;
    call.kind = BoundCallKind::Unsupported;
    call.loc = exprLoc(context, expr);
    call.callee_decl = directCallee(expr);
    if (call.callee_decl) {
        call.stable_callee_name = declName(call.callee_decl);
    }
    result.call = std::move(call);
    result.diagnostics.push_back(makeError(context, result.call->loc,
                                           std::move(message)));
    return result;
}

CallBindingResult bindReachableOrAPI(const ExprBuildContext& context,
                                     const clang::CallExpr* expr) {
    CallBindingResult result;
    if (!expr) {
        result.diagnostics.push_back(makeError(
            context, {}, "Cannot bind null call expression"));
        return result;
    }

    DebugLoc loc = exprLoc(context, expr);
    const clang::FunctionDecl* callee = directCallee(expr);
    const FunctionEntity* entity =
        reachableFunctionForDecl(context.reachability, callee);

    if (entity) {
        BoundCall call;
        call.kind = boundKindForEntity(*entity, expr);
        call.stable_callee_name = entity->key.stable_name;
        call.function_id = entity->id;
        call.template_values = templateValuesForFunction(
            context, callee, entity, loc, result.diagnostics);
        if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
            call.receiver_expr = member->getImplicitObjectArgument();
        }
        call.callee_decl = callee;
        call.loc = loc;
        if (hasError(result.diagnostics)) return result;
        result.call = std::move(call);
        return result;
    }

    if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
        std::string api_name;
        if (isSupportedFixintMemberAPI(member, api_name)) {
            BoundCall call;
            call.kind = BoundCallKind::FixintAPI;
            call.stable_callee_name = api_name;
            call.api_name = api_name;
            call.template_values = templateValuesForFunction(
                context, callee, nullptr, loc, result.diagnostics);
            call.receiver_expr = member->getImplicitObjectArgument();
            call.callee_decl = callee;
            call.loc = loc;
            if (hasError(result.diagnostics)) return result;
            result.call = std::move(call);
            return result;
        }
    }

    std::string free_api;
    if (isSupportedRTLZZFreeAPI(callee, free_api)) {
        BoundCall call;
        call.kind = BoundCallKind::FixintAPI;
        call.stable_callee_name = free_api;
        call.api_name = free_api;
        call.template_values = templateValuesForFunction(
            context, callee, nullptr, loc, result.diagnostics);
        call.callee_decl = callee;
        call.loc = loc;
        if (hasError(result.diagnostics)) return result;
        result.call = std::move(call);
        return result;
    }

    if (isSTLRuntimeCall(callee)) {
        return unsupportedCall(
            context, expr,
            "Unsupported C++ subset feature: STL runtime call '" +
                declName(callee) + "'");
    }

    if (!callee) {
        return unsupportedCall(context, expr, "Unable to resolve call callee");
    }
    return unsupportedCall(context, expr,
                           "Unknown function call '" + declName(callee) + "'");
}

} // namespace

CallBindingResult bindCallExpr(const ExprBuildContext& context,
                               const clang::CallExpr* expr) {
    return bindReachableOrAPI(context, expr);
}

CallBindingResult bindCXXMemberCallExpr(const ExprBuildContext& context,
                                        const clang::CXXMemberCallExpr* expr) {
    return bindReachableOrAPI(context, expr);
}

CallBindingResult bindCXXOperatorCallExpr(const ExprBuildContext& context,
                                          const clang::CXXOperatorCallExpr* expr) {
    CallBindingResult result = bindReachableOrAPI(context, expr);
    if (result.call && result.call->kind != BoundCallKind::Unsupported) {
        return result;
    }

    std::string api_name;
    if (isSupportedOperatorAPI(expr, api_name)) {
        result.diagnostics.clear();
        BoundCall call;
        call.kind = BoundCallKind::FixintAPI;
        call.stable_callee_name = api_name;
        call.api_name = api_name;
        call.callee_decl = directCallee(expr);
        call.loc = exprLoc(context, expr);
        result.call = std::move(call);
    }
    return result;
}

} // namespace pred::s0clang18
