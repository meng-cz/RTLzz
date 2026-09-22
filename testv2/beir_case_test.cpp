#include "backend/beopt_algebraic.hpp"
#include "backend/beopt_constant.hpp"
#include "backend/beopt_cse.hpp"
#include "backend/beopt_dce.hpp"
#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"
#include "backend/rtlgen.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace pred::beir;
using namespace pred::beir::opt;

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while (0)

static Operand literal(std::uint64_t value, int width) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type = {width, {}};
    operand.constant.width = width;
    operand.constant.limbs = {value};
    return operand;
}

static Operand signal(Program& program, std::string name, int width,
                      std::optional<Operation> driver = {}) {
    Signal signal;
    signal.id = program.signals.size();
    signal.name = std::move(name);
    signal.type = {width, {}};
    signal.driver = std::move(driver);
    program.signals.push_back(signal);
    Operand operand;
    operand.kind = OperandKind::Symbol;
    operand.node = signal.id;
    operand.text = signal.name;
    operand.type = signal.type;
    return operand;
}

static Operand caseNode(Program& program, std::vector<Operand> operands, int width) {
    Operation operation;
    operation.kind = OperationKind::Case;
    operation.type = {width, {}};
    operation.operands = std::move(operands);
    return signal(program, "case_" + std::to_string(program.signals.size()),
                  width, std::move(operation));
}

static bool knownBit(const std::vector<std::uint64_t>& limbs, int bit) {
    const std::size_t limb = static_cast<std::size_t>(bit / 64);
    return limb < limbs.size() && ((limbs[limb] >> (bit % 64)) & 1ULL) != 0;
}

static void valueFacts() {
    Program program;
    auto a = signal(program, "a", 1);
    auto b = signal(program, "b", 1);
    auto root = caseNode(program, {a, literal(0xa1, 8), b, literal(0xa2, 8),
                                   literal(0xa3, 8)}, 8);
    MutableProgram graph(program);
    graph.ensureValueFacts();
    const ValueFacts& facts = graph.program().signal(root.node).value;
    CHECK(facts.valid && !facts.constant);
    CHECK(knownBit(facts.known_one, 7));
    CHECK(knownBit(facts.known_zero, 6));
    CHECK(knownBit(facts.known_one, 5));
    CHECK(knownBit(facts.known_zero, 4));
}

static void constantPropagation() {
    Program program;
    auto root = caseNode(program,
                         {literal(0, 1), literal(11, 8),
                          literal(1, 1), literal(22, 8),
                          literal(33, 8)}, 8);
    MutableProgram graph(program);
    graph.ensureValueFacts();
    CHECK(graph.program().signal(root.node).value.constant);
    CHECK(graph.program().signal(root.node).value.value.toU64() == 22);
    CHECK(foldConstants(graph));
    const Operation& folded = *graph.program().signal(root.node).driver;
    CHECK(folded.kind == OperationKind::Assign);
    CHECK(folded.operands[0].constant.toU64() == 22);
}

static void addCarryConstantPropagation() {
    Program program;
    Operation op;
    op.kind = OperationKind::AddCarry;
    op.type = {8, {}};
    op.operands = {literal(255, 8), literal(0, 8), literal(1, 1)};
    auto result = signal(program, "addcarry_result", 8, op);
    MutableProgram graph(program);
    graph.ensureValueFacts();
    CHECK(graph.program().signal(result.node).value.constant);
    CHECK(graph.program().signal(result.node).value.value.toU64() == 0);
    CHECK(foldConstants(graph));
    CHECK(graph.program().signal(result.node).driver->kind == OperationKind::Assign);
}

static void algebraicSimplification() {
    Program program;
    auto a = signal(program, "a", 1);
    auto b = signal(program, "b", 1);
    auto root = caseNode(program,
                         {a, literal(5, 8), literal(1, 1), literal(7, 8),
                          b, literal(9, 8), literal(11, 8)}, 8);
    MutableProgram graph(program);
    CHECK(simplifyAlgebraicIdentities(graph));
    const Operation& simplified = *graph.program().signal(root.node).driver;
    CHECK(simplified.kind == OperationKind::Case);
    CHECK(caseBranchCount(simplified) == 1);
    CHECK(simplified.operands.back().constant.toU64() == 7);

    Program identical;
    auto condition = signal(identical, "condition", 1);
    auto same = caseNode(identical, {condition, literal(3, 8), literal(3, 8)}, 8);
    MutableProgram identical_graph(identical);
    CHECK(simplifyAlgebraicIdentities(identical_graph));
    CHECK(identical_graph.program().signal(same.node).driver->kind == OperationKind::Assign);
}

static void widthPropagation() {
    Program program;
    auto condition = signal(program, "condition", 1);
    auto lhs = signal(program, "lhs", 16);
    auto rhs = signal(program, "rhs", 16);
    auto selected = caseNode(program, {condition, lhs, rhs}, 16);
    Operation trunc;
    trunc.kind = OperationKind::Trunc;
    trunc.type = {4, {}};
    trunc.to_width = 4;
    trunc.operands = {selected};
    auto output = signal(program, "output", 4, trunc);
    program.outputs.push_back(output.text);

    MutableProgram graph(program);
    CHECK(simplifyWidthOperations(graph));
    const Signal& narrowed = graph.program().signal(selected.node);
    CHECK(narrowed.type.width == 4);
    CHECK(narrowed.driver->type.width == 4);
    CHECK(hasValidCaseShape(*narrowed.driver));
    CHECK(narrowed.driver->operands[0].type.width == 1);
    CHECK(narrowed.driver->operands[1].type.width == 4);
    CHECK(narrowed.driver->operands[2].type.width == 4);
}

