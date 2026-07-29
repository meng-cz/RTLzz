#include "s0clang18/s012expr.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclTemplate.h>
#include <llvm/ADT/SmallString.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const ExprBuildContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.12";
    if (context.session) diagnostic.context.source_file = context.session->main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

DebugLoc exprLoc(const ExprBuildContext& context, const clang::Expr* expr) {
    if (!context.session || !expr) return {};
    return debugLocForRange(*context.session, expr->getSourceRange(),
                            context.loc_policy);
}

std::string declName(const clang::Decl* decl) {
    if (const auto* named = llvm::dyn_cast_or_null<clang::NamedDecl>(decl)) {
        std::string qualified = named->getQualifiedNameAsString();
        if (!qualified.empty()) return qualified;
        return named->getNameAsString();
    }
    return {};
}

std::string unqualifiedName(std::string name) {
    auto scope = name.rfind("::");
    if (scope != std::string::npos) name = name.substr(scope + 2);
    return name;
}

std::string unqualifiedName(const clang::NamedDecl* decl) {
    return decl ? unqualifiedName(decl->getNameAsString()) : std::string{};
}

std::string canonicalApiName(std::string name) {
    name = unqualifiedName(std::move(name));
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

const clang::FunctionDecl* canonicalFunction(const clang::FunctionDecl* decl) {
    if (!decl) return nullptr;
    return llvm::dyn_cast<clang::FunctionDecl>(decl->getCanonicalDecl());
}

pred::v2::TypeInfo unknownTypeFor(clang::QualType type) {
    return pred::v2::make_unknown_type(type.isNull() ? "" : type.getAsString());
}

pred::v2::TypeInfo exprType(const ExprBuildContext& context,
                            const clang::Expr* expr,
                            DebugLoc loc) {
    if (!expr) return {};
    if (!context.type_context) return unknownTypeFor(expr->getType());
    auto lowered = lowerQualType(*context.type_context, expr->getType(), loc);
    if (!lowered.ok() || !lowered.value) return unknownTypeFor(expr->getType());
    return lowered.value->type;
}

void appendDiagnostics(ExprBuildResult& out, const ExprBuildResult& in) {
    out.diagnostics.insert(out.diagnostics.end(),
                           in.diagnostics.begin(), in.diagnostics.end());
}

void appendDiagnostics(ExprBuildResult& out,
                       const std::vector<Diagnostic>& diagnostics) {
    out.diagnostics.insert(out.diagnostics.end(),
                           diagnostics.begin(), diagnostics.end());
}

ExprBuildResult failExpr(const ExprBuildContext& context,
                         const clang::Expr* expr,
                         std::string message) {
    ExprBuildResult result;
    result.diagnostics.push_back(makeError(context, exprLoc(context, expr),
                                           std::move(message)));
    return result;
}

pred::v2::ExprPtr attachLoc(pred::v2::ExprPtr expr, DebugLoc loc) {
    if (expr) expr->debug_loc = std::move(loc);
    return expr;
}

std::string integerLiteralText(const clang::IntegerLiteral* literal) {
    if (!literal) return {};
    const llvm::APInt& value = literal->getValue();
    llvm::SmallString<64> text;
    if (literal->getType()->isSignedIntegerType()) {
        value.toString(text, 10, true);
    } else {
        value.toString(text, 10, false);
    }
    return std::string(text.str());
}

std::string apIntText(const llvm::APSInt& value) {
    llvm::SmallString<64> text;
    value.toString(text, 10);
    return std::string(text.str());
}

std::string apIntText(const llvm::APInt& value, bool is_signed) {
    llvm::SmallString<64> text;
    value.toString(text, 10, is_signed);
    return std::string(text.str());
}

std::optional<long long> evalTemplateArgValue(const ExprBuildContext& context,
                                              const clang::TemplateArgument& arg,
                                              DebugLoc loc,
                                              std::vector<Diagnostic>& diagnostics) {
    if (arg.getKind() != clang::TemplateArgument::Integral || !context.const_eval) {
        return std::nullopt;
    }
    auto value = evalTemplateIntegralArgument(*context.const_eval, arg, loc);
    diagnostics.insert(diagnostics.end(),
                       value.diagnostics.begin(), value.diagnostics.end());
    if (!value.value) return std::nullopt;
    return *value.value;
}

std::vector<long long> templateValuesForFunction(
    const ExprBuildContext& context,
    const clang::FunctionDecl* function,
    DebugLoc loc,
    std::vector<Diagnostic>& diagnostics) {
    std::vector<long long> values;
    const clang::TemplateArgumentList* args =
        function ? function->getTemplateSpecializationArgs() : nullptr;
    if (!args) return values;
    for (const clang::TemplateArgument& arg : args->asArray()) {
        if (arg.getKind() == clang::TemplateArgument::Pack) {
            for (const clang::TemplateArgument& packed : arg.pack_elements()) {
                if (auto value = evalTemplateArgValue(
                        context, packed, loc, diagnostics)) {
                    values.push_back(*value);
                }
            }
            continue;
        }
        if (auto value = evalTemplateArgValue(context, arg, loc, diagnostics)) {
            values.push_back(*value);
        }
    }
    return values;
}

bool isFixintRecord(clang::QualType type) {
    type = type.getCanonicalType();
    if (type->isReferenceType()) type = type->getPointeeType().getCanonicalType();
    const auto* record = type->getAsCXXRecordDecl();
    if (!record) return false;
    std::string name = record->getQualifiedNameAsString();
    return name == "vulfixint::Int" ||
           name == "vulfixint::IntSliceRef" ||
           name == "vulfixint::IntConstSliceRef" ||
           name == "vulfixint::IntBitRef" ||
           name == "vulfixint::IntConstBitRef" ||
           name == "vulfixint::IntSignedView";
}

bool isFixintMemberCall(const clang::CXXMemberCallExpr* call) {
    if (!call) return false;
    const clang::Expr* receiver = call->getImplicitObjectArgument();
    return receiver && isFixintRecord(receiver->getType());
}

pred::v2::TypeInfo signedViewType(pred::v2::TypeInfo base) {
    if (base.width <= 0) base.width = 1;
    base.is_signed = true;
    base.is_hw_int = true;
    base.hw_kind = "signed_view";
    base.name = "IntSignedView<" + std::to_string(base.width) + ">";
    return base;
}

pred::v2::ExprPtr makeSurfaceCall(const std::string& callee,
                                  pred::v2::TypeInfo type,
                                  std::vector<pred::v2::ExprPtr> args,
                                  DebugLoc loc) {
    auto out = std::make_shared<pred::v2::Expr>();
    out->kind = pred::v2::ExprKind::Call;
    out->callee = callee;
    out->type = pred::v2::canonicalize_bool_type(std::move(type));
    out->args = std::move(args);
    out->debug_loc = std::move(loc);
    return out;
}

bool isFileScopeVarDecl(const clang::Decl* decl) {
    const auto* var = llvm::dyn_cast_or_null<clang::VarDecl>(decl);
    if (!var) return false;
    if (var->isStaticLocal()) return false;
    const clang::DeclContext* context = var->getDeclContext();
    return context && (context->isTranslationUnit() || context->isNamespace());
}

std::optional<std::string> evaluateIntegerLiteralText(
    const ExprBuildContext& context,
    const clang::Expr* expr) {
    if (!context.session || !context.session->ast_context || !expr) {
        return std::nullopt;
    }
    clang::Expr::EvalResult eval;
    if (!expr->EvaluateAsInt(eval, *context.session->ast_context)) {
        return std::nullopt;
    }
    return apIntText(eval.Val.getInt());
}

std::string calleeNameFromFunction(const FunctionReachabilityGraph* reachability,
                                   const clang::FunctionDecl* function) {
    if (!function) return {};
    if (reachability) {
        auto found = reachability->function_by_decl.find(function);
        if (found == reachability->function_by_decl.end()) {
            const clang::FunctionDecl* canonical = canonicalFunction(function);
            if (canonical) found = reachability->function_by_decl.find(canonical);
        }
        if (found != reachability->function_by_decl.end()) {
            const FunctionEntity* entity = findFunctionEntity(*reachability, found->second);
            if (entity && !entity->key.stable_name.empty()) {
                return entity->key.stable_name;
            }
        }
    }
    return declName(function);
}

const FunctionEntity* functionEntityFromFunction(
    const FunctionReachabilityGraph* reachability,
    const clang::FunctionDecl* function) {
    if (!reachability || !function) return nullptr;
    auto found = reachability->function_by_decl.find(function);
    if (found == reachability->function_by_decl.end()) {
        const clang::FunctionDecl* canonical = canonicalFunction(function);
        if (canonical) found = reachability->function_by_decl.find(canonical);
    }
    if (found == reachability->function_by_decl.end()) return nullptr;
    return findFunctionEntity(*reachability, found->second);
}

const FunctionEntity* lambdaEntityFromMethod(
    const ExprBuildContext& context,
    const clang::CXXMethodDecl* method) {
    if (!context.lambdas || !context.reachability || !method) return nullptr;
    const clang::FunctionDecl* canonical = canonicalFunction(method);
    for (const LambdaInfo& lambda : context.lambdas->lambdas) {
        if (!lambda.call_operator) continue;
        if (canonicalFunction(lambda.call_operator) == canonical) {
            return findFunctionEntity(*context.reachability, lambda.function_id);
        }
    }
    return nullptr;
}

bool isLambdaEntity(const FunctionEntity* entity) {
    return entity &&
           (entity->kind == FunctionEntityKind::Lambda ||
            entity->kind == FunctionEntityKind::GenericLambdaSpecialization);
}

bool isLambdaObjectType(const pred::v2::TypeInfo& type) {
    return type.name.find("(lambda") != std::string::npos ||
           type.struct_name.find("(lambda") != std::string::npos;
}

std::string calleeName(const ExprBuildContext& context,
                       const clang::CallExpr* call) {
    if (!call) return {};
    if (const clang::FunctionDecl* direct = call->getDirectCallee()) {
        return calleeNameFromFunction(context.reachability, direct);
    }
    if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
        if (const clang::CXXMethodDecl* method = member->getMethodDecl()) {
            return calleeNameFromFunction(context.reachability, method);
        }
    }
    const clang::Expr* callee = call->getCallee();
    if (!callee) return {};
    callee = callee->IgnoreParenImpCasts();
    if (const auto* decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(callee)) {
        return declName(decl_ref->getDecl());
    }
    if (const auto* member_expr = llvm::dyn_cast<clang::MemberExpr>(callee)) {
        return declName(member_expr->getMemberDecl());
    }
    return {};
}

