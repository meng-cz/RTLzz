#include "s0clang18/s017validate.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
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
    out.file = "testv2/clang18/s017_virtual_input.logic.cpp";
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

pred::s0clang18::RawPortDecl rawPort(
    const std::string& name,
    pred::s0clang18::PortDirection direction,
    pred::v2::TypeInfo type) {
    pred::s0clang18::RawPortDecl port;
    port.name = name;
    port.direction = direction;
    port.type = std::move(type);
    port.decl_loc = loc(10);
    return port;
}

pred::s0clang18::PortDeclTable makePorts() {
    pred::s0clang18::PortDeclTable ports;
    ports.ports.push_back(rawPort(
        "in_value", pred::s0clang18::PortDirection::Input, intType(8)));
    ports.ports.push_back(rawPort(
        "out_value", pred::s0clang18::PortDirection::Output, intType(8)));
    for (std::size_t i = 0; i < ports.ports.size(); ++i) {
        ports.port_by_name.emplace(ports.ports[i].name, i);
    }
    return ports;
}

pred::s0clang18::RecordMetadataSet makeRecords(bool illegal_pointer_field) {
    pred::s0clang18::RecordMetadataSet records;
    pred::s0clang18::RecordMetadata pair;
    pair.key.canonical_name = "Pair";
    pair.aggregate_initializable = true;
    pair.loc = loc(20);

    pred::s0clang18::RecordField lo;
    lo.name = "lo";
    lo.type = intType(8);
    lo.loc = loc(21);
    pair.fields.push_back(std::move(lo));

    pred::s0clang18::RecordField hi;
    hi.name = "hi";
    hi.type = intType(8);
    if (illegal_pointer_field) hi.type.is_pointer = true;
    hi.loc = loc(22);
    pair.fields.push_back(std::move(hi));

    records.record_by_name.emplace("Pair", 0);
    records.records.push_back(std::move(pair));
    return records;
}

pred::s0clang18::FunctionReachabilityGraph makeReachability(bool include_helper) {
    pred::s0clang18::FunctionReachabilityGraph graph;

    pred::s0clang18::FunctionEntity top;
    top.id = 0;
    top.kind = pred::s0clang18::FunctionEntityKind::Top;
    top.key.stable_name = "top_hls_main";
    top.loc = loc(30);
    graph.top_function = top.id;
    graph.function_by_stable_name.emplace(top.key.stable_name, top.id);
    graph.functions.push_back(std::move(top));

    if (include_helper) {
        pred::s0clang18::FunctionEntity helper;
        helper.id = 1;
        helper.kind = pred::s0clang18::FunctionEntityKind::Helper;
        helper.key.stable_name = "helper_id";
        helper.loc = loc(35);
        graph.function_by_stable_name.emplace(helper.key.stable_name, helper.id);
        graph.functions.push_back(std::move(helper));

        pred::s0clang18::FunctionCallEdge edge;
        edge.caller = 0;
        edge.callee = 1;
        edge.loc = loc(40);
        graph.call_edges.push_back(edge);
    }
    return graph;
}

pred::v2::ExprPtr varRef(const std::string& name, pred::v2::TypeInfo type) {
    auto expr = pred::v2::make_var(name, std::move(type));
    expr->debug_loc = loc(50);
    return expr;
}

pred::v2::ExprPtr portRef(const std::string& name) {
    auto expr = varRef(name, intType(8));
    expr->global_port_name = name;
    return expr;
}

pred::v2::ExprPtr callExpr(const std::string& callee) {
    auto expr = std::make_shared<pred::v2::Expr>();
    expr->kind = pred::v2::ExprKind::Call;
    expr->callee = callee;
    expr->type = intType(8);
    expr->debug_loc = loc(60);
    return expr;
}

pred::v2::StmtPtr assignStmt(pred::v2::ExprPtr target, pred::v2::ExprPtr value) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    stmt->debug_loc = loc(70);
    return stmt;
}

pred::v2::StmtPtr declStmt(const std::string& name, pred::v2::TypeInfo type) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_type = std::move(type);
    stmt->debug_loc = loc(80);
    return stmt;
}

pred::v2::StmtPtr returnStmt(pred::v2::ExprPtr value) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Return;
    stmt->return_value = std::move(value);
    stmt->debug_loc = loc(90);
    return stmt;
}

std::shared_ptr<pred::v2::FunctionAST> makeHelper() {
    auto helper = std::make_shared<pred::v2::FunctionAST>();
    helper->name = "helper_id";
    helper->return_type = intType(8);
    pred::v2::ParamDecl param;
    param.name = "x";
    param.type = intType(8);
    param.debug_loc = loc(100);
    helper->params.push_back(param);
    helper->body.push_back(returnStmt(varRef("x", intType(8))));
    return helper;
}

pred::v2::FunctionAST makeValidFunction() {
    pred::v2::FunctionAST top;
    top.name = "top_hls_main";
    top.return_type = voidType();

    pred::v2::ParamDecl in;
    in.name = "in_value";
    in.type = intType(8);
    in.debug_loc = loc(110);
    top.params.push_back(in);

    pred::v2::ParamDecl out;
    out.name = "out_value";
    out.type = intType(8);
    out.is_output = true;
    out.direction = pred::v2::ParamDirection::Output;
    out.debug_loc = loc(111);
    top.params.push_back(out);

    auto call = callExpr("helper_id");
    call->args.push_back(portRef("in_value"));
    top.body.push_back(assignStmt(portRef("out_value"), std::move(call)));
    top.body.push_back(declStmt("pair", pairType()));
    top.helpers.push_back(makeHelper());
    return top;
}

bool hasIssueKind(const pred::s0clang18::SurfaceValidationResult& result,
                  pred::s0clang18::SurfaceIssueKind kind) {
    for (const auto& issue : result.issues) {
        if (issue.kind == kind) return true;
    }
    return false;
}

} // namespace

int main() {
    auto valid = pred::s0clang18::validateSurfaceAST(
        makeValidFunction(), makePorts(), makeRecords(false),
        makeReachability(true));
    if (!valid.ok()) {
        for (const auto& diagnostic : valid.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(valid.ok());

    auto invalid_function = makeValidFunction();
    invalid_function.body.push_back(
        declStmt("mystery", pred::v2::make_unknown_type("Mystery")));
    invalid_function.body.push_back(
        assignStmt(portRef("out_value"), callExpr("")));
    invalid_function.body.push_back(
        assignStmt(portRef("ghost_port"), varRef("in_value", intType(8))));
    invalid_function.helpers.push_back(makeHelper());
    invalid_function.helpers.back()->name = "helper_not_reachable";

    auto invalid = pred::s0clang18::validateSurfaceAST(
        invalid_function, makePorts(), makeRecords(true),
        makeReachability(true));
    CHECK(!invalid.ok());
    CHECK(!invalid.issues.empty());
    CHECK(!invalid.diagnostics.empty());
    CHECK(hasIssueKind(invalid, pred::s0clang18::SurfaceIssueKind::UnknownType));
    CHECK(hasIssueKind(invalid, pred::s0clang18::SurfaceIssueKind::UnresolvedCallee));
    CHECK(hasIssueKind(invalid, pred::s0clang18::SurfaceIssueKind::IllegalPort));
    CHECK(hasIssueKind(invalid,
                       pred::s0clang18::SurfaceIssueKind::IllegalReferenceOrPointer));
    CHECK(hasIssueKind(invalid, pred::s0clang18::SurfaceIssueKind::UnresolvedSymbol));

    std::cout << "s017validate_test passed\n";
    return 0;
}
