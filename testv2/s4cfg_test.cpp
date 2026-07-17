#include "ast/AST.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"

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
    return make_hw_type("Int", 8, false);
}

static TypeInfo boolType() {
    return make_bool_type();
}

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static ParamDecl param(const std::string& name, TypeInfo type) {
    ParamDecl out;
    out.name = name;
    out.type = std::move(type);
    out.passing = ParamPassingKind::Value;
    out.direction = ParamDirection::Input;
    return out;
}

static ParamDecl outputParam(const std::string& name, TypeInfo type) {
    ParamDecl out = param(name, std::move(type));
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

static StmtPtr ret(ExprPtr value = nullptr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Return;
    if (value) stmt->return_value = std::move(value);
    return stmt;
}

static StmtPtr exprStmt(ExprPtr expr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->expr_stmt = std::move(expr);
    return stmt;
}

static StmtPtr ifStmt(ExprPtr cond, std::vector<StmtPtr> then_body,
                      std::vector<StmtPtr> else_body = {}) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::If;
    stmt->if_cond = std::move(cond);
    stmt->if_then = std::move(then_body);
    stmt->if_else = std::move(else_body);
    return stmt;
}

static StmtPtr whileStmt(ExprPtr cond, std::vector<StmtPtr> body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::While;
    stmt->while_cond = std::move(cond);
    stmt->while_body = std::move(body);
    return stmt;
}

static StmtPtr doWhileStmt(std::vector<StmtPtr> body, ExprPtr cond) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::DoWhile;
    stmt->while_body = std::move(body);
    stmt->while_cond = std::move(cond);
    return stmt;
}

static StmtPtr forStmt(StmtPtr init, ExprPtr cond, ExprPtr step, std::vector<StmtPtr> body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::For;
    stmt->for_init = std::move(init);
    stmt->for_cond = std::move(cond);
    stmt->for_step = std::move(step);
    stmt->for_body = std::move(body);
    return stmt;
}

static StmtPtr breakStmt() {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Break;
    return stmt;
}

static StmtPtr continueStmt() {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Continue;
    return stmt;
}

static StmtPtr switchStmt(ExprPtr selector, std::vector<CaseClause> cases) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Switch;
    stmt->switch_expr = std::move(selector);
    stmt->switch_cases = std::move(cases);
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

static FunctionAST baseTop() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    return top;
}

static std::string cfgDebug(const FunctionAST& fn) {
    auto s3 = pred::s3statementize::statementizeFunctionASTOrThrow(fn);
    pred::s4cfg::CFGOptions options;
    options.debug_print = true;
    auto result = pred::s4cfg::buildCFGProgram(s3, options);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.program.has_value());
    CHECK(!result.debug_text.empty());
    return result.debug_text;
}

static void expectContains(const std::string& text, const std::string& needle) {
    if (text.find(needle) == std::string::npos) {
        std::cerr << "Expected debug text to contain:\n" << needle
                  << "\nActual debug text:\n" << text << "\n";
    }
    CHECK(text.find(needle) != std::string::npos);
}

static void expectInOrder(const std::string& text, const std::vector<std::string>& needles) {
    std::size_t pos = 0;
    for (const auto& needle : needles) {
        auto found = text.find(needle, pos);
        if (found == std::string::npos) {
            std::cerr << "Expected in-order item:\n" << needle
                      << "\nActual debug text:\n" << text << "\n";
        }
        CHECK(found != std::string::npos);
        pos = found + needle.size();
    }
}

static void straightLineAndCallsStayInBlocks() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("f", {make_var("a", int8())}, int8())));

    auto debug = cfgDebug(top);
    expectContains(debug, "s4cfg\n");
    expectContains(debug, "top hls_main entry=bb0 exit=bb2");
    expectInOrder(debug, {
        "bb1",
        "call out = f(a)",
        "term return",
    });
}

static void ifElseAndReturnSlotAreBuilt() {
    FunctionAST top;
    top.name = "helper_ret";
    top.return_type = int8();
    top.params.push_back(param("a", int8()));
    top.params.push_back(param("cond", boolType()));
    top.body.push_back(ifStmt(make_var("cond", boolType()),
                              {ret(make_var("a", int8()))},
                              {ret(make_literal("0", int8()))}));

    auto debug = cfgDebug(top);
    expectContains(debug, "return_slot=__ret_helper_ret_0");
    expectContains(debug, "term branch cond ?");
    expectContains(debug, "assign __ret_helper_ret_0 = a");
    expectContains(debug, "assign __ret_helper_ret_0 = 0");
    expectContains(debug, "term return __ret_helper_ret_0");
}

