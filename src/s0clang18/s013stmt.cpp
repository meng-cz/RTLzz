#include "s0clang18/s013stmt.hpp"
#include "s0clang18/s014init.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/SmallString.h>

#include <algorithm>
#include <iterator>
#include <optional>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const StmtBuildContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.13";
    if (context.expr_context.session) {
        diagnostic.context.source_file =
            context.expr_context.session->main_file_path;
    }
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

DebugLoc stmtLoc(const StmtBuildContext& context, const clang::Stmt* stmt) {
    if (!context.expr_context.session || !stmt) return {};
    return debugLocForRange(*context.expr_context.session, stmt->getSourceRange(),
                            context.expr_context.loc_policy);
}

void appendDiagnostics(StmtBuildResult& out,
                       const std::vector<Diagnostic>& diagnostics) {
    out.diagnostics.insert(out.diagnostics.end(),
                           diagnostics.begin(), diagnostics.end());
}

void appendStmtResult(StmtBuildResult& out, StmtBuildResult in) {
    out.diagnostics.insert(out.diagnostics.end(),
                           in.diagnostics.begin(), in.diagnostics.end());
    out.stmts.insert(out.stmts.end(),
                     std::make_move_iterator(in.stmts.begin()),
                     std::make_move_iterator(in.stmts.end()));
}

StmtBuildResult failStmt(const StmtBuildContext& context,
                         const clang::Stmt* stmt,
                         std::string message) {
    StmtBuildResult result;
    result.diagnostics.push_back(makeError(context, stmtLoc(context, stmt),
                                           std::move(message)));
    return result;
}

pred::v2::StmtPtr makeStmt(pred::v2::StmtKind kind,
                           const StmtBuildContext& context,
                           const clang::Stmt* source) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = kind;
    stmt->debug_loc = stmtLoc(context, source);
    return stmt;
}

std::vector<pred::v2::StmtPtr> oneStmt(pred::v2::StmtPtr stmt) {
    std::vector<pred::v2::StmtPtr> out;
    if (stmt) out.push_back(std::move(stmt));
    return out;
}

std::optional<pred::v2::TypeInfo> lowerStmtType(
    const StmtBuildContext& context,
    clang::QualType type,
    DebugLoc loc,
    std::vector<Diagnostic>& diagnostics) {
    if (!context.expr_context.type_context) {
        return pred::v2::make_unknown_type(type.isNull() ? "" : type.getAsString());
    }
    auto lowered = lowerQualType(*context.expr_context.type_context, type, loc);
    if (!lowered.ok()) {
        diagnostics.insert(diagnostics.end(),
                           lowered.diagnostics.begin(), lowered.diagnostics.end());
        return std::nullopt;
    }
    return lowered.value->type;
}

bool hasSyntacticInitializer(const StmtBuildContext& context,
                             const clang::VarDecl* var) {
    if (!context.expr_context.session || !var) return var && var->hasInit();
    std::optional<SourceTextSlice> source =
        sourceTextForRange(*context.expr_context.session, var->getSourceRange());
    if (!source) return var->hasInit();
    std::size_t name_pos = source->text.find(var->getNameAsString());
    std::string tail = name_pos == std::string::npos
        ? source->text
        : source->text.substr(name_pos + var->getNameAsString().size());
    return tail.find('=') != std::string::npos ||
           tail.find('{') != std::string::npos ||
           tail.find('(') != std::string::npos;
}

const clang::Expr* ignoreTransparentExpr(const clang::Expr* expr) {
    while (expr) {
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expr)) {
            expr = cleanup->getSubExpr();
            continue;
        }
        if (const auto* materialized =
                llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr)) {
            expr = materialized->getSubExpr();
            continue;
        }
        if (const auto* bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr)) {
            expr = bind->getSubExpr();
            continue;
        }
        if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
            expr = paren->getSubExpr();
            continue;
        }
        if (const auto* implicit = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
            expr = implicit->getSubExpr();
            continue;
        }
        if (const auto* constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
            expr = constant->getSubExpr();
            continue;
        }
        break;
    }
    return expr;
}

bool isLambdaInitializer(const clang::Expr* expr) {
    return llvm::isa_and_nonnull<clang::LambdaExpr>(ignoreTransparentExpr(expr));
}