std::string operatorName(clang::OverloadedOperatorKind op) {
    switch (op) {
    case clang::OO_Plus: return "+";
    case clang::OO_Minus: return "-";
    case clang::OO_Star: return "*";
    case clang::OO_Slash: return "/";
    case clang::OO_Percent: return "%";
    case clang::OO_Amp: return "&";
    case clang::OO_Pipe: return "|";
    case clang::OO_Caret: return "^";
    case clang::OO_Tilde: return "~";
    case clang::OO_Exclaim: return "!";
    case clang::OO_EqualEqual: return "==";
    case clang::OO_ExclaimEqual: return "!=";
    case clang::OO_Less: return "<";
    case clang::OO_Greater: return ">";
    case clang::OO_LessEqual: return "<=";
    case clang::OO_GreaterEqual: return ">=";
    case clang::OO_LessLess: return "<<";
    case clang::OO_GreaterGreater: return ">>";
    case clang::OO_AmpAmp: return "&&";
    case clang::OO_PipePipe: return "||";
    case clang::OO_PlusPlus: return "++";
    case clang::OO_MinusMinus: return "--";
    case clang::OO_Subscript: return "[]";
    case clang::OO_Call: return "operator()";
    default: return {};
    }
}

ExprBuildResult buildExprImpl(const ExprBuildContext& context,
                              const clang::Expr* expr,
                              bool lvalue);

