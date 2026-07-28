#include "s0clang18/s016portlift.hpp"

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

static pred::v2::TypeInfo intType(int width) {
    return pred::v2::make_hw_type("Int", width, false);
}

static pred::v2::ExprPtr portRef(const std::string& name,
                                 pred::v2::TypeInfo type) {
    auto expr = pred::v2::make_var(name, std::move(type));
    expr->global_port_name = name;
    return expr;
}

static pred::v2::StmtPtr returnStmt(pred::v2::ExprPtr value) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Return;
    stmt->return_value = std::move(value);
    return stmt;
}

static pred::v2::StmtPtr assignStmt(pred::v2::ExprPtr target,
                                    pred::v2::ExprPtr value) {
    auto stmt = std::make_shared<pred::v2::Stmt>();
    stmt->kind = pred::v2::StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    return stmt;
}

static pred::v2::ExprPtr callExpr(const std::string& callee,
                                  pred::v2::TypeInfo type) {
    auto expr = std::make_shared<pred::v2::Expr>();
    expr->kind = pred::v2::ExprKind::Call;
    expr->callee = callee;
    expr->type = std::move(type);
    return expr;
}

static pred::s0clang18::RawPortDecl rawPort(
    const std::string& name,
    pred::s0clang18::PortDirection direction,
    pred::v2::TypeInfo type) {
    pred::s0clang18::RawPortDecl port;
    port.name = name;
    port.direction = direction;
    port.type = std::move(type);
    port.decl_loc.file = "s016_virtual_input.logic.cpp";
    port.decl_loc.line = 1;
    port.decl_loc.column = 1;
    return port;
}

static pred::s0clang18::PortDeclTable makePorts() {
    pred::s0clang18::PortDeclTable ports;
    ports.ports.push_back(rawPort(
        "in_value", pred::s0clang18::PortDirection::Input, intType(8)));

    pred::v2::TypeInfo lanes = intType(8);
    lanes.is_array = true;
    lanes.array_size = 2;
    lanes.array_dims = {2};
    ports.ports.push_back(rawPort(
        "lanes", pred::s0clang18::PortDirection::Input, lanes));

    ports.ports.push_back(rawPort(
        "out_value", pred::s0clang18::PortDirection::Output, intType(8)));

    for (std::size_t i = 0; i < ports.ports.size(); ++i) {
        ports.port_by_name.emplace(ports.ports[i].name, i);
    }
    return ports;
}

static pred::s0clang18::FunctionReachabilityGraph makeReachability() {
    pred::s0clang18::FunctionReachabilityGraph graph;

    pred::s0clang18::FunctionEntity top;
    top.id = 0;
    top.kind = pred::s0clang18::FunctionEntityKind::Top;
    top.key.stable_name = "top_hls_main";
    graph.top_function = top.id;
    graph.function_by_stable_name.emplace(top.key.stable_name, top.id);
    graph.functions.push_back(std::move(top));

    pred::s0clang18::FunctionEntity mid;
    mid.id = 1;
    mid.kind = pred::s0clang18::FunctionEntityKind::Helper;
    mid.key.stable_name = "helper_mid";
    graph.function_by_stable_name.emplace(mid.key.stable_name, mid.id);
    graph.functions.push_back(std::move(mid));

    pred::s0clang18::FunctionEntity leaf;
    leaf.id = 2;
    leaf.kind = pred::s0clang18::FunctionEntityKind::Helper;
    leaf.key.stable_name = "helper_leaf";
    graph.function_by_stable_name.emplace(leaf.key.stable_name, leaf.id);
    graph.functions.push_back(std::move(leaf));

    pred::s0clang18::FunctionCallEdge top_to_mid;
    top_to_mid.caller = 0;
    top_to_mid.callee = 1;
    graph.call_edges.push_back(top_to_mid);

    pred::s0clang18::FunctionCallEdge mid_to_leaf;
    mid_to_leaf.caller = 1;
    mid_to_leaf.callee = 2;
    graph.call_edges.push_back(mid_to_leaf);

    return graph;
}