void collectIntegerInitValues(const StmtBuildContext& context,
                              const clang::Stmt* stmt,
                              std::vector<std::string>& out,
                              int depth = 0) {
    if (!stmt || depth > 8 || !context.expr_context.session ||
        !context.expr_context.session->ast_context) {
        return;
    }
    if (const auto* expr = llvm::dyn_cast<clang::Expr>(stmt)) {
        clang::Expr::EvalResult eval;
        if (expr->EvaluateAsInt(eval, *context.expr_context.session->ast_context)) {
            llvm::SmallString<64> text;
            eval.Val.getInt().toString(text, 10);
            out.push_back(std::string(text.str()));
            return;
        }
    }
    for (const clang::Stmt* child : stmt->children()) {
        collectIntegerInitValues(context, child, out, depth + 1);
    }
}

std::vector<pred::v2::StmtPtr> blockForSubStmt(const StmtBuildContext& context,
                                               const clang::Stmt* stmt,
                                               std::vector<Diagnostic>& diagnostics) {
    if (!stmt) return {};
    if (const auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
        auto built = buildCompoundStmt(context, compound);
        diagnostics.insert(diagnostics.end(),
                           built.diagnostics.begin(), built.diagnostics.end());
        return std::move(built.stmts);
    }
    auto built = buildStmt(context, stmt);
    diagnostics.insert(diagnostics.end(),
                       built.diagnostics.begin(), built.diagnostics.end());
    return std::move(built.stmts);
}

pred::v2::StmtPtr firstOrBlock(const StmtBuildContext& context,
                               const clang::Stmt* source,
                               StmtBuildResult built) {
    if (built.stmts.empty()) return nullptr;
    if (built.stmts.size() == 1) return std::move(built.stmts.front());
    auto block = makeStmt(pred::v2::StmtKind::Block, context, source);
    block->block_stmts = std::move(built.stmts);
    block->synthetic_flatten_block = true;
    return block;
}

ExprBuildResult buildExprRequired(const StmtBuildContext& context,
                                  const clang::Expr* expr,
                                  bool lvalue = false) {
    return lvalue ? buildLValueExpr(context.expr_context, expr)
                  : buildExpr(context.expr_context, expr);
}

StmtBuildResult buildDeclStmtImpl(const StmtBuildContext& context,
                                  const clang::DeclStmt* decl_stmt) {
    StmtBuildResult result;
    for (const clang::Decl* decl : decl_stmt->decls()) {
        if (llvm::isa<clang::StaticAssertDecl>(decl)) {
            continue;
        }
        const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
        if (!var) {
            result.diagnostics.push_back(makeError(
                context, stmtLoc(context, decl_stmt),
                "Unsupported declaration inside DeclStmt"));
            continue;
        }
        if (var->hasInit() && isLambdaInitializer(var->getInit())) {
            continue;
        }

        auto stmt = makeStmt(pred::v2::StmtKind::Decl, context, decl_stmt);
        stmt->decl_name = var->getNameAsString();
        DebugLoc loc = context.expr_context.session
            ? debugLocForRange(*context.expr_context.session,
                               var->getSourceRange(),
                               context.expr_context.loc_policy)
            : stmt->debug_loc;
        auto type = lowerStmtType(context, var->getType(), loc, result.diagnostics);
        if (type) stmt->decl_type = *type;
        stmt->decl_type.is_static = var->isStaticLocal();
        if (stmt->decl_type.is_static && stmt->decl_type.is_array && var->hasInit()) {
            collectIntegerInitValues(context, var->getInit(),
                                     stmt->decl_type.init_values);
        }

        if (var->hasInit() && hasSyntacticInitializer(context, var)) {
            InitBuildInput input;
            input.decl = var;
            input.init_expr = var->getInit();
            input.target_type = stmt->decl_type;
            input.loc = loc;
            auto init = buildInitializer(context.expr_context, input);
            appendDiagnostics(result, init.diagnostics);
            if (!init.init_args.empty()) {
                stmt->decl_init_args = std::move(init.init_args);
            } else if (init.init_expr) {
                stmt->decl_init = std::move(init.init_expr.value());
            }
            stmt->decl_default_constructed = init.default_constructed;
        }
        result.stmts.push_back(std::move(stmt));
    }
    return result;
}

std::string assignmentOpcode(const clang::BinaryOperator* binary) {
    if (!binary) return {};
    switch (binary->getOpcode()) {
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
    default: return {};
    }
}