ExprBuildResult buildChild(const ExprBuildContext& context,
                           const clang::Expr* expr) {
    return buildExprImpl(context, expr, false);
}

ExprBuildResult buildCallExpr(const ExprBuildContext& context,
                              const clang::CallExpr* call) {
    ExprBuildResult result;
    auto out = std::make_shared<pred::v2::Expr>();
    out->kind = pred::v2::ExprKind::Call;
    out->callee = calleeName(context, call);
    out->type = exprType(context, call, exprLoc(context, call));
    out->debug_loc = exprLoc(context, call);
    const FunctionEntity* entity =
        functionEntityFromFunction(context.reachability,
                                   call ? call->getDirectCallee() : nullptr);
    if (!entity) {
        if (const auto* member = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(call)) {
            entity = functionEntityFromFunction(context.reachability,
                                                member->getMethodDecl());
            if (!entity) entity = lambdaEntityFromMethod(context, member->getMethodDecl());
        }
    }
    if (!entity) {
        if (const auto* op_call = llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(call)) {
            if (op_call->getOperator() == clang::OO_Call) {
                if (const auto* method =
                        llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
                            op_call->getDirectCallee())) {
                    entity = lambdaEntityFromMethod(context, method);
                }
            }
        }
    }
    if (entity && !entity->key.stable_name.empty()) {
        out->callee = entity->key.stable_name;
    }

    if (out->callee.empty()) {
        result.diagnostics.push_back(makeError(
            context, out->debug_loc, "Unable to resolve call callee"));
    }

    const bool lambda_call =
        isLambdaEntity(entity) ||
        out->callee.rfind("lambda_", 0) == 0 ||
        out->callee.rfind("generic_lambda_", 0) == 0;

    if (isLambdaEntity(entity) && context.lambdas) {
        if (const LambdaInfo* lambda = findLambdaInfo(*context.lambdas, entity->id)) {
            for (const LambdaCapture& capture : lambda->captures) {
                if (capture.kind == LambdaCaptureKind::This) continue;
                if (isLambdaObjectType(capture.type)) continue;
                auto arg = pred::v2::make_var(capture.source_name, capture.type);
                arg->debug_loc = capture.loc.valid() ? capture.loc : out->debug_loc;
                out->args.push_back(std::move(arg));
            }
        }
    }

    if (!lambda_call) {
    if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
        if (const clang::Expr* receiver = member->getImplicitObjectArgument()) {
            auto built_receiver = buildChild(context, receiver);
            appendDiagnostics(result, built_receiver);
            if (built_receiver.expr) out->args.push_back(std::move(built_receiver.expr));
        }
    }
    }

    unsigned arg_index = 0;
    unsigned arg_start = 0;
    if (lambda_call) {
        if (const auto* op_call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call)) {
            if (op_call->getOperator() == clang::OO_Call &&
                op_call->getNumArgs() > 0) {
                arg_start = 1;
            }
        }
    }
    for (const clang::Expr* arg : call->arguments()) {
        if (arg_index++ < arg_start) continue;
        auto built = buildChild(context, arg);
        appendDiagnostics(result, built);
        if (built.expr) out->args.push_back(std::move(built.expr));
    }
    out->args.erase(std::remove_if(out->args.begin(), out->args.end(),
                                   [](const pred::v2::ExprPtr& arg) {
                                       return arg && isLambdaObjectType(arg->type);
                                   }),
                    out->args.end());

    if (hasError(result.diagnostics)) return result;
    result.expr = std::move(out);
    return result;
}