static void whileContinueTargetsConditionPrelude() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(whileStmt(call("keep", {make_var("out", int8())}, boolType()),
                                 {continueStmt(),
                                  assign(make_var("out", int8()), make_var("a", int8()))}));

    auto debug = cfgDebug(top);
    expectInOrder(debug, {
        "loop 0 pre_test",
        "condition_prelude=bb",
        "call __tmp_hls_main_keep_0 = keep(out)",
        "term branch __tmp_hls_main_keep_0",
        "succs=[continue:bb3]",
        "term jump bb3",
    });
}

static void forAndDoWhileLoopsHaveRegions() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(forStmt(
        decl("i", int8(), make_literal("0", int8())),
        call("lt", {make_var("i", int8())}, boolType()),
        make_unary("++", make_var("i", int8()), int8()),
        {assign(make_var("out", int8()), make_var("i", int8()))}));
    top.body.push_back(doWhileStmt(
        {assign(make_var("out", int8()), make_var("a", int8()))},
        call("again", {make_var("out", int8())}, boolType())));

    auto debug = cfgDebug(top);
    expectContains(debug, "loop 0 pre_test");
    expectContains(debug, "loop 1 post_test");
    expectContains(debug, "condition_prelude=bb");
    expectContains(debug, "call __tmp_hls_main_lt_0 = lt(i)");
    expectContains(debug, "op __tmp_hls_main_inc_1 = Add(i, 1)");
    expectContains(debug, "call __tmp_hls_main_again_2 = again(out)");
    expectContains(debug, "backedge");
}

static void switchFallthroughBreakAndDefaultAreExplicit() {
    auto top = baseTop();
    top.params.push_back(param("sel", int8()));
    top.params.push_back(outputParam("out", int8()));
    CaseClause c0;
    c0.value = make_literal("0", int8());
    c0.body.push_back(assign(make_var("out", int8()), make_literal("1", int8())));
    CaseClause c1;
    c1.value = make_literal("1", int8());
    c1.body.push_back(assign(make_var("out", int8()), make_literal("2", int8())));
    c1.body.push_back(breakStmt());
    CaseClause def;
    def.body.push_back(assign(make_var("out", int8()), make_literal("3", int8())));
    top.body.push_back(switchStmt(make_var("sel", int8()), {c0, c1, def}));

    auto debug = cfgDebug(top);
    expectContains(debug, "term switch sel case 0");
    expectContains(debug, "case 1");
    expectContains(debug, "default ->");
    expectContains(debug, "fallthrough");
    expectContains(debug, "break");
}

static void helpersAndLambdasAreBuilt() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(exprStmt(call("proc", {make_var("a", int8())}, voidType())));

    auto helper = std::make_shared<FunctionAST>();
    helper->name = "proc";
    helper->return_type = voidType();
    helper->params.push_back(param("x", int8()));
    helper->body.push_back(ret());
    top.helpers.push_back(helper);

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "lam";
    lambda->return_type = int8();
    lambda->params.push_back(param("x", int8()));
    lambda->body.push_back(ret(make_var("x", int8())));
    top.lambdas["lam"] = lambda;

    auto debug = cfgDebug(top);
    expectContains(debug, "helper proc");
    expectContains(debug, "lambda lam");
    expectContains(debug, "return_slot=__ret_lam_0");
}

static void invalidBreakIsError() {
    auto top = baseTop();
    top.body.push_back(breakStmt());
    auto s3 = pred::s3statementize::statementizeFunctionASTOrThrow(top);
    auto result = pred::s4cfg::buildCFGProgram(s3);
    CHECK(!result.ok());
    CHECK(result.error->formatted.find("break statement is not inside") != std::string::npos);
}

int main() {
    straightLineAndCallsStayInBlocks();
    ifElseAndReturnSlotAreBuilt();
    whileContinueTargetsConditionPrelude();
    forAndDoWhileLoopsHaveRegions();
    switchFallthroughBreakAndDefaultAreExplicit();
    helpersAndLambdasAreBuilt();
    invalidBreakIsError();
    return 0;
}
