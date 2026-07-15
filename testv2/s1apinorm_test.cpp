#include "ast/ASTBuilder.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"
#include "s6inline/S6Inline.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace pred;

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

static TypeInfo int8() {
    return make_hw_type("Int", 8, true);
}

static TypeInfo int4() {
    return make_hw_type("Int", 4, true);
}

static TypeInfo uint16() {
    return make_hw_type("UInt", 16, false);
}

static TypeInfo boolType() {
    return make_bool_type();
}

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static ParamDecl valueParam(const std::string& name, TypeInfo type) {
    ParamDecl out;
    out.name = name;
    out.type = std::move(type);
    out.passing = ParamPassingKind::Value;
    out.direction = ParamDirection::Input;
    return out;
}

static ParamDecl outputParam(const std::string& name, TypeInfo type) {
    ParamDecl out = valueParam(name, std::move(type));
    out.type.is_reference = true;
    out.passing = ParamPassingKind::MutableRef;
    out.direction = ParamDirection::Output;
    out.is_reference = true;
    out.is_output = true;
    return out;
}

static ExprPtr call(const std::string& callee, std::vector<ExprPtr> args, TypeInfo type) {
    auto expr = std::make_shared<Expr>();
    expr->kind = ExprKind::Call;
    expr->callee = callee;
    expr->args = std::move(args);
    expr->type = std::move(type);
    return expr;
}

static StmtPtr assign(ExprPtr target, ExprPtr value) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    return stmt;
}

static FunctionAST baseTop() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    return top;
}

static FunctionAST parseFixture(const std::string& file) {
    std::vector<std::string> clang_args = {
        "-I.",
        "-Ithird_party/vulsim/vullib",
        "-std=c++20",
    };
    auto build = buildASTFromSource(file, "hls_main", clang_args);
    if (!build.error.empty()) {
        std::cerr << "AST build failed for " << file << ":\n" << build.error << "\n";
    }
    CHECK(build.error.empty());
    CHECK(build.function.has_value());
    return std::move(build.function.value());
}

static pred::s1apinorm::S1FunctionAST normalizeS1Ok(const FunctionAST& ast) {
    pred::s1apinorm::APINormOptions options;
    options.debug_print = true;
    auto result = pred::s1apinorm::normalizeAPIs(ast, options);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.function.has_value());
    CHECK(!result.debug_text.empty());
    return std::move(result.function.value());
}

static void validateAfterS1(const FunctionAST& ast) {
    auto norm = normalizeS1Ok(ast);
    auto validation = pred::s2validate::validateFunctionAST(norm);
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());
}

static bool containsHardwareOpExpr(const pred::s1apinorm::S1ExprPtr& expr,
                                   pred::s1apinorm::S1HardwareOp op) {
    if (!expr) return false;
    if (expr->kind == pred::s1apinorm::S1ExprKind::HardwareOp &&
        expr->hardware_op == op) {
        return true;
    }
    if (containsHardwareOpExpr(expr->left, op) ||
        containsHardwareOpExpr(expr->right, op) ||
        containsHardwareOpExpr(expr->operand, op) ||
        containsHardwareOpExpr(expr->array_base, op) ||
        containsHardwareOpExpr(expr->index, op) ||
        containsHardwareOpExpr(expr->struct_base, op) ||
        containsHardwareOpExpr(expr->cast_expr, op) ||
        containsHardwareOpExpr(expr->cond, op) ||
        containsHardwareOpExpr(expr->then_expr, op) ||
        containsHardwareOpExpr(expr->else_expr, op) ||
        containsHardwareOpExpr(expr->base, op) ||
        containsHardwareOpExpr(expr->value, op)) {
        return true;
    }
    for (const auto& arg : expr->args) {
        if (containsHardwareOpExpr(arg, op)) return true;
    }
    for (const auto& part : expr->parts) {
        if (containsHardwareOpExpr(part, op)) return true;
    }
    return false;
}

static bool containsHardwareOpStmt(const pred::s1apinorm::S1StmtPtr& stmt,
                                   pred::s1apinorm::S1HardwareOp op);

static bool containsHardwareOpList(const std::vector<pred::s1apinorm::S1StmtPtr>& stmts,
                                   pred::s1apinorm::S1HardwareOp op) {
    for (const auto& stmt : stmts) {
        if (containsHardwareOpStmt(stmt, op)) return true;
    }
    return false;
}