pred::v2::ExprPtr makeCast(pred::v2::ExprPtr child,
                           pred::v2::TypeInfo type,
                           DebugLoc loc);

ExprBuildResult buildFixintMemberCallExpr(const ExprBuildContext& context,
                                          const clang::CXXMemberCallExpr* call) {
    ExprBuildResult result;
    if (!call || !isFixintMemberCall(call)) return result;
    const clang::CXXMethodDecl* method = call->getMethodDecl();
    DebugLoc loc = exprLoc(context, call);
    if (llvm::isa_and_nonnull<clang::CXXConversionDecl>(method)) {
        auto receiver = buildChild(context, call->getImplicitObjectArgument());
        appendDiagnostics(result, receiver);
        if (hasError(result.diagnostics)) return result;
        pred::v2::TypeInfo target = exprType(context, call, loc);
        if (receiver.expr && target.width > 0 &&
            receiver.expr->type.width != target.width) {
            result.expr = makeCast(std::move(receiver.expr), target, loc);
        } else {
            result.expr = std::move(receiver.expr);
            if (result.expr && target.width > 0) result.expr->type = target;
        }
        return result;
    }

    std::string api = canonicalApiName(unqualifiedName(method));
    if (api != "at" && api != "pick" && api != "to" && api != "sint") {
        return result;
    }

    auto receiver = buildChild(context, call->getImplicitObjectArgument());
    appendDiagnostics(result, receiver);
    if (hasError(result.diagnostics)) return result;

    std::vector<long long> template_values = templateValuesForFunction(
        context, method, loc, result.diagnostics);
    if (hasError(result.diagnostics)) return result;

    std::vector<pred::v2::ExprPtr> args;
    if (receiver.expr) args.push_back(std::move(receiver.expr));
    for (const clang::Expr* arg : call->arguments()) {
        auto built = buildChild(context, arg);
        appendDiagnostics(result, built);
        if (built.expr) args.push_back(std::move(built.expr));
    }
    if (hasError(result.diagnostics)) return result;

    pred::v2::TypeInfo type = exprType(context, call, loc);
    if (api == "sint" && !args.empty()) {
        type = signedViewType(args.front()->type);
        result.expr = makeSurfaceCall("sint", std::move(type), std::move(args), loc);
        return result;
    }
    if (api == "at") {
        if (template_values.size() == 1) {
            template_values.push_back(template_values.front());
        }
        if (template_values.size() != 2) {
            result.diagnostics.push_back(makeError(
                context, loc, "S0Clang18 cannot resolve Int::at template indices"));
            return result;
        }
        int hi = static_cast<int>(template_values[0]);
        int lo = static_cast<int>(template_values[1]);
        if (hi < lo || lo < 0) {
            result.diagnostics.push_back(makeError(
                context, loc, "S0Clang18 Int::at has invalid template indices"));
            return result;
        }
        type = pred::v2::make_hw_type("Int", hi - lo + 1, false);
        if (hi == lo) type = pred::v2::make_bool_type();
        auto out = makeSurfaceCall("at", std::move(type), std::move(args), loc);
        out->hi = hi;
        out->lo = lo;
        result.expr = std::move(out);
        return result;
    }
    if (api == "pick") {
        if (!template_values.empty()) {
            type = pred::v2::make_hw_type(
                "Int", static_cast<int>(template_values.front()), false);
        } else {
            type = pred::v2::make_bool_type();
        }
        auto out = makeSurfaceCall(template_values.empty() ? "bit_at" : "pick",
                                   std::move(type), std::move(args), loc);
        if (!template_values.empty()) {
            out->to_width = static_cast<int>(template_values.front());
        }
        result.expr = std::move(out);
        return result;
    }
    if (api == "to") {
        result.expr = makeSurfaceCall("to", std::move(type), std::move(args), loc);
        return result;
    }
    return result;
}

