#include "backend/beopt_boolean.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>

using namespace pred::beir;
using namespace pred::beir::opt;

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while (0)

static Operand input(Program& program, const std::string& name) {
    Signal signal;
    signal.id = program.signals.size();
    signal.name = name;
    signal.type = {1, {}};
    program.signals.push_back(signal);
    Operand value;
    value.kind = OperandKind::Symbol;
    value.node = signal.id;
    value.text = name;
    value.type = signal.type;
    return value;
}

static Operand operation(Program& program, OperationKind kind, OpCode opcode,
                         std::vector<Operand> operands) {
    Operation operation;
    operation.kind = kind;
    operation.op = opcode;
    operation.type = {1, {}};
    operation.operands = std::move(operands);
    Signal signal;
    signal.id = program.signals.size();
    signal.name = "n" + std::to_string(signal.id);
    signal.type = operation.type;
    signal.driver = std::move(operation);
    program.signals.push_back(signal);
    Operand value;
    value.kind = OperandKind::Symbol;
    value.node = signal.id;
    value.text = signal.name;
    value.type = signal.type;
    return value;
}

static Operand logicalNot(Program& program, Operand value) {
    return operation(program, OperationKind::Unary, OpCode::LogicNot, {value});
}

static Operand binary(Program& program, OpCode opcode, Operand lhs, Operand rhs) {
    return operation(program, OperationKind::Binary, opcode, {lhs, rhs});
}

static Operand ite(Program& program, Operand condition, Operand when_true,
                   Operand when_false) {
    return operation(program, OperationKind::Ite, OpCode::None,
                     {condition, when_true, when_false});
}

static bool evaluate(const Program& program, NodeId root,
                     const std::map<std::string, bool>& inputs) {
    std::map<NodeId, bool> cache;
    std::function<bool(const Operand&)> operand;
    std::function<bool(NodeId)> node = [&](NodeId id) -> bool {
        if (auto found = cache.find(id); found != cache.end()) return found->second;
        const Signal& signal = program.signal(id);
        if (!signal.driver) return cache[id] = inputs.at(signal.name);
        const Operation& op = *signal.driver;
        auto get = [&](std::size_t index) { return operand(op.operands.at(index)); };
        bool value = false;
        if (op.kind == OperationKind::Assign) value = get(0);
        else if (op.kind == OperationKind::Ite) value = get(0) ? get(1) : get(2);
        else if (op.kind == OperationKind::Unary && op.op == OpCode::LogicNot)
            value = !get(0);
        else if (op.kind == OperationKind::Binary &&
                 (op.op == OpCode::BitAnd || op.op == OpCode::LogicAnd))
            value = get(0) && get(1);
        else if (op.kind == OperationKind::Binary &&
                 (op.op == OpCode::BitOr || op.op == OpCode::LogicOr))
            value = get(0) || get(1);
        else CHECK(false);
        return cache[id] = value;
    };
    operand = [&](const Operand& value) {
        return value.kind == OperandKind::Literal
            ? !value.constant.isZero() : node(value.node);
    };
    return node(root);
}

static bool reachableIte(const Program& program, NodeId root) {
    std::map<NodeId, bool> visited;
    std::function<bool(NodeId)> visit = [&](NodeId id) {
        if (visited[id]) return false;
        visited[id] = true;
        const Signal& signal = program.signal(id);
        if (!signal.driver) return false;
        if (signal.driver->kind == OperationKind::Ite) return true;
        for (const Operand& value : signal.driver->operands)
            if (value.kind == OperandKind::Symbol && visit(value.node)) return true;
        return false;
    };
    return visit(root);
}

static void checkEquivalent(const Program& before, const Program& after,
                            NodeId root, int input_count) {
    for (int bits = 0; bits < (1 << input_count); ++bits) {
        std::map<std::string, bool> inputs;
        for (int index = 0; index < input_count; ++index)
            inputs[std::string(1, static_cast<char>('a' + index))] =
                ((bits >> index) & 1) != 0;
        CHECK(evaluate(before, root, inputs) == evaluate(after, root, inputs));
    }
}

static void runToFixedPoint(MutableProgram& graph) {
    for (int round = 0; round < 8 && normalizeBooleanControl(graph); ++round) {}
}

static void guardedPhi() {
    Program program;
    auto a = input(program, "a");
    auto b = input(program, "b");
    auto c = input(program, "c");
    auto guard = binary(program, OpCode::LogicAnd, a, logicalNot(program, b));
    auto root = ite(program, guard, c, b);
    Program before = program;
    MutableProgram graph(program);
    runToFixedPoint(graph);
    CHECK(!reachableIte(graph.program(), root.node));
    checkEquivalent(before, graph.program(), root.node, 3);
}

static void directShortCircuitPhi() {
    Program program;
    auto a = input(program, "a");
    auto b = input(program, "b");
    auto root = ite(program, logicalNot(program, a), a, b);
    Program before = program;
    MutableProgram graph(program);
    runToFixedPoint(graph);
    CHECK(!reachableIte(graph.program(), root.node));
    checkEquivalent(before, graph.program(), root.node, 2);
}

static void booleanLaws() {
    for (bool consensus_case : {false, true}) {
        Program program;
        auto a = input(program, "a");
        auto b = input(program, "b");
        auto left = binary(program, OpCode::BitAnd, a, b);
        Operand root = consensus_case
            ? binary(program, OpCode::BitOr, left,
                     binary(program, OpCode::BitAnd, a, logicalNot(program, b)))
            : binary(program, OpCode::BitOr, a, left);
        Program before = program;
        MutableProgram graph(program);
        runToFixedPoint(graph);
        checkEquivalent(before, graph.program(), root.node, 2);
        CHECK(graph.program().signal(root.node).driver->kind == OperationKind::Assign);
    }
}

static void preservePriorityMux() {
    Program program;
    auto a = input(program, "a");
    auto b = input(program, "b");
    auto c = input(program, "c");
    auto root = ite(program, a, b, c);
    MutableProgram graph(program);
    CHECK(!normalizeBooleanControl(graph));
    CHECK(reachableIte(graph.program(), root.node));
}

int main() {
    guardedPhi();
    directShortCircuitPhi();
    booleanLaws();
    preservePriorityMux();
    std::cout << "beopt boolean tests passed\n";
}