static bool containsHardwareOpStmt(const pred::s1apinorm::S1StmtPtr& stmt,
                                   pred::s1apinorm::S1HardwareOp op) {
    if (!stmt) return false;
    if (containsHardwareOpExpr(stmt->assign_target, op) ||
        containsHardwareOpExpr(stmt->assign_value, op) ||
        containsHardwareOpExpr(stmt->if_cond, op) ||
        containsHardwareOpList(stmt->if_then, op) ||
        containsHardwareOpList(stmt->if_else, op) ||
        containsHardwareOpStmt(stmt->for_init, op) ||
        containsHardwareOpExpr(stmt->for_cond, op) ||
        containsHardwareOpExpr(stmt->for_step, op) ||
        containsHardwareOpList(stmt->for_body, op) ||
        containsHardwareOpExpr(stmt->while_cond, op) ||
        containsHardwareOpList(stmt->while_body, op) ||
        containsHardwareOpExpr(stmt->switch_expr, op) ||
        containsHardwareOpList(stmt->block_stmts, op) ||
        containsHardwareOpExpr(stmt->expr_stmt, op)) {
        return true;
    }
    if (stmt->decl_init && containsHardwareOpExpr(stmt->decl_init.value(), op)) return true;
    if (stmt->return_value && containsHardwareOpExpr(stmt->return_value.value(), op)) return true;
    for (const auto& arg : stmt->decl_init_args) {
        if (containsHardwareOpExpr(arg, op)) return true;
    }
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value && containsHardwareOpExpr(clause.value.value(), op)) return true;
        if (containsHardwareOpList(clause.body, op)) return true;
    }
    return false;
}

static bool containsS3HardwareOp(const std::vector<pred::s3statementize::S3StmtPtr>& stmts,
                                 pred::s3statementize::HardwareOp op) {
    for (const auto& stmt : stmts) {
        if (!stmt) continue;
        if (stmt->kind == pred::s3statementize::S3StmtKind::Op &&
            stmt->op.kind == pred::s3statementize::OpExpr::Kind::Hardware &&
            stmt->op.hardware_op == op) {
            return true;
        }
        if (containsS3HardwareOp(stmt->condition_prelude, op) ||
            containsS3HardwareOp(stmt->then_body, op) ||
            containsS3HardwareOp(stmt->else_body, op) ||
            containsS3HardwareOp(stmt->for_init, op) ||
            containsS3HardwareOp(stmt->for_step, op) ||
            containsS3HardwareOp(stmt->loop_body, op)) {
            return true;
        }
        for (const auto& clause : stmt->switch_cases) {
            if (containsS3HardwareOp(clause.body, op)) return true;
        }
    }
    return false;
}

static bool containsCFGHardwareOp(const pred::s4cfg::FunctionCFG& cfg,
                                  pred::s3statementize::HardwareOp op) {
    for (const auto& block : cfg.blocks) {
        if (!block) continue;
        for (const auto& stmt : block->stmts) {
            if (!stmt.stmt) continue;
            if (stmt.stmt->kind == pred::s3statementize::S3StmtKind::Op &&
                stmt.stmt->op.kind == pred::s3statementize::OpExpr::Kind::Hardware &&
                stmt.stmt->op.hardware_op == op) {
                return true;
            }
        }
    }
    return false;
}

static bool containsInlinedHardwareOp(const pred::s6inline::InlinedFunction& fn,
                                      pred::s3statementize::HardwareOp op) {
    for (const auto& block : fn.blocks) {
        if (!block) continue;
        for (const auto& stmt : block->stmts) {
            if (!stmt.stmt) continue;
            if (stmt.stmt->kind == pred::s3statementize::S3StmtKind::Op &&
                stmt.stmt->op.kind == pred::s3statementize::OpExpr::Kind::Hardware &&
                stmt.stmt->op.hardware_op == op) {
                return true;
            }
        }
    }
    return false;
}

static void pseudoSliceWriteBecomesWriteSlice() {
    auto top = baseTop();
    top.params.push_back(outputParam("n", int8()));
    top.body.push_back(assign(
        call("__slice", {make_var("n", int8()),
                         make_literal("3", TypeInfo{"int", 32, true}),
                         make_literal("0", TypeInfo{"int", 32, true})}, int4()),
        make_literal("1", int4())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body.size() == 1);
    CHECK(norm.body[0]->assign_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[0]->assign_target->var_name == "n");
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::WriteSlice);
    CHECK(norm.body[0]->assign_value->hi == 3);
    CHECK(norm.body[0]->assign_value->lo == 0);
    validateAfterS1(top);
}