bool isFixintFreeAPI(const clang::FunctionDecl* callee, std::string& api) {
    api = canonicalApiName(unqualifiedName(callee));
    return api == "Cat" || api == "cat" || api == "concat" ||
           api == "Repeat" || api == "repeat" ||
           api == "ReduceOr" || api == "reduce_or" ||
           api == "ReduceAnd" || api == "reduce_and" ||
           api == "ReduceXor" || api == "reduce_xor" ||
           api == "ZExt" || api == "zext" ||
           api == "Trunc" || api == "trunc";
}

ExprBuildResult buildFixintFreeCallExpr(const ExprBuildContext& context,
                                        const clang::CallExpr* call) {
    ExprBuildResult result;
    std::string api;
    if (!isFixintFreeAPI(call ? call->getDirectCallee() : nullptr, api)) {
        return result;
    }
    DebugLoc loc = exprLoc(context, call);
    std::vector<pred::v2::ExprPtr> args;
    for (const clang::Expr* arg : call->arguments()) {
        auto built = buildChild(context, arg);
        appendDiagnostics(result, built);
        if (built.expr) args.push_back(std::move(built.expr));
    }
    if (hasError(result.diagnostics)) return result;

    auto out = makeSurfaceCall(api, exprType(context, call, loc), std::move(args), loc);
    std::vector<long long> template_values = templateValuesForFunction(
        context, call->getDirectCallee(), loc, result.diagnostics);
    if (!template_values.empty()) {
        out->times = static_cast<int>(template_values.front());
        out->to_width = static_cast<int>(template_values.front());
    }
    if (hasError(result.diagnostics)) return result;
    result.expr = std::move(out);
    return result;
}

