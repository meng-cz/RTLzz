#include "s0clang18/s007consteval.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/APValue.h>
#include <clang/AST/Expr.h>
#include <clang/AST/TemplateBase.h>

#include <algorithm>
#include <limits>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const ConstEvalContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.7";
    if (context.session) diagnostic.context.source_file = context.session->main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

clang::ASTContext* astContext(const ConstEvalContext& context) {
    return context.session ? context.session->ast_context : nullptr;
}

template <typename T>
StepResult<T> fail(const ConstEvalContext& context,
                   DebugLoc loc,
                   std::string message) {
    StepResult<T> result;
    result.diagnostics.push_back(makeError(context, std::move(loc), std::move(message)));
    return result;
}

bool fitsLongLong(const llvm::APSInt& value) {
    return value.isRepresentableByInt64();
}

StepResult<long long> apsIntToLongLong(const ConstEvalContext& context,
                                       const llvm::APSInt& value,
                                       DebugLoc loc) {
    if (!fitsLongLong(value)) {
        return fail<long long>(context, loc,
                               "constant value does not fit in signed 64-bit result");
    }
    StepResult<long long> result;
    result.value = value.getExtValue();
    return result;
}

StepResult<ConstValue> evalIntegerConstExpr(const ConstEvalContext& context,
                                            const clang::Expr* expr,
                                            DebugLoc loc) {
    clang::ASTContext* ast = astContext(context);
    if (!ast) {
        return fail<ConstValue>(context, loc,
                                "constant evaluation requires a valid ASTContext");
    }
    if (!expr) {
        return fail<ConstValue>(context, loc,
                                "cannot evaluate null expression as constant");
    }

    clang::Expr::EvalResult eval;
    if (!expr->EvaluateAsInt(eval, *ast, clang::Expr::SE_NoSideEffects,
                             true)) {
        return fail<ConstValue>(context, loc,
                                "expression is not an integer constant expression");
    }
    if (!eval.Val.isInt()) {
        return fail<ConstValue>(context, loc,
                                "constant expression did not produce an integer value");
    }

    ConstValue value;
    value.kind = ConstValue::Kind::Integer;
    value.integer = eval.Val.getInt();
    value.loc = loc;
    StepResult<ConstValue> result;
    result.value = std::move(value);
    return result;
}

StepResult<ConstValue> evalBoolConstExpr(const ConstEvalContext& context,
                                         const clang::Expr* expr,
                                         DebugLoc loc) {
    clang::ASTContext* ast = astContext(context);
    if (!ast) {
        return fail<ConstValue>(context, loc,
                                "constant evaluation requires a valid ASTContext");
    }
    if (!expr) {
        return fail<ConstValue>(context, loc,
                                "cannot evaluate null expression as bool constant");
    }

    bool bool_value = false;
    if (!expr->EvaluateAsBooleanCondition(bool_value, *ast, true)) {
        return fail<ConstValue>(context, loc,
                                "expression is not a bool constant expression");
    }

    ConstValue value;
    value.kind = ConstValue::Kind::Bool;
    value.boolean = bool_value;
    value.integer = llvm::APSInt(llvm::APInt(1, bool_value ? 1 : 0), true);
    value.loc = loc;
    StepResult<ConstValue> result;
    result.value = std::move(value);
    return result;
}

} // namespace

StepResult<ConstValue> evalConstExpr(const ConstEvalContext& context,
                                     const clang::Expr* expr,
                                     DebugLoc loc) {
    if (!context.session || !context.session->ast_context) {
        return fail<ConstValue>(context, loc,
                                "constant evaluation requires a valid Clang18Session");
    }
    if (!expr) {
        return fail<ConstValue>(context, loc,
                                "cannot evaluate null expression as constant");
    }

    if (expr->getType()->isBooleanType()) {
        return evalBoolConstExpr(context, expr, loc);
    }
    if (expr->getType()->isIntegerType() || expr->getType()->isEnumeralType()) {
        return evalIntegerConstExpr(context, expr, loc);
    }
    return fail<ConstValue>(context, loc,
                            "expression type is not a supported constant integer or bool");
}

StepResult<long long> evalIntegerExpr(const ConstEvalContext& context,
                                      const clang::Expr* expr,
                                      DebugLoc loc) {
    StepResult<ConstValue> value = evalIntegerConstExpr(context, expr, loc);
    if (!value.ok()) {
        StepResult<long long> result;
        result.diagnostics = std::move(value.diagnostics);
        return result;
    }
    return apsIntToLongLong(context, value.value->integer, loc);
}

StepResult<bool> evalBoolExpr(const ConstEvalContext& context,
                              const clang::Expr* expr,
                              DebugLoc loc) {
    StepResult<ConstValue> value = evalBoolConstExpr(context, expr, loc);
    if (!value.ok()) {
        StepResult<bool> result;
        result.diagnostics = std::move(value.diagnostics);
        return result;
    }
    StepResult<bool> result;
    result.value = value.value->boolean;
    return result;
}

StepResult<long long> evalTemplateIntegralArgument(
    const ConstEvalContext& context,
    const clang::TemplateArgument& argument,
    DebugLoc loc) {
    if (!context.session || !context.session->ast_context) {
        return fail<long long>(context, loc,
                               "template argument evaluation requires a valid Clang18Session");
    }
    if (argument.getKind() != clang::TemplateArgument::Integral) {
        return fail<long long>(context, loc,
                               "template argument is not an integral constant");
    }
    return apsIntToLongLong(context, argument.getAsIntegral(), loc);
}

} // namespace pred::s0clang18