static void pseudoSliceReadBecomesSlice() {
    auto top = baseTop();
    top.params.push_back(valueParam("n", int8()));
    top.params.push_back(outputParam("out", int4()));
    top.body.push_back(assign(
        make_var("out", int4()),
        call("__slice", {make_var("n", int8()),
                         make_literal("3", TypeInfo{"int", 32, true}),
                         make_literal("0", TypeInfo{"int", 32, true})}, int4())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::Slice);
    CHECK(norm.body[0]->assign_value->hi == 3);
    CHECK(norm.body[0]->assign_value->lo == 0);
    validateAfterS1(top);
}

static void residualReduceCallBecomesHardwareExpr() {
    auto top = baseTop();
    top.params.push_back(valueParam("n", int8()));
    top.params.push_back(outputParam("out", boolType()));
    top.body.push_back(assign(make_var("out", boolType()),
                              call("ReduceOr", {make_var("n", int8())}, boolType())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::ReduceOr);
    validateAfterS1(top);
}

static void dynamicRangeWriteBecomesHardwareExpr() {
    auto top = baseTop();
    top.params.push_back(outputParam("n", int8()));
    top.params.push_back(valueParam("idx", int8()));
    auto target = call("__dynamic_range_at",
                       {make_var("n", int8()), make_var("idx", int8())}, int4());
    target->intrinsic = IntrinsicKind::DynamicRangeAt;
    top.body.push_back(assign(std::move(target), make_literal("1", int4())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body[0]->assign_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::DynamicWriteSlice);
    validateAfterS1(top);
}

static void residualZExtCallBecomesHardwareExpr() {
    auto top = baseTop();
    top.params.push_back(valueParam("n", int8()));
    top.params.push_back(outputParam("out", uint16()));
    auto zext = call("zext", {make_var("n", int8())}, uint16());
    zext->to_width = 16;
    top.body.push_back(assign(make_var("out", uint16()), std::move(zext)));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::ZExt);
    validateAfterS1(top);
}

static void dynamicRangeReadStaysHardwareOp() {
    auto top = baseTop();
    top.params.push_back(valueParam("n", int8()));
    top.params.push_back(valueParam("idx", int8()));
    top.params.push_back(outputParam("out", int4()));
    auto value = call("__dynamic_range_at",
                      {make_var("n", int8()), make_var("idx", int8())}, int4());
    value->intrinsic = IntrinsicKind::DynamicRangeAt;
    value->to_width = 4;
    top.body.push_back(assign(make_var("out", int4()), std::move(value)));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body[0]->assign_value->kind == pred::s1apinorm::S1ExprKind::HardwareOp);
    CHECK(norm.body[0]->assign_value->hardware_op == pred::s1apinorm::S1HardwareOp::DynamicSlice);
    CHECK(norm.body[0]->assign_value->to_width == 4);
    validateAfterS1(top);
}

static void sourceAtAPIsPassThroughS2() {
    auto ast = parseFixture("testv2/fixtures/s1apinorm/source_at.logic.cpp");
    auto norm = normalizeS1Ok(ast);
    auto validation = pred::s2validate::validateFunctionAST(norm);
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());
    CHECK(containsHardwareOpList(norm.body, pred::s1apinorm::S1HardwareOp::WriteSlice));
    CHECK(containsHardwareOpList(norm.body, pred::s1apinorm::S1HardwareOp::Slice) ||
          containsHardwareOpList(norm.body, pred::s1apinorm::S1HardwareOp::BitSelect));
}

static void sourcePickAPIsBecomeDynamicHardwareOps() {
    auto ast = parseFixture("testv2/fixtures/s1apinorm/source_pick.logic.cpp");
    auto norm = normalizeS1Ok(ast);
    auto validation = pred::s2validate::validateFunctionAST(norm);
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());
    CHECK(containsHardwareOpList(norm.body, pred::s1apinorm::S1HardwareOp::DynamicSlice));
    CHECK(containsHardwareOpList(norm.body, pred::s1apinorm::S1HardwareOp::DynamicBitSelect));

    auto s3 = pred::s3statementize::statementizeFunctionAST(norm);
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());
    CHECK(containsS3HardwareOp(s3.program->top.body,
                               pred::s3statementize::HardwareOp::DynamicSlice));
    CHECK(containsS3HardwareOp(s3.program->top.body,
                               pred::s3statementize::HardwareOp::DynamicBitSelect));

    auto s4 = pred::s4cfg::buildCFGProgram(s3.program.value());
    if (!s4.ok()) std::cerr << s4.error->formatted << "\n";
    CHECK(s4.ok());
    CHECK(s4.program.has_value());
    CHECK(containsCFGHardwareOp(s4.program->top,
                                pred::s3statementize::HardwareOp::DynamicSlice));
    CHECK(containsCFGHardwareOp(s4.program->top,
                                pred::s3statementize::HardwareOp::DynamicBitSelect));

    auto s5 = pred::s5unroll::unrollCFGProgram(s4.program.value());
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());
    CHECK(containsCFGHardwareOp(s5.program->top,
                                pred::s3statementize::HardwareOp::DynamicSlice));
    CHECK(containsCFGHardwareOp(s5.program->top,
                                pred::s3statementize::HardwareOp::DynamicBitSelect));

    auto s6 = pred::s6inline::inlineCFGProgram(s5.program.value());
    if (!s6.ok()) std::cerr << s6.error->formatted << "\n";
    CHECK(s6.ok());
    CHECK(s6.program.has_value());
    CHECK(containsInlinedHardwareOp(s6.program->top,
                                    pred::s3statementize::HardwareOp::DynamicSlice));
    CHECK(containsInlinedHardwareOp(s6.program->top,
                                    pred::s3statementize::HardwareOp::DynamicBitSelect));
}

int main() {
    pseudoSliceWriteBecomesWriteSlice();
    pseudoSliceReadBecomesSlice();
    residualReduceCallBecomesHardwareExpr();
    dynamicRangeWriteBecomesHardwareExpr();
    residualZExtCallBecomesHardwareExpr();
    dynamicRangeReadStaysHardwareOp();
    sourceAtAPIsPassThroughS2();
    sourcePickAPIsBecomeDynamicHardwareOps();
    return 0;
}