ExprBuildResult buildOperatorCallExpr(const ExprBuildContext& context,
                                      const clang::CXXOperatorCallExpr* call) {
    if (!call) return failExpr(context, call, "Cannot build null operator call");
    std::string op = operatorName(call->getOperator());
    if (op.empty()) return buildCallExpr(context, call);
    if (op == "operator()") return buildCallExpr(context, call);

    if (op == "[]" && call->getNumArgs() == 2) {
        auto base = buildChild(context, call->getArg(0));
        auto index = buildChild(context, call->getArg(1));
        ExprBuildResult result;
        appendDiagnostics(result, base);
        appendDiagnostics(result, index);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_array_access(
                                    std::move(base.expr), std::move(index.expr),
                                    exprType(context, call, exprLoc(context, call))),
                                exprLoc(context, call));
        return result;
    }

    if (call->getNumArgs() == 1) {
        auto operand = buildChild(context, call->getArg(0));
        ExprBuildResult result;
        appendDiagnostics(result, operand);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_unary(
                                    op, std::move(operand.expr),
                                    exprType(context, call, exprLoc(context, call))),
                                exprLoc(context, call));
        return result;
    }

    if (call->getNumArgs() == 2) {
        auto lhs = buildChild(context, call->getArg(0));
        auto rhs = buildChild(context, call->getArg(1));
        ExprBuildResult result;
        appendDiagnostics(result, lhs);
        appendDiagnostics(result, rhs);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_binary(
                                    op, std::move(lhs.expr), std::move(rhs.expr),
                                    exprType(context, call, exprLoc(context, call))),
                                exprLoc(context, call));
        return result;
    }

    return buildCallExpr(context, call);
}

pred::v2::ExprPtr makeCast(pred::v2::ExprPtr child,
                           pred::v2::TypeInfo type,
                           DebugLoc loc) {
    auto out = std::make_shared<pred::v2::Expr>();
    out->kind = pred::v2::ExprKind::Cast;
    out->cast_type = type;
    out->type = pred::v2::canonicalize_bool_type(std::move(type));
    out->cast_expr = std::move(child);
    out->debug_loc = std::move(loc);
    return out;
}

bool isSurfaceImplicitCast(clang::CastKind kind) {
    switch (kind) {
    case clang::CK_IntegralCast:
    case clang::CK_IntegralToBoolean:
    case clang::CK_BooleanToSignedIntegral:
        return true;
    default:
        return false;
    }
}

