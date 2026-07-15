#include "ast/ASTBuilder.h"
#include "s2validate/S2Validate.h"

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

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static ParamDecl valueParam(const std::string& name, TypeInfo type) {
    ParamDecl param;
    param.name = name;
    param.type = std::move(type);
    param.passing = ParamPassingKind::Value;
    param.direction = ParamDirection::Input;
    return param;
}

static ParamDecl outputParam(const std::string& name, TypeInfo type) {
    ParamDecl param;
    param.name = name;
    param.type = std::move(type);
    param.type.is_reference = true;
    param.passing = ParamPassingKind::MutableRef;
    param.direction = ParamDirection::Output;
    param.is_reference = true;
    param.is_output = true;
    return param;
}

static ExprPtr call(const std::string& callee, std::vector<ExprPtr> args, TypeInfo type) {
    auto expr = std::make_shared<Expr>();
    expr->kind = ExprKind::Call;
    expr->callee = callee;
    expr->args = std::move(args);
    expr->type = std::move(type);
    return expr;
}

static StmtPtr exprStmt(ExprPtr expr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->expr_stmt = std::move(expr);
    return stmt;
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

static FunctionAST makeRecursiveProgram() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(exprStmt(call("foo", {make_var("a", int8())}, voidType())));

    auto foo = std::make_shared<FunctionAST>();
    foo->name = "foo";
    foo->return_type = voidType();
    foo->params.push_back(valueParam("x", int8()));
    foo->body.push_back(exprStmt(call("bar", {make_var("x", int8())}, voidType())));

    auto bar = std::make_shared<FunctionAST>();
    bar->name = "bar";
    bar->return_type = voidType();
    bar->params.push_back(valueParam("y", int8()));
    bar->body.push_back(exprStmt(call("foo", {make_var("y", int8())}, voidType())));

    top.helpers.push_back(foo);
    top.helpers.push_back(bar);
    return top;
}

static FunctionAST makeLambdaProgram() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()),
                              call("lam", {make_var("a", int8())}, int8())));

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "lam";
    lambda->return_type = int8();
    lambda->params.push_back(valueParam("z", int8()));
    lambda->body.push_back(ret(make_var("z", int8())));
    top.lambdas["lam"] = lambda;
    return top;
}

static FunctionAST makeStructRefFieldProgram() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(valueParam("a", int8()));
    top.params.push_back(outputParam("out", int8()));
    top.body.push_back(assign(make_var("out", int8()), make_var("a", int8())));

    TypeInfo bad_type;
    bad_type.name = "Bad";
    bad_type.struct_name = "Bad";

    TypeInfo ref_field = int8();
    ref_field.is_reference = true;
    top.struct_fields["Bad"] = {StructFieldInfo{"ref", ref_field}};
    top.struct_fields["struct Bad"] = top.struct_fields["Bad"];

    auto consume = std::make_shared<FunctionAST>();
    consume->name = "consume";
    consume->return_type = voidType();
    consume->params.push_back(valueParam("value", bad_type));
    consume->struct_fields = top.struct_fields;
    top.helpers.push_back(consume);
    return top;
}

static FunctionAST makeRepeatUndeclaredOperandProgram() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(outputParam("out", make_hw_type("UInt", 16, false)));
    top.body.push_back(assign(make_var("out", make_hw_type("UInt", 16, false)),
                              make_repeat(make_var("missing", int8()), 2)));
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
        std::cerr << "AST build failed for " << file << ": " << build.error << "\n";
    }
    CHECK(build.error.empty());
    CHECK(build.function.has_value());
    return std::move(build.function.value());
}

static void expectOk(const FunctionAST& fn) {
    pred::s2validate::ValidateOptions options;
    options.debug_print = true;
    auto result = pred::s2validate::validateFunctionAST(fn, options);
    if (!result.ok()) {
        std::cerr << result.error->formatted << "\n";
    }
    CHECK(result.ok());
    CHECK(!result.debug_text.empty());
}

static void expectErrorContains(const FunctionAST& fn, const std::string& needle) {
    auto result = pred::s2validate::validateFunctionAST(fn);
    if (result.ok()) {
        std::cerr << "Expected validation error containing: " << needle << "\n";
    }
    CHECK(!result.ok());
    if (result.error->formatted.find(needle) == std::string::npos) {
        std::cerr << "Expected error containing: " << needle << "\n";
        std::cerr << result.error->formatted << "\n";
    }
    CHECK(result.error->formatted.find(needle) != std::string::npos);
}

int main() {
    expectOk(parseFixture("testv2/fixtures/s2validate/legal_basic.logic.cpp"));
    expectOk(makeLambdaProgram());

    (void)parseFixture("testv2/fixtures/s2validate/illegal_struct_ref_field.logic.cpp");
    expectErrorContains(makeStructRefFieldProgram(), "reference or pointer");
    expectErrorContains(
        parseFixture("testv2/fixtures/s2validate/illegal_top_struct_param.logic.cpp"),
        "Top-level struct parameter");
    expectErrorContains(
        parseFixture("testv2/fixtures/s2validate/illegal_unknown_call.logic.cpp"),
        "Unknown function call");
    expectErrorContains(
        parseFixture("testv2/fixtures/s2validate/illegal_legacy_proxy.logic.cpp"),
        "legacy proxy carrier");
    expectErrorContains(makeRepeatUndeclaredOperandProgram(), "Use of undeclared variable");
    expectErrorContains(makeRecursiveProgram(), "Recursive helper/lambda call graph");
    return 0;
}
