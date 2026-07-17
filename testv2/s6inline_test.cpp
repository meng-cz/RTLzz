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
#include <unordered_set>
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

static TypeInfo int16() {
    return make_hw_type("Int", 16, false);
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

static StmtPtr exprStmt(ExprPtr expr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->expr_stmt = std::move(expr);
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

static pred::s6inline::InlineResult runS6(const FunctionAST& ast) {
    auto s1 = pred::s1apinorm::normalizeAPIs(ast);
    if (!s1.ok()) std::cerr << s1.error->formatted << "\n";
    CHECK(s1.ok());
    CHECK(s1.function.has_value());

    auto validation = pred::s2validate::validateFunctionAST(s1.function.value());
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());

    auto s3 = pred::s3statementize::statementizeFunctionAST(s1.function.value());
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());

    auto s4 = pred::s4cfg::buildCFGProgram(s3.program.value());
    if (!s4.ok()) std::cerr << s4.error->formatted << "\n";
    CHECK(s4.ok());
    CHECK(s4.program.has_value());

    auto s5 = pred::s5unroll::unrollCFGProgram(s4.program.value());
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());

    pred::s6inline::InlineOptions options;
    options.debug_print = true;
    auto s6 = pred::s6inline::inlineCFGProgram(s5.program.value(), options);
    if (!s6.ok()) std::cerr << s6.error->formatted << "\n";
    CHECK(s6.ok());
    CHECK(s6.program.has_value());
    CHECK(!s6.debug_text.empty());
    return s6;
}

static pred::s6inline::InlineResult runS6WithoutS2(const FunctionAST& ast) {
    auto s1 = pred::s1apinorm::normalizeAPIs(ast);
    if (!s1.ok()) std::cerr << s1.error->formatted << "\n";
    CHECK(s1.ok());
    CHECK(s1.function.has_value());
    auto s3 = pred::s3statementize::statementizeFunctionAST(*s1.function);
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());

    auto s4 = pred::s4cfg::buildCFGProgram(s3.program.value());
    if (!s4.ok()) std::cerr << s4.error->formatted << "\n";
    CHECK(s4.ok());
    CHECK(s4.program.has_value());

    auto s5 = pred::s5unroll::unrollCFGProgram(s4.program.value());
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());

    pred::s6inline::InlineOptions options;
    options.debug_print = true;
    return pred::s6inline::inlineCFGProgram(s5.program.value(), options);
}

static void expectContains(const std::string& text, const std::string& needle) {
    if (text.find(needle) == std::string::npos) {
        std::cerr << "Expected debug text to contain:\n" << needle
                  << "\nActual debug text:\n" << text << "\n";
    }
    CHECK(text.find(needle) != std::string::npos);
}

static void expectErrorContains(const pred::s6inline::InlineResult& result,
                                const std::string& needle) {
    if (result.ok()) {
        std::cerr << "Expected S6 error containing:\n" << needle << "\n";
    }
    CHECK(!result.ok());
    CHECK(result.error.has_value());
    if (result.error->formatted.find(needle) == std::string::npos) {
        std::cerr << "Expected S6 error containing:\n" << needle
                  << "\nActual error:\n" << result.error->formatted << "\n";
    }
    CHECK(result.error->formatted.find(needle) != std::string::npos);
}

static std::size_t countSubstring(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static void expectNoCalls(const pred::s6inline::InlinedFunction& fn) {
    std::unordered_set<pred::s3statementize::SymbolId> ids;
    for (std::size_t i = 0; i < fn.symbols.size(); ++i) {
        CHECK(fn.symbols[i].id == static_cast<pred::s3statementize::SymbolId>(i));
        CHECK(ids.insert(fn.symbols[i].id).second);
    }
    for (const auto& block : fn.blocks) {
        for (const auto& stmt : block->stmts) {
            CHECK(!stmt.stmt ||
                  stmt.stmt->kind != pred::s3statementize::S3StmtKind::Call);
        }
    }
}

static bool hasAssignToRoot(const pred::s6inline::InlinedFunction& fn,
                            const std::string& root) {
    for (const auto& block : fn.blocks) {
        for (const auto& stmt : block->stmts) {
            if (stmt.stmt &&
                stmt.stmt->kind == pred::s3statementize::S3StmtKind::Assign &&
                stmt.stmt->target.root == root) {
                return true;
            }
        }
    }
    return false;
}

static bool hasSymbolContaining(const pred::s6inline::InlinedFunction& fn,
                                const std::string& needle) {
    for (const auto& symbol : fn.symbols) {
        if (symbol.name.find(needle) != std::string::npos) return true;
    }
    return false;
}

static void helperReturnAndMutableRefInline() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(decl("t", int8(), call("add1", {make_var("a", int8())}, int8())));
    top.body.push_back(exprStmt(call("touch", {make_var("t", int8())}, voidType())));
    top.body.push_back(assign(make_var("out", int8()), make_var("t", int8())));

    auto add1 = std::make_shared<FunctionAST>();
    add1->name = "add1";
    add1->return_type = int8();
    add1->params.push_back(valueParam("x", int8()));
    add1->body.push_back(ret(make_binary("+", make_var("x", int8()),
                                         make_literal("1", int8()), int8())));
    top.helpers.push_back(add1);

    auto touch = std::make_shared<FunctionAST>();
    touch->name = "touch";
    touch->return_type = voidType();
    touch->params.push_back(outputParam("value", int8()));
    touch->body.push_back(assign(make_var("value", int8()),
                                 make_binary("+", make_var("value", int8()),
                                             make_literal("2", int8()), int8())));
    top.helpers.push_back(touch);

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() == 2);
    expectContains(s6.debug_text, "inlined caller=hls_main callee=add1");
    expectContains(s6.debug_text, "inlined caller=hls_main callee=touch");
    CHECK(hasAssignToRoot(s6.program->top, "t"));
}