ExprBuildResult buildExprImpl(const ExprBuildContext& context,
                              const clang::Expr* expr,
                              bool lvalue) {
    if (!expr) return failExpr(context, expr, "Cannot build null expression");
    DebugLoc loc = exprLoc(context, expr);

    if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expr)) {
        return buildExprImpl(context, cleanup->getSubExpr(), lvalue);
    }
    if (const auto* materialized = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr)) {
        return buildExprImpl(context, materialized->getSubExpr(), lvalue);
    }
    if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
        return buildExprImpl(context, paren->getSubExpr(), lvalue);
    }
    if (const auto* bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr)) {
        return buildExprImpl(context, bind->getSubExpr(), lvalue);
    }
    if (const auto* implicit = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
        auto built = buildExprImpl(context, implicit->getSubExpr(), lvalue);
        if (lvalue || hasError(built.diagnostics) || !built.expr ||
            !isSurfaceImplicitCast(implicit->getCastKind())) {
            return built;
        }
        pred::v2::TypeInfo target = exprType(context, expr, loc);
        if (built.expr->kind == pred::v2::ExprKind::Literal) {
            if (built.expr->literal_value == "true") {
                built.expr->literal_value = "1";
            } else if (built.expr->literal_value == "false") {
                built.expr->literal_value = "0";
            }
            built.expr->type = pred::v2::canonicalize_bool_type(std::move(target));
            return built;
        }
        if (target.width > 0 &&
            (built.expr->type.width != target.width ||
             built.expr->type.hw_kind != target.hw_kind ||
             built.expr->type.is_signed != target.is_signed)) {
            built.expr = makeCast(std::move(built.expr), std::move(target), loc);
        }
        return built;
    }
    if (const auto* constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
        return buildExprImpl(context, constant->getSubExpr(), lvalue);
    }

    if (const auto* literal = llvm::dyn_cast<clang::IntegerLiteral>(expr)) {
        ExprBuildResult result;
        result.expr = attachLoc(pred::v2::make_literal(
                                    integerLiteralText(literal),
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }
    if (const auto* literal = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(expr)) {
        ExprBuildResult result;
        result.expr = attachLoc(pred::v2::make_literal(
                                    literal->getValue() ? "true" : "false",
                                    pred::v2::make_bool_type()),
                                loc);
        return result;
    }

    if (llvm::isa<clang::SubstNonTypeTemplateParmExpr>(expr)) {
        if (std::optional<std::string> value =
                evaluateIntegerLiteralText(context, expr)) {
            ExprBuildResult result;
            result.expr = attachLoc(pred::v2::make_literal(
                                        *value, exprType(context, expr, loc)),
                                    loc);
            return result;
        }
    }

    if (const auto* decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
        ExprBuildResult result;
        if (const auto* enum_constant =
                llvm::dyn_cast_or_null<clang::EnumConstantDecl>(
                    decl_ref->getDecl())) {
            result.expr = attachLoc(pred::v2::make_literal(
                                        apIntText(enum_constant->getInitVal()),
                                        exprType(context, expr, loc)),
                                    loc);
            return result;
        }
        if (std::optional<std::string> value =
                evaluateIntegerLiteralText(context, expr)) {
            result.expr = attachLoc(pred::v2::make_literal(
                                        *value, exprType(context, expr, loc)),
                                    loc);
            return result;
        }
        result.expr = attachLoc(pred::v2::make_var(declName(decl_ref->getDecl()),
                                                   exprType(context, expr, loc)),
                                loc);
        if (result.expr && isFileScopeVarDecl(decl_ref->getDecl())) {
            result.expr->global_port_name = declName(decl_ref->getDecl());
        }
        return result;
    }

    if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
        auto base = buildExprImpl(context, member->getBase(), true);
        ExprBuildResult result;
        appendDiagnostics(result, base);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_field_access(
                                    std::move(base.expr),
                                    member->getMemberDecl()
                                        ? member->getMemberDecl()->getNameAsString()
                                        : "",
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }

    if (const auto* subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
        auto base = buildExprImpl(context, subscript->getBase(), true);
        auto index = buildChild(context, subscript->getIdx());
        ExprBuildResult result;
        appendDiagnostics(result, base);
        appendDiagnostics(result, index);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_array_access(
                                    std::move(base.expr), std::move(index.expr),
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }

    if (const auto* op_call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr)) {
        return buildOperatorCallExpr(context, op_call);
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
        auto lhs = buildChild(context, binary->getLHS());
        auto rhs = buildChild(context, binary->getRHS());
        ExprBuildResult result;
        appendDiagnostics(result, lhs);
        appendDiagnostics(result, rhs);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_binary(
                                    binaryOpcodeForClangExpr(binary),
                                    std::move(lhs.expr), std::move(rhs.expr),
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
        auto operand = buildChild(context, unary->getSubExpr());
        ExprBuildResult result;
        appendDiagnostics(result, operand);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_unary(
                                    unaryOpcodeForClangExpr(unary),
                                    std::move(operand.expr),
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }

    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
        auto cond = buildChild(context, conditional->getCond());
        auto then_expr = buildChild(context, conditional->getTrueExpr());
        auto else_expr = buildChild(context, conditional->getFalseExpr());
        ExprBuildResult result;
        appendDiagnostics(result, cond);
        appendDiagnostics(result, then_expr);
        appendDiagnostics(result, else_expr);
        if (hasError(result.diagnostics)) return result;
        result.expr = attachLoc(pred::v2::make_ternary(
                                    std::move(cond.expr), std::move(then_expr.expr),
                                    std::move(else_expr.expr),
                                    exprType(context, expr, loc)),
                                loc);
        return result;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expr)) {
        if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call)) {
            auto fixint = buildFixintMemberCallExpr(context, member);
            if (fixint.expr || !fixint.diagnostics.empty()) return fixint;
        }
        auto fixint = buildFixintFreeCallExpr(context, call);
        if (fixint.expr || !fixint.diagnostics.empty()) return fixint;
        return buildCallExpr(context, call);
    }

    if (const auto* cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr)) {
        auto child = buildChild(context, cast->getSubExpr());
        ExprBuildResult result;
        appendDiagnostics(result, child);
        if (hasError(result.diagnostics)) return result;
        result.expr = makeCast(std::move(child.expr), exprType(context, expr, loc), loc);
        return result;
    }

    if (const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(expr)) {
        ExprBuildResult result;
        auto out = std::make_shared<pred::v2::Expr>();
        out->kind = pred::v2::ExprKind::Call;
        out->callee = typeLabel(exprType(context, expr, loc));
        out->type = exprType(context, expr, loc);
        out->debug_loc = loc;
        for (const clang::Expr* arg : construct->arguments()) {
            auto built = buildChild(context, arg);
            appendDiagnostics(result, built);
            if (built.expr) out->args.push_back(std::move(built.expr));
        }
        if (hasError(result.diagnostics)) return result;
        result.expr = std::move(out);
        return result;
    }

    if (const auto* init_list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
        ExprBuildResult result;
        auto out = std::make_shared<pred::v2::Expr>();
        out->kind = pred::v2::ExprKind::Call;
        out->type = exprType(context, expr, loc);
        out->callee = typeLabel(out->type);
        out->debug_loc = loc;
        for (const clang::Expr* init : init_list->inits()) {
            auto built = buildChild(context, init);
            appendDiagnostics(result, built);
            if (built.expr) out->args.push_back(std::move(built.expr));
        }
        if (hasError(result.diagnostics)) return result;
        result.expr = std::move(out);
        return result;
    }

    (void)lvalue;
    return failExpr(context, expr,
                    "Unsupported expression kind '" +
                        std::string(expr->getStmtClassName()) + "'");
}

} // namespace