static void signedShiftRetainsSignBit() {
    Program program;
    program.function_name = "signed_shift_width";
    auto input = signal(program, "input_value", 32);
    input.signed_view = true;
    Operation shift_op;
    shift_op.kind = OperationKind::Binary;
    shift_op.op = OpCode::Shr;
    shift_op.type = {32, {}};
    shift_op.operands = {input, literal(1, 32)};
    auto shifted = signal(program, "shifted", 32, shift_op);
    Operation assign;
    assign.kind = OperationKind::Assign;
    assign.type = {32, {}};
    assign.operands = {shifted};
    auto output = signal(program, "output", 32, assign);
    program.outputs.push_back(output.text);

    MutableProgram graph(program);
    simplifyWidthOperations(graph);
    const auto& result = graph.program().signal(shifted.node);
    CHECK(result.type.width == 32);
    CHECK(result.driver->kind == OperationKind::Binary);
    CHECK(result.driver->op == OpCode::Shr);
    CHECK(result.driver->operands[0].signed_view);
    const std::string rtl = pred::rtlgen::emitSystemVerilog(graph.program());
    CHECK(rtl.find("32'(($signed(input_value) >>> 32'h1))") != std::string::npos);
}

static void rtlEmission() {
    Program program;
    program.function_name = "case_test";
    auto condition = signal(program, "condition", 1);
    auto root = caseNode(program, {condition, literal(1, 8), literal(2, 8)}, 8);
    program.outputs.push_back(root.text);
    const std::string rtl = pred::rtlgen::emitSystemVerilog(program);
    CHECK(rtl.find("always_comb begin") != std::string::npos);
    CHECK(rtl.find("case (1'b1)") != std::string::npos);
    CHECK(rtl.find("default:") != std::string::npos);
    CHECK(emitText(program).find("driver case") != std::string::npos);
}

static void moduleBodyEmission() {
    Program program;
    program.function_name = "inner_logic";
    program.ports.push_back({"input_port", PortDirection::Input, {8, {}}, {}});
    program.ports.push_back({"output_port", PortDirection::Output, {8, {}}, {}});
    const std::string body = pred::rtlgen::emitSystemVerilogBody(
        program, {{"input_port", "register_data"}, {"output_port", "output_port"}});
    CHECK(body.find("module ") == std::string::npos);
    CHECK(body.find("endmodule") == std::string::npos);
    CHECK(body.find("logic [7:0] input_port;") != std::string::npos);
    CHECK(body.find("assign input_port = register_data;") != std::string::npos);
    CHECK(body.find("logic [7:0] output_port;") == std::string::npos);
}

static void graphOptimizations() {
    Program program;
    auto condition = signal(program, "condition", 1);
    auto first = caseNode(program, {condition, literal(1, 8), literal(2, 8)}, 8);
    auto duplicate = caseNode(program, {condition, literal(1, 8), literal(2, 8)}, 8);
    Operation combine;
    combine.kind = OperationKind::Binary;
    combine.op = OpCode::BitXor;
    combine.type = {8, {}};
    combine.operands = {first, duplicate};
    auto output = signal(program, "output", 8, combine);
    program.outputs.push_back(output.text);
    MutableProgram graph(program);
    CHECK(mergeCommonExpressions(graph));
    const Operation& root = *graph.program().signal(output.node).driver;
    CHECK(root.operands[0].node == root.operands[1].node);
    CHECK(eliminateDeadNodes(graph));
}

static void predicateContexts() {
    using namespace predicate_detail;
    Program program;
    auto a = signal(program, "a", 1);
    auto b = signal(program, "b", 1);
    auto on_a = signal(program, "on_a", 8);
    auto on_b = signal(program, "on_b", 8);
    auto fallback = signal(program, "fallback", 8);
    auto root = caseNode(program, {a, on_a, b, on_b, fallback}, 8);
    program.outputs.push_back(root.text);
    MutableProgram graph(program);
    const auto contexts = analyzeDemandContexts(graph);
    PredicateRelations relations(graph.program());
    CHECK(contexts[on_a.node].size() == 1);
    CHECK(relations.implies(contexts[on_a.node][0], {a, true}) == Proof::Proven);
    CHECK(contexts[on_b.node].size() == 1);
    CHECK(relations.implies(contexts[on_b.node][0], {a, false}) == Proof::Proven);
    CHECK(relations.implies(contexts[on_b.node][0], {b, true}) == Proof::Proven);
    CHECK(contexts[fallback.node].size() == 1);
    CHECK(relations.implies(contexts[fallback.node][0], {a, false}) == Proof::Proven);
    CHECK(relations.implies(contexts[fallback.node][0], {b, false}) == Proof::Proven);
}

int main() {
    valueFacts();
    constantPropagation();
    addCarryConstantPropagation();
    algebraicSimplification();
    widthPropagation();
    signedShiftRetainsSignBit();
    rtlEmission();
    moduleBodyEmission();
    graphOptimizations();
    predicateContexts();
    std::cout << "BEIR case tests passed\n";
}