static void valueParamWriteDoesNotAliasCallerActual() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("clobber", {make_var("a", int8())}, int8())));

    auto clobber = std::make_shared<FunctionAST>();
    clobber->name = "clobber";
    clobber->return_type = int8();
    clobber->params.push_back(valueParam("x", int8()));
    clobber->body.push_back(assign(make_var("x", int8()), make_literal("7", int8())));
    clobber->body.push_back(ret(make_var("x", int8())));
    top.helpers.push_back(clobber);

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(!hasAssignToRoot(s6.program->top, "a"));
    expectContains(s6.debug_text, "__s6_clobber_");
}

static void nestedHelperAndLambdaInline() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("outer", {make_var("a", int8())}, int8())));

    auto inner = std::make_shared<FunctionAST>();
    inner->name = "inner";
    inner->return_type = int8();
    inner->params.push_back(valueParam("x", int8()));
    inner->body.push_back(ret(make_var("x", int8())));
    top.helpers.push_back(inner);

    auto outer = std::make_shared<FunctionAST>();
    outer->name = "outer";
    outer->return_type = int8();
    outer->params.push_back(valueParam("y", int8()));
    outer->body.push_back(ret(call("inner", {make_var("y", int8())}, int8())));
    top.helpers.push_back(outer);

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "lam";
    lambda->return_type = int8();
    lambda->params.push_back(valueParam("z", int8()));
    lambda->body.push_back(ret(make_var("z", int8())));
    top.lambdas["lam"] = lambda;
    top.body.push_back(assign(make_var("out", int8()),
                              call("lam", {make_var("out", int8())}, int8())));

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() == 3);
    expectContains(s6.debug_text, "callee=inner");
    expectContains(s6.debug_text, "callee=outer");
    expectContains(s6.debug_text, "callee=lam");
}

static void deeperMultiLevelHelperInline() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("level1", {make_var("a", int8())}, int8())));

    auto level4 = std::make_shared<FunctionAST>();
    level4->name = "level4";
    level4->return_type = int8();
    level4->params.push_back(valueParam("x", int8()));
    level4->body.push_back(ret(make_binary("+", make_var("x", int8()),
                                           make_literal("4", int8()), int8())));
    top.helpers.push_back(level4);

    auto level3 = std::make_shared<FunctionAST>();
    level3->name = "level3";
    level3->return_type = int8();
    level3->params.push_back(valueParam("x", int8()));
    level3->body.push_back(ret(call("level4", {make_var("x", int8())}, int8())));
    top.helpers.push_back(level3);

    auto level2 = std::make_shared<FunctionAST>();
    level2->name = "level2";
    level2->return_type = int8();
    level2->params.push_back(valueParam("x", int8()));
    level2->body.push_back(ret(call("level3", {make_var("x", int8())}, int8())));
    top.helpers.push_back(level2);

    auto level1 = std::make_shared<FunctionAST>();
    level1->name = "level1";
    level1->return_type = int8();
    level1->params.push_back(valueParam("x", int8()));
    level1->body.push_back(ret(call("level2", {make_var("x", int8())}, int8())));
    top.helpers.push_back(level1);

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() == 4);
    expectContains(s6.debug_text, "callee=level1");
    expectContains(s6.debug_text, "callee=level2");
    expectContains(s6.debug_text, "callee=level3");
    expectContains(s6.debug_text, "callee=level4");
}

static void overloadResolutionInline() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(valueParam("b", int16()));
    top.params.push_back(outputParam("out_a", int8()));
    top.params.push_back(outputParam("out_b", int16()));
    top.body.push_back(assign(make_var("out_a", int8()),
                              call("pick", {make_var("a", int8())}, int8())));
    top.body.push_back(assign(make_var("out_b", int16()),
                              call("pick", {make_var("b", int16())}, int16())));

    auto pick_8 = std::make_shared<FunctionAST>();
    pick_8->name = "pick";
    pick_8->return_type = int8();
    pick_8->params.push_back(valueParam("x", int8()));
    pick_8->body.push_back(decl("width8_marker", int8(), make_var("x", int8())));
    pick_8->body.push_back(ret(make_var("width8_marker", int8())));
    top.helpers.push_back(pick_8);

    auto pick_16 = std::make_shared<FunctionAST>();
    pick_16->name = "pick";
    pick_16->return_type = int16();
    pick_16->params.push_back(valueParam("x", int16()));
    pick_16->body.push_back(decl("width16_marker", int16(), make_var("x", int16())));
    pick_16->body.push_back(ret(make_var("width16_marker", int16())));
    top.helpers.push_back(pick_16);

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() == 2);
    CHECK(hasSymbolContaining(s6.program->top, "width8_marker"));
    CHECK(hasSymbolContaining(s6.program->top, "width16_marker"));
}