static pred::v2::FunctionAST makeFunction() {
    pred::v2::FunctionAST top;
    top.name = "top_hls_main";
    top.return_type = pred::v2::make_unknown_type("void");

    auto leaf = std::make_shared<pred::v2::FunctionAST>();
    leaf->name = "helper_leaf";
    leaf->return_type = intType(8);
    auto lane0 = pred::v2::make_array_access(
        portRef("lanes", intType(8)),
        pred::v2::make_literal("0", intType(32)),
        intType(8));
    leaf->body.push_back(returnStmt(pred::v2::make_binary(
        "^", portRef("in_value", intType(8)), std::move(lane0), intType(8))));

    auto mid = std::make_shared<pred::v2::FunctionAST>();
    mid->name = "helper_mid";
    mid->return_type = intType(8);
    mid->body.push_back(returnStmt(callExpr("helper_leaf", intType(8))));
    mid->helpers.push_back(std::move(leaf));

    auto call_mid = callExpr("helper_mid", intType(8));
    top.body.push_back(assignStmt(portRef("out_value", intType(8)),
                                  std::move(call_mid)));
    top.helpers.push_back(std::move(mid));
    return top;
}

static const pred::s0clang18::FunctionPortRequirement* requirementByName(
    const pred::s0clang18::PortLiftPlan& plan,
    const std::string& name) {
    for (const auto& requirement : plan.requirements) {
        if (requirement.function_name == name) return &requirement;
    }
    return nullptr;
}

static bool requiresPort(const pred::s0clang18::FunctionPortRequirement& requirement,
                         const std::string& port_name) {
    for (const auto& port : requirement.ports) {
        if (port.port_name == port_name) return true;
    }
    return false;
}

int main() {
    auto ports = makePorts();
    auto reachability = makeReachability();
    auto function = makeFunction();

    auto plan_result = pred::s0clang18::analyzeGlobalPortRequirements(
        function, ports, reachability);
    if (!plan_result.ok()) {
        for (const auto& diagnostic : plan_result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(plan_result.ok());
    CHECK(plan_result.value.has_value());
    const auto& plan = *plan_result.value;

    const auto* top_req = requirementByName(plan, "top_hls_main");
    const auto* mid_req = requirementByName(plan, "helper_mid");
    const auto* leaf_req = requirementByName(plan, "helper_leaf");
    CHECK(top_req != nullptr);
    CHECK(mid_req != nullptr);
    CHECK(leaf_req != nullptr);

    CHECK(requiresPort(*leaf_req, "in_value"));
    CHECK(requiresPort(*leaf_req, "lanes"));
    CHECK(!requiresPort(*leaf_req, "out_value"));
    CHECK(requiresPort(*mid_req, "in_value"));
    CHECK(requiresPort(*mid_req, "lanes"));
    CHECK(requiresPort(*top_req, "in_value"));
    CHECK(requiresPort(*top_req, "lanes"));
    CHECK(requiresPort(*top_req, "out_value"));

    auto lifted = pred::s0clang18::liftGlobalPorts(
        std::move(function), ports, plan);
    CHECK(lifted.diagnostics.empty());

    const auto& top = lifted.function;
    CHECK(top.params.size() == 3);
    CHECK(top.params[0].name == "in_value");
    CHECK(top.params[1].name == "lanes");
    CHECK(top.params[2].name == "out_value");
    CHECK(top.params[2].is_output);

    const auto& mid = *top.helpers.at(0);
    CHECK(mid.params.size() == 2);
    CHECK(mid.params[0].name == "__rtlzz_port_in_value");
    CHECK(mid.params[1].name == "__rtlzz_port_lanes");

    const auto& leaf = *mid.helpers.at(0);
    CHECK(leaf.params.size() == 2);
    CHECK(leaf.params[0].name == "__rtlzz_port_in_value");
    CHECK(leaf.params[1].name == "__rtlzz_port_lanes");

    const auto& top_assign = top.body.at(0);
    CHECK(top_assign->assign_target->var_name == "out_value");
    CHECK(top_assign->assign_target->global_port_name.empty());
    CHECK(top_assign->assign_value->args.size() == 2);
    CHECK(top_assign->assign_value->args[0]->var_name == "in_value");
    CHECK(top_assign->assign_value->args[1]->var_name == "lanes");

    const auto& mid_return = mid.body.at(0)->return_value.value();
    CHECK(mid_return->args.size() == 2);
    CHECK(mid_return->args[0]->var_name == "__rtlzz_port_in_value");
    CHECK(mid_return->args[1]->var_name == "__rtlzz_port_lanes");

    const auto& leaf_return = leaf.body.at(0)->return_value.value();
    CHECK(leaf_return->left->var_name == "__rtlzz_port_in_value");
    CHECK(leaf_return->left->global_port_name.empty());
    CHECK(leaf_return->right->array_base->var_name == "__rtlzz_port_lanes");
    CHECK(leaf_return->right->array_base->global_port_name.empty());

    std::cout << "s016portlift_test passed\n";
    return 0;
}