std::string compoundBaseOpcode(std::string op) {
    if (!op.empty() && op.back() == '=') op.pop_back();
    return op;
}

StmtBuildResult buildAssignmentStmt(const StmtBuildContext& context,
                                    const clang::Expr* source,
                                    const clang::Expr* lhs_expr,
                                    const clang::Expr* rhs_expr,
                                    const std::string& op) {
    StmtBuildResult result;
    auto stmt = makeStmt(pred::v2::StmtKind::Assign, context, source);

    auto lhs = buildExprRequired(context, lhs_expr, true);
    auto rhs = buildExprRequired(context, rhs_expr);
    appendDiagnostics(result, lhs.diagnostics);
    appendDiagnostics(result, rhs.diagnostics);
    stmt->assign_target = std::move(lhs.expr);
    if (op == "=") {
        stmt->assign_value = std::move(rhs.expr);
    } else {
        auto lhs_value = buildExprRequired(context, lhs_expr);
        appendDiagnostics(result, lhs_value.diagnostics);
        stmt->assign_value = pred::v2::make_binary(
            compoundBaseOpcode(op), std::move(lhs_value.expr), std::move(rhs.expr),
            stmt->assign_target ? stmt->assign_target->type : pred::v2::TypeInfo{});
        if (stmt->assign_value) stmt->assign_value->debug_loc = stmt->debug_loc;
    }

    result.stmts.push_back(std::move(stmt));
    return result;
}

bool isAssignmentOperator(clang::OverloadedOperatorKind op) {
    return op == clang::OO_Equal || op == clang::OO_PlusEqual ||
           op == clang::OO_MinusEqual || op == clang::OO_StarEqual ||
           op == clang::OO_SlashEqual || op == clang::OO_PercentEqual ||
           op == clang::OO_LessLessEqual || op == clang::OO_GreaterGreaterEqual ||
           op == clang::OO_AmpEqual || op == clang::OO_CaretEqual ||
           op == clang::OO_PipeEqual;
}

const clang::Expr* memberAssignmentRhs(const clang::CXXMemberCallExpr* call) {
    if (!call || call->getNumArgs() < 1) return nullptr;
    const clang::CXXMethodDecl* method = call->getMethodDecl();
    if (!method || !isAssignmentOperator(method->getOverloadedOperator())) {
        return nullptr;
    }
    return call->getArg(0);
}

std::string assignmentOperatorToken(clang::OverloadedOperatorKind op) {
    switch (op) {
    case clang::OO_Equal: return "=";
    case clang::OO_PlusEqual: return "+=";
    case clang::OO_MinusEqual: return "-=";
    case clang::OO_StarEqual: return "*=";
    case clang::OO_SlashEqual: return "/=";
    case clang::OO_PercentEqual: return "%=";
    case clang::OO_LessLessEqual: return "<<=";
    case clang::OO_GreaterGreaterEqual: return ">>=";
    case clang::OO_AmpEqual: return "&=";
    case clang::OO_CaretEqual: return "^=";
    case clang::OO_PipeEqual: return "|=";
    default: return {};
    }
}

StmtBuildResult buildExprStatement(const StmtBuildContext& context,
                                   const clang::Expr* expr) {
    const clang::Expr* core = ignoreTransparentExpr(expr);
    if (!core) core = expr;
    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(core)) {
        std::string op = assignmentOpcode(binary);
        if (!op.empty()) {
            return buildAssignmentStmt(context, binary, binary->getLHS(),
                                       binary->getRHS(), op);
        }
    }
    if (const auto* op_call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(core)) {
        if (isAssignmentOperator(op_call->getOperator()) &&
            op_call->getNumArgs() >= 2) {
            return buildAssignmentStmt(
                context, op_call, op_call->getArg(0), op_call->getArg(1),
                assignmentOperatorToken(op_call->getOperator()));
        }
    }
    if (const auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(core)) {
        if (const clang::Expr* rhs = memberAssignmentRhs(member_call)) {
            return buildAssignmentStmt(
                context, member_call, member_call->getImplicitObjectArgument(),
                rhs,
                assignmentOperatorToken(
                    member_call->getMethodDecl()->getOverloadedOperator()));
        }
    }

    StmtBuildResult result;
    auto stmt = makeStmt(pred::v2::StmtKind::ExprStmt, context, expr);
    auto built = buildExprRequired(context, expr);
    appendDiagnostics(result, built.diagnostics);
    stmt->expr_stmt = std::move(built.expr);
    result.stmts.push_back(std::move(stmt));
    return result;
}