static void lambdaWinsOverSameNameHelper() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("same", {make_var("a", int8())}, int8())));

    auto helper = std::make_shared<FunctionAST>();
    helper->name = "same";
    helper->return_type = int8();
    helper->params.push_back(valueParam("x", int8()));
    helper->body.push_back(decl("helper_marker", int8(), make_literal("1", int8())));
    helper->body.push_back(ret(make_var("helper_marker", int8())));
    top.helpers.push_back(helper);

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "same";
    lambda->return_type = int8();
    lambda->params.push_back(valueParam("x", int8()));
    lambda->body.push_back(decl("lambda_marker", int8(), make_literal("2", int8())));
    lambda->body.push_back(ret(make_var("lambda_marker", int8())));
    top.lambdas["same"] = lambda;

    auto s6 = runS6WithoutS2(top);
    if (!s6.ok()) std::cerr << s6.error->formatted << "\n";
    CHECK(s6.ok());
    CHECK(s6.program.has_value());
    expectNoCalls(s6.program->top);
    CHECK(hasSymbolContaining(s6.program->top, "lambda_marker"));
    CHECK(!hasSymbolContaining(s6.program->top, "helper_marker"));
}

static void helperCallsLambdaInline() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("outer", {make_var("a", int8())}, int8())));

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "inner_lam";
    lambda->return_type = int8();
    lambda->params.push_back(valueParam("x", int8()));
    lambda->body.push_back(decl("lambda_inside_helper_marker", int8(),
                                make_var("x", int8())));
    lambda->body.push_back(ret(make_var("lambda_inside_helper_marker", int8())));
    top.lambdas["inner_lam"] = lambda;

    auto outer = std::make_shared<FunctionAST>();
    outer->name = "outer";
    outer->return_type = int8();
    outer->params.push_back(valueParam("y", int8()));
    outer->body.push_back(ret(call("inner_lam", {make_var("y", int8())}, int8())));
    top.helpers.push_back(outer);

    auto s6 = runS6(top);
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() == 2);
    expectContains(s6.debug_text, "inlined caller=outer callee=inner_lam");
    expectContains(s6.debug_text, "inlined caller=hls_main callee=outer");
    CHECK(hasSymbolContaining(s6.program->top, "lambda_inside_helper_marker"));
}

static void indirectRecursionDetectedInS6() {
    auto top = baseTop();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("foo", {make_var("a", int8())}, int8())));

    auto foo = std::make_shared<FunctionAST>();
    foo->name = "foo";
    foo->return_type = int8();
    foo->params.push_back(valueParam("x", int8()));
    foo->body.push_back(ret(call("bar", {make_var("x", int8())}, int8())));
    top.helpers.push_back(foo);

    auto bar = std::make_shared<FunctionAST>();
    bar->name = "bar";
    bar->return_type = int8();
    bar->params.push_back(valueParam("y", int8()));
    bar->body.push_back(ret(call("foo", {make_var("y", int8())}, int8())));
    top.helpers.push_back(bar);

    expectErrorContains(runS6WithoutS2(top),
                        "Recursive helper/lambda call graph reached S6");
}

static void sourceIntegrationInline() {
    auto s6 = runS6(parseFixture("testv2/fixtures/s6inline/source_helpers.logic.cpp"));
    expectNoCalls(s6.program->top);
    CHECK(s6.summaries.size() >= 4);
    expectContains(s6.debug_text, "callee=choose");
    expectContains(s6.debug_text, "callee=adjust");
    expectContains(s6.debug_text, "callee=touch");
    expectContains(s6.debug_text, "callee=lam");
}

static void sourceLoopCallsInlineAfterUnroll() {
    auto s6 = runS6(parseFixture("testv2/fixtures/s6inline/source_loop_calls.logic.cpp"));
    expectNoCalls(s6.program->top);
    CHECK(countSubstring(s6.debug_text, "callee=inc") >= 3);
    CHECK(countSubstring(s6.debug_text, "callee=bump") == 3);
    expectContains(s6.debug_text, "inlined caller=bump callee=inc");
}

int main() {
    helperReturnAndMutableRefInline();
    valueParamWriteDoesNotAliasCallerActual();
    nestedHelperAndLambdaInline();
    deeperMultiLevelHelperInline();
    overloadResolutionInline();
    lambdaWinsOverSameNameHelper();
    helperCallsLambdaInline();
    indirectRecursionDetectedInS6();
    sourceIntegrationInline();
    sourceLoopCallsInlineAfterUnroll();
    return 0;
}
