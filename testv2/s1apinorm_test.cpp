#include "s0ast/S0AST.h"
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
using namespace pred::v2;

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

static TypeInfo int8() {
    return make_hw_type("Int", 8, false);
}

static TypeInfo int4() {
    return make_hw_type("Int", 4, false);
}

static TypeInfo int16() {
    return make_hw_type("Int", 16, false);
}

static TypeInfo boolType() {
    return make_bool_type();
}

static TypeInfo pairType() {
    TypeInfo type;
    type.name = "Pair";
    type.struct_name = "Pair";
    return type;
}

static TypeInfo boxType() {
    TypeInfo type;
    type.name = "Box";
    type.struct_name = "Box";
    return type;
}

static TypeInfo arrayOf(TypeInfo elem, int size) {
    elem.is_array = true;
    elem.array_size = size;
    elem.array_dims = {size};
    return elem;
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

static StmtPtr decl(const std::string& name, TypeInfo type, ExprPtr init = nullptr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_type = std::move(type);
    if (init) stmt->decl_init = std::move(init);
    return stmt;
}

static StmtPtr ret(ExprPtr value = nullptr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Return;
    if (value) stmt->return_value = std::move(value);
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
    auto parsed = pred::s0ast::parseProgram(file, std::nullopt, "hls_main", clang_args);
    if (!parsed.ok()) {
        std::cerr << "S0 parse failed for " << file << ":\n"
                  << (parsed.error ? parsed.error->message : "unknown error") << "\n";
    }
    CHECK(parsed.ok());
    CHECK(parsed.program.has_value());
    return pred::s0ast::surfaceAST(*parsed.program);
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
        containsHardwareOpList(stmt->for_init, op) ||
        containsHardwareOpExpr(stmt->for_cond, op) ||
        containsHardwareOpExpr(stmt->for_step, op) ||
        containsHardwareOpList(stmt->for_body, op) ||
        containsHardwareOpExpr(stmt->while_cond, op) ||
        containsHardwareOpList(stmt->while_body, op) ||
        containsHardwareOpExpr(stmt->switch_expr, op) ||
        containsHardwareOpList(stmt->block_stmts, op) ||
        containsHardwareOpExpr(stmt->expr_stmt, op) ||
        containsHardwareOpExpr(stmt->construct_target, op)) {
        return true;
    }
    if (stmt->return_value && containsHardwareOpExpr(stmt->return_value.value(), op)) return true;
    for (const auto& arg : stmt->construct_args) {
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
    top.params.push_back(outputParam("out", int16()));
    auto zext = call("zext", {make_var("n", int8())}, int16());
    zext->to_width = 16;
    top.body.push_back(assign(make_var("out", int16()), std::move(zext)));

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

static void declarationInitCallBecomesDeclThenAssignCall() {
    auto top = baseTop();
    top.params.push_back(valueParam("seed", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.struct_fields["Pair"] = {
        StructFieldInfo{"n", int8()},
        StructFieldInfo{"m", int8()},
    };

    auto helper = std::make_shared<FunctionAST>();
    helper->name = "make_pair";
    helper->return_type = pairType();
    helper->params.push_back(valueParam("seed", int8()));
    helper->body.push_back(decl("tmp", pairType()));
    helper->body.push_back(ret(make_var("tmp", pairType())));
    top.helpers.push_back(helper);

    top.body.push_back(decl("p", pairType(),
                            call("make_pair", {make_var("seed", int8())}, pairType())));
    top.body.push_back(assign(
        make_var("out", int8()),
        make_field_access(make_var("p", pairType()), "n", int8())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body.size() == 3);
    CHECK(norm.body[0]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[0]->decl_name == "p");
    CHECK(norm.body[1]->kind == pred::s1apinorm::S1StmtKind::Assign);
    CHECK(norm.body[1]->assign_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[1]->assign_target->var_name == "p");
    CHECK(norm.body[1]->assign_value->kind == pred::s1apinorm::S1ExprKind::Call);
    CHECK(norm.body[1]->assign_value->callee == "make_pair");
    CHECK(norm.body[2]->kind == pred::s1apinorm::S1StmtKind::Assign);
    validateAfterS1(top);
}

static void declarationInitArgsBecomeConstructStmt() {
    auto top = baseTop();
    top.params.push_back(outputParam("out", int8()));
    auto init = decl("n", int8());
    init->decl_init_args.push_back(make_literal("1", int8()));
    top.body.push_back(init);
    top.body.push_back(assign(make_var("out", int8()), make_var("n", int8())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body.size() == 3);
    CHECK(norm.body[0]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[1]->kind == pred::s1apinorm::S1StmtKind::Construct);
    CHECK(norm.body[1]->construct_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[1]->construct_target->var_name == "n");
    CHECK(norm.body[1]->construct_args.size() == 1);
    CHECK(norm.body[2]->kind == pred::s1apinorm::S1StmtKind::Assign);
    validateAfterS1(top);
}

static void aggregateDeclarationInitFromFieldAndArrayBecomeAssigns() {
    auto top = baseTop();
    auto pair_array = arrayOf(pairType(), 2);
    top.params.push_back(valueParam("idx", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.struct_fields["Pair"] = {
        StructFieldInfo{"n", int8()},
        StructFieldInfo{"m", int8()},
    };
    top.struct_fields["Box"] = {
        StructFieldInfo{"member", pairType()},
    };

    top.body.push_back(decl("box", boxType()));
    top.body.push_back(decl("arr", pair_array));
    top.body.push_back(decl(
        "from_member",
        pairType(),
        make_field_access(make_var("box", boxType()), "member", pairType())));
    top.body.push_back(decl(
        "from_index",
        pairType(),
        make_array_access(make_var("arr", pair_array), make_var("idx", int8()),
                          pairType())));
    top.body.push_back(assign(
        make_var("out", int8()),
        make_binary("+",
                    make_field_access(make_var("from_member", pairType()), "n", int8()),
                    make_field_access(make_var("from_index", pairType()), "m", int8()),
                    int8())));

    auto norm = normalizeS1Ok(top);
    CHECK(norm.body.size() == 7);
    CHECK(norm.body[0]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[0]->decl_name == "box");
    CHECK(norm.body[1]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[1]->decl_name == "arr");

    CHECK(norm.body[2]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[2]->decl_name == "from_member");
    CHECK(norm.body[3]->kind == pred::s1apinorm::S1StmtKind::Assign);
    CHECK(norm.body[3]->assign_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[3]->assign_target->var_name == "from_member");
    CHECK(norm.body[3]->assign_value->kind == pred::s1apinorm::S1ExprKind::FieldAccess);
    CHECK(norm.body[3]->assign_value->field_name == "member");

    CHECK(norm.body[4]->kind == pred::s1apinorm::S1StmtKind::Decl);
    CHECK(norm.body[4]->decl_name == "from_index");
    CHECK(norm.body[5]->kind == pred::s1apinorm::S1StmtKind::Assign);
    CHECK(norm.body[5]->assign_target->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[5]->assign_target->var_name == "from_index");
    CHECK(norm.body[5]->assign_value->kind == pred::s1apinorm::S1ExprKind::ArrayAccess);
    CHECK(norm.body[5]->assign_value->index->kind == pred::s1apinorm::S1ExprKind::VarRef);
    CHECK(norm.body[5]->assign_value->index->var_name == "idx");
    CHECK(norm.body[6]->kind == pred::s1apinorm::S1StmtKind::Assign);

    auto validation = pred::s2validate::validateFunctionAST(norm);
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());

    auto s3 = pred::s3statementize::statementizeFunctionAST(norm);
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());
}

static void sourceAggregateDeclarationInitsFollowParsedConstructorShape() {
    auto ast = parseFixture("testv2/fixtures/s1apinorm/source_aggregate_decl_init.logic.cpp");
    auto norm = normalizeS1Ok(ast);

    bool saw_from_member = false;
    bool saw_from_index = false;
    for (std::size_t i = 1; i < norm.body.size(); ++i) {
        const auto& prev = norm.body[i - 1];
        const auto& stmt = norm.body[i];
        if (!prev || !stmt ||
            prev->kind != pred::s1apinorm::S1StmtKind::Decl ||
            stmt->kind != pred::s1apinorm::S1StmtKind::Construct ||
            !stmt->construct_target ||
            stmt->construct_target->kind != pred::s1apinorm::S1ExprKind::VarRef) {
            continue;
        }
        if (prev->decl_name == "from_member") {
            CHECK(stmt->construct_target->var_name == "from_member");
            CHECK(stmt->construct_callee == "Pair");
            CHECK(stmt->construct_args.size() == 1);
            CHECK(stmt->construct_args[0]->kind == pred::s1apinorm::S1ExprKind::FieldAccess);
            CHECK(stmt->construct_args[0]->field_name == "member");
            saw_from_member = true;
        }
        if (prev->decl_name == "from_index") {
            CHECK(stmt->construct_target->var_name == "from_index");
            CHECK(stmt->construct_callee == "Pair");
            CHECK(stmt->construct_args.size() == 1);
            CHECK(stmt->construct_args[0]->kind == pred::s1apinorm::S1ExprKind::ArrayAccess);
            CHECK(stmt->construct_args[0]->index->kind == pred::s1apinorm::S1ExprKind::VarRef);
            CHECK(stmt->construct_args[0]->index->var_name == "idx");
            saw_from_index = true;
        }
    }
    CHECK(saw_from_member);
    CHECK(saw_from_index);

    auto validation = pred::s2validate::validateFunctionAST(norm);
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());

    auto s3 = pred::s3statementize::statementizeFunctionAST(norm);
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());
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
    declarationInitCallBecomesDeclThenAssignCall();
    declarationInitArgsBecomeConstructStmt();
    aggregateDeclarationInitFromFieldAndArrayBecomeAssigns();
    sourceAggregateDeclarationInitsFollowParsedConstructorShape();
    sourceAtAPIsPassThroughS2();
    sourcePickAPIsBecomeDynamicHardwareOps();
    return 0;
}