StmtBuildResult buildSwitchCases(const StmtBuildContext& context,
                                 const clang::Stmt* body,
                                 pred::v2::StmtPtr& switch_stmt) {
    StmtBuildResult result;
    auto startCase = [&](std::optional<pred::v2::ExprPtr> value) -> pred::v2::CaseClause& {
        pred::v2::CaseClause clause;
        clause.value = std::move(value);
        switch_stmt->switch_cases.push_back(std::move(clause));
        return switch_stmt->switch_cases.back();
    };

    pred::v2::CaseClause* current = nullptr;
    auto appendBuiltToCurrent = [&](StmtBuildResult built) {
        appendDiagnostics(result, built.diagnostics);
        if (!current) current = &startCase(std::nullopt);
        current->body.insert(current->body.end(),
                             std::make_move_iterator(built.stmts.begin()),
                             std::make_move_iterator(built.stmts.end()));
    };

    const auto* compound = llvm::dyn_cast_or_null<clang::CompoundStmt>(body);
    if (!compound) {
        result.diagnostics.push_back(makeError(
            context, stmtLoc(context, body),
            "Switch body is not a compound statement"));
        return result;
    }

    for (const clang::Stmt* child : compound->body()) {
        if (const auto* case_stmt = llvm::dyn_cast<clang::CaseStmt>(child)) {
            auto value = buildExprRequired(context, case_stmt->getLHS());
            appendDiagnostics(result, value.diagnostics);
            current = &startCase(std::move(value.expr));
            const clang::Stmt* sub = case_stmt->getSubStmt();
            if (sub && !llvm::isa<clang::NullStmt>(sub)) {
                appendBuiltToCurrent(buildStmt(context, sub));
            }
            continue;
        }
        if (const auto* default_stmt = llvm::dyn_cast<clang::DefaultStmt>(child)) {
            current = &startCase(std::nullopt);
            const clang::Stmt* sub = default_stmt->getSubStmt();
            if (sub && !llvm::isa<clang::NullStmt>(sub)) {
                appendBuiltToCurrent(buildStmt(context, sub));
            }
            continue;
        }
        if (!current) continue;
        appendBuiltToCurrent(buildStmt(context, child));
    }
    return result;
}

} // namespace

