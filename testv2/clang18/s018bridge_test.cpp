#include "s0clang18/s018bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

namespace {

pred::DebugLoc loc(int line) {
    pred::DebugLoc out;
    out.file = "testv2/clang18/s018_virtual_input.logic.cpp";
    out.line = line;
    out.column = 1;
    return out;
}

pred::v2::TypeInfo intType(int width) {
    return pred::v2::make_hw_type("Int", width, false);
}

pred::v2::TypeInfo voidType() {
    return pred::v2::make_unknown_type("void");
}

pred::v2::TypeInfo pairType() {
    pred::v2::TypeInfo type;
    type.name = "Pair";
    type.struct_name = "Pair";
    return type;
}

pred::v2::ExprPtr varRef(const std::string& name, pred::v2::TypeInfo type) {
    auto expr = pred::v2::make_var(name, std::move(type));
    expr->debug_loc = loc(20);
    return expr;
}

pred::v2::ExprPtr literal(const std::string& value, pred::v2::TypeInfo type) {
    auto expr = pred::v2::make_literal(value, std::move(type));
    expr->debug_loc = loc(21);
    return expr;
}

pred::v2::StmtPtr declStmt(const std::string& name, pred::v2::TypeInfo type) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_type = std::move(type);
    stmt->debug_loc = loc(30);
    return stmt;
}

pred::v2::StmtPtr assignStmt(pred::v2::ExprPtr target, pred::v2::ExprPtr value) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    stmt->debug_loc = loc(31);
    return stmt;
}

pred::s0clang18::RecordMetadataSet makeRecords() {
    pred::s0clang18::RecordMetadataSet records;
    pred::s0clang18::RecordMetadata pair;
    pair.key.canonical_name = "Pair";
    pair.aggregate_initializable = true;
    pair.loc = loc(40);

    pred::s0clang18::RecordField lo;
    lo.name = "lo";
    lo.type = intType(8);
    lo.loc = loc(41);
    pair.fields.push_back(std::move(lo));

    pred::s0clang18::RecordField hi;
    hi.name = "hi";
    hi.type = intType(8);
    hi.loc = loc(42);
    pair.fields.push_back(std::move(hi));

    pred::s0clang18::RecordConstructor constructor;
    constructor.param_names = {"lo", "hi"};
    constructor.field_to_param.emplace("lo", "lo");
    constructor.field_to_param.emplace("hi", "hi");
    constructor.loc = loc(43);
    pair.constructors.push_back(std::move(constructor));

    records.record_by_name.emplace("Pair", 0);
    records.records.push_back(std::move(pair));
    return records;
}

pred::v2::FunctionAST makeSurfaceAST() {
    pred::v2::FunctionAST top;
    top.name = "top_hls_main";
    top.return_type = voidType();

    pred::v2::ParamDecl input;
    input.name = "in_value";
    input.type = intType(8);
    input.debug_loc = loc(50);
    top.params.push_back(input);

    pred::v2::ParamDecl output;
    output.name = "out_value";
    output.type = intType(8);
    output.direction = pred::v2::ParamDirection::Output;
    output.is_output = true;
    output.debug_loc = loc(51);
    top.params.push_back(output);

    top.body.push_back(declStmt("pair", pairType()));
    top.body.push_back(assignStmt(varRef("out_value", intType(8)),
                                  literal("0", intType(8))));

    auto helper = std::make_shared<pred::v2::FunctionAST>();
    helper->name = "helper_id";
    helper->return_type = intType(8);
    helper->params.push_back(input);
    auto ret = std::make_shared<pred::v2::Stmt>();
    ret->kind = pred::v2::StmtKind::Return;
    ret->return_value = varRef("in_value", intType(8));
    ret->debug_loc = loc(60);
    helper->body.push_back(std::move(ret));
    top.helpers.push_back(std::move(helper));
    return top;
}

void testDirectBridge() {
    pred::s0clang18::Clang18PipelineState state;
    state.options.source_name = "testv2/clang18/s018_direct.logic.cpp";
    state.options.top_function = "hls_main";
    state.records = makeRecords();

    auto program =
        pred::s0clang18::bridgeToS0Program(state, makeSurfaceAST());
    CHECK(program.source_name == state.options.source_name);
    CHECK(program.top_function == 0);
    CHECK(program.functions.size() == 2);
    CHECK(program.functions[0].kind == pred::s0ast::S0FunctionKind::Top);
    CHECK(program.functions[0].params.size() == 2);
    CHECK(program.functions[0].body.size() == 2);
    CHECK(program.functions[1].kind == pred::s0ast::S0FunctionKind::Helper);
    CHECK(program.struct_fields.count("Pair") == 1);
    CHECK(program.struct_fields.at("Pair").size() == 2);
    CHECK(program.struct_constructors.count("Pair") == 1);
    CHECK(program.surface_ast.struct_fields.count("Pair") == 1);

    std::string text = pred::s0clang18::debugPrint(state);
    CHECK(text.find("s0clang18 bridge") != std::string::npos);
}

void testPipelineEntry() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s018_pipeline.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

#pragma input_port in_value
Int<8> in_value;
#pragma output_port out_value
Int<8> out_value;

Int<8> helper(Int<8> x) {
    return x;
}

void hls_main() {
    Int<8> tmp = helper(in_value);
}
)cpp";
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};
    options.debug_print = true;

    auto result = pred::s0clang18::buildS0ProgramWithClang18(options);
    if (!result.ok()) {
        if (result.error) std::cerr << result.error->message << "\n";
        std::cerr << result.debug_text << "\n";
    }
    CHECK(result.ok());
    CHECK(result.program.has_value());
    CHECK(result.program->top_function == 0);
    CHECK(result.program->functions.size() >= 1);
    CHECK(result.program->surface_ast.name.find("hls_main") != std::string::npos);
    CHECK(!result.debug_text.empty());
}

} // namespace

int main() {
    testDirectBridge();
    testPipelineEntry();
    std::cout << "s018bridge_test passed\n";
    return 0;
}
