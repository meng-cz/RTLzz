#include "backend/beopt_predicate.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

using namespace pred::beir;
using namespace pred::beir::opt::predicate_detail;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __LINE__ << ": " << #condition << '\n'; \
        std::exit(1); \
    } \
} while (0)

static Operand signal(Program& program, const std::string& name, int width,
                      std::optional<Operation> driver = {}) {
    Signal value;
    value.id = program.signals.size();
    value.name = name;
    value.type = {width, {}};
    value.driver = std::move(driver);
    program.signals.push_back(value);

    Operand result;
    result.kind = OperandKind::Symbol;
    result.node = value.id;
    result.text = name;
    result.type = value.type;
    return result;
}

static Operand literal(std::uint64_t value, int width) {
    Operand result;
    result.kind = OperandKind::Literal;
    result.type = {width, {}};
    result.constant.width = width;
    result.constant.limbs = {value};
    return result;
}

static Operand binary(Program& program, const std::string& name, OpCode opcode,
                      Operand lhs, Operand rhs) {
    Operation driver;
    driver.kind = OperationKind::Binary;
    driver.op = opcode;
    driver.operands = {std::move(lhs), std::move(rhs)};
    driver.type = {1, {}};
    return signal(program, name, 1, std::move(driver));
}

int main() {
    Program program;
    const auto a = signal(program, "a", 1);
    const auto b = signal(program, "b", 1);
    const auto both = binary(program, "both", OpCode::LogicAnd, a, b);
    const auto selector = signal(program, "selector", 2);
    const auto eq0 = binary(program, "eq0", OpCode::Eq, selector, literal(0, 2));
    const auto eq1 = binary(program, "eq1", OpCode::Eq, selector, literal(1, 2));

    PredicateRelations relations(program);
    CHECK(relations.implies(a, a) == Proof::Proven);
    CHECK(relations.implies(both, a) == Proof::Proven);
    CHECK(relations.isExclusive(a, b) == Proof::Unknown);
    CHECK(relations.isExclusive(eq0, eq1) == Proof::Proven);

    // Structural fast-path results must use the same true/false ordering as SAT.
    SnapshotPredicateRelations snapshot(program);
    for (bool truth : {false, true}) {
        auto context = guardedContext(a, truth);
        normalizeContext(context);
        CHECK(snapshot.classify(context, a) == relations.classifyDetailed(context, a));
    }

    // A relation object can be queried again after its BEIR graph changes.
    program.signal(eq1.node).driver->operands[1] = literal(0, 2);
    CHECK(relations.isExclusive(eq0, eq1) == Proof::Unknown);
    CHECK(relations.implies(a, a) == Proof::Proven);

    // Large conditions must be proved without enumerating every atom value.
    Operand wide_or = a;
    for (int i = 0; i < 128; ++i) {
        const auto input = signal(program, "wide_" + std::to_string(i), 1);
        wide_or = binary(program, "wide_or_" + std::to_string(i),
                         OpCode::LogicOr, wide_or, input);
    }
    CHECK(relations.impliesDetailed(guardedContext(a, true), {wide_or, true})
          == ProofOutcome::Proven);
    PredicateRelations generous_relations(program, false, {512, 256});
    CHECK(generous_relations.implies(a, wide_or) == Proof::Proven);
    const auto possible = generous_relations.classify(unconditionalContext(), wide_or);
    CHECK(possible.first == Proof::Unknown && possible.second == Proof::Unknown);

    PredicateRelations formula_limited(program, true, {64, 256});
    CHECK(formula_limited.impliesDetailed(guardedContext(a, true), {wide_or, true})
          == ProofOutcome::ResourceLimit);
    CHECK(formula_limited.implies(guardedContext(a, true), {wide_or, true})
          == Proof::Unknown);
    const auto limited_classification =
        formula_limited.classifyDetailed(unconditionalContext(), wide_or);
    CHECK(limited_classification.first == ProofOutcome::ResourceLimit);
    CHECK(limited_classification.second == ProofOutcome::ResourceLimit);
    // The cached large formula must not count against a smaller later query.
    CHECK(formula_limited.impliesDetailed(guardedContext(a, true), {a, true})
          == ProofOutcome::Proven);
    CHECK(formula_limited.impliesDetailed(unconditionalContext(), {a, true})
          == ProofOutcome::Counterexample);
    PredicateRelations atom_limited(program, true, {512, 16});
    CHECK(atom_limited.impliesDetailed(guardedContext(a, true), {wide_or, true})
          == ProofOutcome::ResourceLimit);
    CHECK(atom_limited.impliesDetailed(guardedContext(a, true), {a, true})
          == ProofOutcome::Proven);
    PredicateRelations retried(program, false, {512, 256});
    CHECK(retried.impliesDetailed(guardedContext(a, true), {wide_or, true})
          == ProofOutcome::Proven);

    // Compare SAT proofs with exhaustive truth tables on a small mixed formula.
    const auto c = signal(program, "c", 1);
    Operation not_driver;
    not_driver.kind = OperationKind::Unary;
    not_driver.op = OpCode::LogicNot;
    not_driver.type = {1, {}};
    not_driver.operands = {a};
    const auto not_a = signal(program, "not_a", 1, not_driver);
    const auto left = binary(program, "left", OpCode::LogicOr, a, b);
    const auto right = binary(program, "right", OpCode::LogicOr, not_a, c);
    const auto mixed = binary(program, "mixed", OpCode::LogicAnd, left, right);
    for (int a_fact = -1; a_fact <= 1; ++a_fact)
        for (int b_fact = -1; b_fact <= 1; ++b_fact)
            for (int c_fact = -1; c_fact <= 1; ++c_fact)
                for (bool expected : {false, true}) {
                    Context context = unconditionalContext();
                    if (a_fact >= 0) context = branchContext(context, a, a_fact != 0);
                    if (b_fact >= 0) context = branchContext(context, b, b_fact != 0);
                    if (c_fact >= 0) context = branchContext(context, c, c_fact != 0);
                    bool has_counterexample = false;
                    for (int bits = 0; bits < 8; ++bits) {
                        const bool av = (bits & 1) != 0;
                        const bool bv = (bits & 2) != 0;
                        const bool cv = (bits & 4) != 0;
                        if ((a_fact >= 0 && av != (a_fact != 0)) ||
                            (b_fact >= 0 && bv != (b_fact != 0)) ||
                            (c_fact >= 0 && cv != (c_fact != 0))) continue;
                        if (((av || bv) && (!av || cv)) != expected)
                            has_counterexample = true;
                    }
                    CHECK((relations.implies(context, {mixed, expected}) == Proof::Proven)
                          == !has_counterexample);
                }

    // Demand contexts are classified against one snapshot, then rewritten.
    Program nested;
    const auto x = signal(nested, "x", 1);
    const auto y = signal(nested, "y", 1);
    const auto xy = binary(nested, "xy", OpCode::LogicAnd, x, y);
    Operation inner_op;
    inner_op.kind = OperationKind::Ite;
    inner_op.type = {8, {}};
    inner_op.operands = {xy, literal(9, 8), literal(10, 8)};
    const auto inner = signal(nested, "inner", 8, inner_op);
    Operation middle_op = inner_op;
    middle_op.operands = {y, inner, literal(9, 8)};
    const auto middle = signal(nested, "middle", 8, middle_op);
    Operation outer_op = inner_op;
    outer_op.operands = {x, middle, literal(9, 8)};
    const auto outer = signal(nested, "outer", 8, outer_op);
    nested.outputs.push_back(outer.text);
    Operand large_guard = x;
    for (int i = 0; i < 40; ++i) {
        const auto input = signal(nested, "large_" + std::to_string(i), 1);
        large_guard = binary(nested, "large_or_" + std::to_string(i),
                             OpCode::LogicOr, large_guard, input);
    }
    Operation large_op = inner_op;
    large_op.operands = {large_guard, literal(11, 8), literal(12, 8)};
    const auto large = signal(nested, "large", 8, large_op);
    Operation output_op;
    output_op.kind = OperationKind::Assign;
    output_op.type = {8, {}};
    output_op.operands = {large};
    const auto large_output = signal(nested, "large_output", 8, output_op);
    nested.outputs.push_back(large_output.text);
    const auto unconditional = unconditionallyReachable(MutableProgram(nested));
    CHECK(unconditional[outer.node]);
    CHECK(unconditional[large.node]);
    CHECK(!unconditional[inner.node]);
    std::vector<bool> candidates(nested.signals.size(), false);
    candidates[inner.node] = true;
    const auto relevant = relevantToCandidates(nested, candidates);
    CHECK(relevant[outer.node] && relevant[middle.node] && relevant[inner.node]);
    CHECK(!relevant[large_output.node] && !relevant[large.node]);
    const auto restricted_contexts = analyzeDemandContexts(MutableProgram(nested), &relevant);
    CHECK(!restricted_contexts[inner.node].empty());
    CHECK(restricted_contexts[large.node].empty());
    CHECK(restricted_contexts[xy.node].empty());
    PredicateSupport nested_support(nested);
    CHECK(mayBenefitFromProof(xy, {guardedContext(x, true)}, nested_support));
    CHECK(mayBenefitFromProof(xy, {guardedContext(large_guard, true)}, nested_support));
    CHECK(!mayBenefitFromProof(xy, {guardedContext(large_output, true)}, nested_support));
    PredicateSupport selector_support(program);
    CHECK(mayBenefitFromProof(eq1, {guardedContext(eq0, true)}, selector_support));
    MutableProgram limited_graph(nested);
    CHECK(pred::beir::opt::sinkPredicates(limited_graph, {64, 16}));
    CHECK(limited_graph.program().signal(inner.node).driver->kind == OperationKind::Assign);
    CHECK(limited_graph.program().signal(large.node).driver->kind == OperationKind::Ite);
    MutableProgram graph(std::move(nested));
    CHECK(pred::beir::opt::sinkPredicates(graph));
    const auto& rewritten = *graph.program().signal(inner.node).driver;
    CHECK(rewritten.kind == OperationKind::Assign);
    CHECK(rewritten.operands[0].kind == OperandKind::Literal);
    CHECK(rewritten.operands[0].constant.toU64() == 9);

    std::cout << "BEIR predicate tests passed\n";
}
