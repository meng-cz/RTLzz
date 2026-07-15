#include "ast/AST.h"
#include "s3statementize/S3Statementize.h"

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

static TypeInfo boolType() {
    return make_bool_type();
}

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static TypeInfo arrayOf(TypeInfo elem, int size) {
    elem.is_array = true;
    elem.array_size = size;
    elem.array_dims = {size};
    return elem;
}

static TypeInfo structType(const std::string& name) {
    TypeInfo type;
    type.name = name;
    type.struct_name = name;
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

static StmtPtr ifStmt(ExprPtr cond, std::vector<StmtPtr> then_body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::If;
    stmt->if_cond = std::move(cond);
    stmt->if_then = std::move(then_body);
    return stmt;
}

static StmtPtr whileStmt(ExprPtr cond, std::vector<StmtPtr> body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::While;
    stmt->while_cond = std::move(cond);
    stmt->while_body = std::move(body);
    return stmt;
}

static StmtPtr blockStmt(std::vector<StmtPtr> body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Block;
    stmt->block_stmts = std::move(body);
    return stmt;
}

static FunctionAST baseTop() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    return top;
}

static std::string statementizeDebug(const FunctionAST& fn) {
    pred::s3statementize::StatementizeOptions options;
    options.debug_print = true;
    auto result = pred::s3statementize::statementizeFunctionAST(fn, options);
    if (!result.ok()) {
        std::cerr << result.error->formatted << "\n";
    }
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

static void nestedCallsBecomeStatementLevel() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(param("b", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(
        make_var("out", int8()),
        call("f",
             {call("g", {make_var("a", int8())}, int8()),
              call("h", {make_var("b", int8())}, int8())},
             int8())));

    auto debug = statementizeDebug(top);
    expectInOrder(debug, {
        "call __tmp_hls_main_g_0 = g(a)",
        "call __tmp_hls_main_h_1 = h(b)",
        "call out = f(__tmp_hls_main_g_0, __tmp_hls_main_h_1)",
    });
}

static void returnIfHelperAndLambdaAreStatementized() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(ifStmt(call("pred", {make_var("a", int8())}, boolType()),
                              {assign(make_var("out", int8()), make_var("a", int8()))}));

    auto helper = std::make_shared<FunctionAST>();
    helper->name = "helper_ret";
    helper->return_type = int8();
    helper->params.push_back(param("z", int8()));
    helper->body.push_back(ret(make_binary("+",
                                           call("f", {make_var("z", int8())}, int8()),
                                           make_literal("1", int8()),
                                           int8())));
    top.helpers.push_back(helper);

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "lam";
    lambda->return_type = int8();
    lambda->params.push_back(param("x", int8()));
    lambda->body.push_back(ret(call("g", {make_var("x", int8())}, int8())));
    top.lambdas["lam"] = lambda;

    auto debug = statementizeDebug(top);
    expectInOrder(debug, {
        "call __tmp_hls_main_pred_0 = pred(a)",
        "if __tmp_hls_main_pred_0",
    });
    expectContains(debug, "helper helper_ret\n");
    expectInOrder(debug, {
        "call __tmp_helper_ret_f_0 = f(z)",
        "op __tmp_helper_ret_binary_1 = Add(__tmp_helper_ret_f_0, 1)",
        "return __tmp_helper_ret_binary_1",
    });
    expectContains(debug, "lambda lam\n");
    expectContains(debug, "call __tmp_lam_g_0 = g(x)");
}

static void complexLValueKeepsRhsBeforeLhs() {
    auto top = baseTop();
    auto arr_type = arrayOf(int8(), 8);
    top.params.push_back(outputParam("arr", arr_type));
    top.params.push_back(param("i", int8()));
    top.params.push_back(param("x", int8()));

    auto indexed = make_array_access(
        make_var("arr", arr_type),
        call("f", {make_var("i", int8())}, int8()),
        int8());
    top.body.push_back(assign(indexed, call("g", {make_var("x", int8())}, int8())));

    auto debug = statementizeDebug(top);
    expectInOrder(debug, {
        "call __tmp_hls_main_g_0 = g(x)",
        "call __tmp_hls_main_f_1 = f(i)",
        "assign arr[__tmp_hls_main_f_1] = __tmp_hls_main_g_0",
    });
}

static void constructorArgsAndNestedAssignmentAreLowered() {
    auto top = baseTop();
    auto packet = structType("Packet");
    top.struct_fields["Packet"] = {StructFieldInfo{"value", int8()}};
    top.params.push_back(param("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(decl("p", packet,
                            call("Packet",
                                 {call("g", {make_var("a", int8())}, int8())},
                                 packet)));
    top.body.push_back(assign(
        make_var("out", int8()),
        make_binary("+",
                    make_binary("=", make_var("a", int8()), make_literal("1", int8()), int8()),
                    make_literal("2", int8()),
                    int8())));

    auto debug = statementizeDebug(top);
    expectInOrder(debug, {
        "call __tmp_hls_main_g_0 = g(a)",
        "construct p = Packet(__tmp_hls_main_g_0)",
    });
    expectInOrder(debug, {
        "assign a = 1",
        "op __tmp_hls_main_binary_1 = Add(a, 2)",
        "assign out = __tmp_hls_main_binary_1",
    });
}