ExprBuildResult buildExpr(const ExprBuildContext& context,
                          const clang::Expr* expr) {
    return buildExprImpl(context, expr, false);
}

ExprBuildResult buildLValueExpr(const ExprBuildContext& context,
                                const clang::Expr* expr) {
    return buildExprImpl(context, expr, true);
}

std::string binaryOpcodeForClangExpr(const clang::Expr* expr) {
    if (const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(expr)) {
        switch (binary->getOpcode()) {
        case clang::BO_Add: return "+";
        case clang::BO_Sub: return "-";
        case clang::BO_Mul: return "*";
        case clang::BO_Div: return "/";
        case clang::BO_Rem: return "%";
        case clang::BO_Shl: return "<<";
        case clang::BO_Shr: return ">>";
        case clang::BO_LT: return "<";
        case clang::BO_GT: return ">";
        case clang::BO_LE: return "<=";
        case clang::BO_GE: return ">=";
        case clang::BO_EQ: return "==";
        case clang::BO_NE: return "!=";
        case clang::BO_And: return "&";
        case clang::BO_Xor: return "^";
        case clang::BO_Or: return "|";
        case clang::BO_LAnd: return "&&";
        case clang::BO_LOr: return "||";
        case clang::BO_Assign: return "=";
        case clang::BO_AddAssign: return "+=";
        case clang::BO_SubAssign: return "-=";
        case clang::BO_MulAssign: return "*=";
        case clang::BO_DivAssign: return "/=";
        case clang::BO_RemAssign: return "%=";
        case clang::BO_ShlAssign: return "<<=";
        case clang::BO_ShrAssign: return ">>=";
        case clang::BO_AndAssign: return "&=";
        case clang::BO_XorAssign: return "^=";
        case clang::BO_OrAssign: return "|=";
        case clang::BO_Comma: return ",";
        case clang::BO_PtrMemD:
        case clang::BO_PtrMemI:
            return "";
        }
    }
    if (const auto* op_call = llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(expr)) {
        return operatorName(op_call->getOperator());
    }
    return "";
}

std::string unaryOpcodeForClangExpr(const clang::Expr* expr) {
    if (const auto* unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(expr)) {
        switch (unary->getOpcode()) {
        case clang::UO_PostInc: return "post++";
        case clang::UO_PostDec: return "post--";
        case clang::UO_PreInc: return "++";
        case clang::UO_PreDec: return "--";
        case clang::UO_AddrOf: return "&";
        case clang::UO_Deref: return "*";
        case clang::UO_Plus: return "+";
        case clang::UO_Minus: return "-";
        case clang::UO_Not: return "~";
        case clang::UO_LNot: return "!";
        case clang::UO_Real:
        case clang::UO_Imag:
        case clang::UO_Extension:
        case clang::UO_Coawait:
            return "";
        }
    }
    if (const auto* op_call = llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(expr)) {
        return operatorName(op_call->getOperator());
    }
    return "";
}

} // namespace pred::s0clang18