StmtBuildResult buildStmt(const StmtBuildContext& context,
                          const clang::Stmt* stmt) {
    if (!stmt) return {};

    if (const auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
        auto block = makeStmt(pred::v2::StmtKind::Block, context, stmt);
        auto built = buildCompoundStmt(context, compound);
        block->block_stmts = std::move(built.stmts);
        built.stmts = oneStmt(std::move(block));
        return built;
    }

    if (const auto* decl = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
        return buildDeclStmtImpl(context, decl);
    }

    if (const auto* expr = llvm::dyn_cast<clang::Expr>(stmt)) {
        return buildExprStatement(context, expr);
    }

    if (const auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
        std::optional<StepResult<bool>> constant_condition;
        if (context.expr_context.const_eval) {
            constant_condition = evalBoolExpr(
                *context.expr_context.const_eval,
                if_stmt->getCond(),
                stmtLoc(context, if_stmt->getCond()));
        }
        if ((constant_condition && constant_condition->value.has_value()) ||
            if_stmt->isConstexpr()) {
            if (!context.expr_context.const_eval) {
                return failStmt(context, stmt,
                                "constexpr if requires const eval context");
            }
            StmtBuildResult result;
            appendDiagnostics(result, constant_condition->diagnostics);
            if (!constant_condition->value.has_value()) {
                result.diagnostics.push_back(makeError(
                    context, stmtLoc(context, stmt),
                    "Unable to evaluate constexpr if condition"));
                return result;
            }
            const clang::Stmt* selected =
                *constant_condition->value ? if_stmt->getThen() : if_stmt->getElse();
            if (selected) {
                auto selected_result = buildStmt(context, selected);
                appendDiagnostics(result, selected_result.diagnostics);
                result.stmts = std::move(selected_result.stmts);
            }
            return result;
        }
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::If, context, stmt);
        auto cond = buildExprRequired(context, if_stmt->getCond());
        appendDiagnostics(result, cond.diagnostics);
        out->if_cond = std::move(cond.expr);
        out->if_then = blockForSubStmt(context, if_stmt->getThen(), result.diagnostics);
        out->if_else = blockForSubStmt(context, if_stmt->getElse(), result.diagnostics);
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (const auto* for_stmt = llvm::dyn_cast<clang::ForStmt>(stmt)) {
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::For, context, stmt);
        if (for_stmt->getInit()) {
            auto init = buildStmt(context, for_stmt->getInit());
            appendDiagnostics(result, init.diagnostics);
            out->for_init = firstOrBlock(
                context, for_stmt->getInit(), std::move(init));
        }
        if (for_stmt->getCond()) {
            auto cond = buildExprRequired(context, for_stmt->getCond());
            appendDiagnostics(result, cond.diagnostics);
            out->for_cond = std::move(cond.expr);
        }
        if (for_stmt->getInc()) {
            auto step = buildExprRequired(context, for_stmt->getInc());
            appendDiagnostics(result, step.diagnostics);
            out->for_step = std::move(step.expr);
        }
        out->for_body = blockForSubStmt(context, for_stmt->getBody(), result.diagnostics);
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (const auto* while_stmt = llvm::dyn_cast<clang::WhileStmt>(stmt)) {
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::While, context, stmt);
        auto cond = buildExprRequired(context, while_stmt->getCond());
        appendDiagnostics(result, cond.diagnostics);
        out->while_cond = std::move(cond.expr);
        out->while_body = blockForSubStmt(context, while_stmt->getBody(), result.diagnostics);
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (const auto* do_stmt = llvm::dyn_cast<clang::DoStmt>(stmt)) {
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::DoWhile, context, stmt);
        out->while_body = blockForSubStmt(context, do_stmt->getBody(), result.diagnostics);
        auto cond = buildExprRequired(context, do_stmt->getCond());
        appendDiagnostics(result, cond.diagnostics);
        out->while_cond = std::move(cond.expr);
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (const auto* switch_stmt = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::Switch, context, stmt);
        auto cond = buildExprRequired(context, switch_stmt->getCond());
        appendDiagnostics(result, cond.diagnostics);
        out->switch_expr = std::move(cond.expr);
        auto cases = buildSwitchCases(context, switch_stmt->getBody(), out);
        appendDiagnostics(result, cases.diagnostics);
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (llvm::isa<clang::BreakStmt>(stmt)) {
        StmtBuildResult result;
        result.stmts.push_back(makeStmt(pred::v2::StmtKind::Break, context, stmt));
        return result;
    }

    if (llvm::isa<clang::ContinueStmt>(stmt)) {
        StmtBuildResult result;
        result.stmts.push_back(makeStmt(pred::v2::StmtKind::Continue, context, stmt));
        return result;
    }

    if (const auto* return_stmt = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
        StmtBuildResult result;
        auto out = makeStmt(pred::v2::StmtKind::Return, context, stmt);
        if (return_stmt->getRetValue()) {
            auto value = buildExprRequired(context, return_stmt->getRetValue());
            appendDiagnostics(result, value.diagnostics);
            out->return_value = std::move(value.expr);
        }
        result.stmts.push_back(std::move(out));
        return result;
    }

    if (llvm::isa<clang::NullStmt>(stmt)) return {};

    return failStmt(context, stmt,
                    "Unsupported statement kind '" +
                        std::string(stmt->getStmtClassName()) + "'");
}

StmtBuildResult buildCompoundStmt(const StmtBuildContext& context,
                                  const clang::CompoundStmt* stmt) {
    StmtBuildResult result;
    if (!stmt) return result;
    for (const clang::Stmt* child : stmt->body()) {
        appendStmtResult(result, buildStmt(context, child));
    }
    return result;
}

StmtBuildResult buildFunctionBody(const StmtBuildContext& context,
                                  const clang::FunctionDecl* function_decl) {
    if (!function_decl) {
        return failStmt(context, nullptr, "Cannot build body for null function");
    }
    const clang::FunctionDecl* definition = function_decl->getDefinition();
    if (!definition) definition = function_decl;
    const auto* body = llvm::dyn_cast_or_null<clang::CompoundStmt>(
        definition->getBody());
    if (!body) {
        return failStmt(context, nullptr,
                        "Function has no compound statement body");
    }
    return buildCompoundStmt(context, body);
}

} // namespace pred::s0clang18