static void incrementsShortCircuitTernaryAndLoopConditionAreStructured() {
    auto top = baseTop();
    top.params.push_back(param("a", int8()));
    top.params.push_back(param("b", int8()));
    top.params.push_back(param("cond", boolType()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              make_unary("post++", make_var("a", int8()), int8())));
    top.body.push_back(assign(make_var("out", int8()),
                              make_unary("++", make_var("b", int8()), int8())));
    top.body.push_back(ifStmt(make_binary("&&",
                                          make_var("cond", boolType()),
                                          call("pred", {make_var("a", int8())}, boolType()),
                                          boolType()),
                              {assign(make_var("out", int8()), make_var("a", int8()))}));
    top.body.push_back(assign(make_var("out", int8()),
                              make_ternary(make_var("cond", boolType()),
                                           call("f", {make_var("a", int8())}, int8()),
                                           call("g", {make_var("b", int8())}, int8()),
                                           int8())));
    top.body.push_back(whileStmt(call("keep", {make_var("out", int8())}, boolType()),
                                 {assign(make_var("out", int8()), make_var("a", int8()))}));

    auto debug = statementizeDebug(top);
    expectInOrder(debug, {
        "assign __tmp_hls_main_post_0 = a",
        "op __tmp_hls_main_inc_1 = Add(a, 1)",
        "assign a = __tmp_hls_main_inc_1",
        "assign out = __tmp_hls_main_post_0",
    });
    expectInOrder(debug, {
        "op __tmp_hls_main_inc_2 = Add(b, 1)",
        "assign b = __tmp_hls_main_inc_2",
        "assign out = b",
    });
    expectInOrder(debug, {
        "assign __tmp_hls_main_shortcircuit_3 = cond",
        "if cond",
        "call __tmp_hls_main_pred_4 = pred(a)",
        "assign __tmp_hls_main_shortcircuit_3 = __tmp_hls_main_pred_4",
        "if __tmp_hls_main_shortcircuit_3",
    });
    expectInOrder(debug, {
        "if cond",
        "call __tmp_hls_main_f_6 = f(a)",
        "assign __tmp_hls_main_ternary_5 = __tmp_hls_main_f_6",
        "else",
        "call __tmp_hls_main_g_7 = g(b)",
        "assign __tmp_hls_main_ternary_5 = __tmp_hls_main_g_7",
        "assign out = __tmp_hls_main_ternary_5",
    });
    expectInOrder(debug, {
        "while __tmp_hls_main_keep_8",
        "cond_prelude",
        "call __tmp_hls_main_keep_8 = keep(out)",
    });
}

static void shadowedNamesUseDistinctSymbols() {
    auto top = baseTop();
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(decl("x", int8(), make_literal("1", int8())));
    top.body.push_back(blockStmt({
        decl("x", int8(), make_literal("2", int8())),
        assign(make_var("out", int8()), make_var("x", int8())),
    }));

    auto program = pred::s3statementize::statementizeFunctionASTOrThrow(top);
    const auto& fn = program.top;
    CHECK(fn.symbols.size() >= 3);

    pred::s3statementize::SymbolId outer_x = -1;
    pred::s3statementize::SymbolId inner_x = -1;
    for (const auto& symbol : fn.symbols) {
        if (symbol.name != "x") continue;
        if (outer_x < 0) outer_x = symbol.id;
        else inner_x = symbol.id;
    }
    CHECK(outer_x >= 0);
    CHECK(inner_x >= 0);
    CHECK(outer_x != inner_x);

    pred::s3statementize::S3StmtPtr out_assign;
    for (const auto& stmt : fn.body) {
        if (stmt->kind == pred::s3statementize::S3StmtKind::Assign &&
            stmt->target.root == "out") {
            out_assign = stmt;
        }
    }
    CHECK(out_assign != nullptr);
    CHECK(out_assign->value.kind == pred::s3statementize::OperandKind::Var);
    CHECK(out_assign->value.var_symbol == inner_x);
}

int main() {
    nestedCallsBecomeStatementLevel();
    returnIfHelperAndLambdaAreStatementized();
    complexLValueKeepsRhsBeforeLhs();
    constructorArgsAndNestedAssignmentAreLowered();
    incrementsShortCircuitTernaryAndLoopConditionAreStructured();
    shadowedNamesUseDistinctSymbols();
    return 0;
}
