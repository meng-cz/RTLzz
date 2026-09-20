#include "backend/beopt.hpp"
#include "backend/beopt_bit_updates.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace pred::beir;
using namespace pred::beir::opt;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " \
                  << #condition << "\n"; \
        std::exit(1); \
    } \
} while (false)

static Operand signal(Program& program, std::string name, int width,
                      std::optional<Operation> driver = {}) {
    Signal value;
    value.id = program.signals.size();
    value.name = std::move(name);
    value.type = ValueType{width, {}};
    value.driver = std::move(driver);
    program.signals.push_back(value);
    Operand out;
    out.kind = OperandKind::Symbol;
    out.node = value.id;
    out.text = value.name;
    out.type = value.type;
    return out;
}

static Operand write(Program& program, Operand base, int lo, int width,
                     Operand value) {
    Operation op;
    op.kind = OperationKind::WriteSlice;
    op.type = base.type;
    op.lo = lo;
    op.hi = lo + width - 1;
    op.operands = {std::move(base), std::move(value)};
    return signal(program, "update_" + std::to_string(program.signals.size()),
                  op.type.width, std::move(op));
}

static Operand assign(Program& program, Operand value) {
    Operation op;
    op.kind = OperationKind::Assign;
    op.type = value.type;
    op.operands = {std::move(value)};
    return signal(program, "alias_" + std::to_string(program.signals.size()),
                  op.type.width, std::move(op));
}

static void makeOutput(Program& program, const Operand& value) {
    program.outputs.push_back(program.signal(value.node).name);
}

static std::uint64_t mask(int width) {
    return width >= 64 ? ~std::uint64_t{0} :
                         ((std::uint64_t{1} << width) - 1);
}

static std::uint64_t evaluate(const Program& program,
                              const std::string& output,
                              const std::map<std::string, std::uint64_t>& inputs) {
    std::map<NodeId, std::uint64_t> cache;
    std::function<std::uint64_t(const Operand&)> eval_operand;
    std::function<std::uint64_t(NodeId)> eval_signal = [&](NodeId id) {
        if (cache.count(id)) return cache[id];
        const auto& current = program.signal(id);
        if (!current.driver) return cache[id] = inputs.at(current.name) & mask(current.type.width);
        const auto& op = *current.driver;
        auto arg = [&](std::size_t index) { return eval_operand(op.operands.at(index)); };
        std::uint64_t result = 0;
        switch (op.kind) {
        case OperationKind::Assign:
        case OperationKind::Cast:
        case OperationKind::ZExt:
        case OperationKind::Trunc:
            result = arg(0);
            break;
        case OperationKind::Slice:
            result = arg(0) >> op.lo;
            break;
        case OperationKind::Concat:
            for (std::size_t i = 0; i < op.operands.size(); ++i) {
                result = (result << op.operands[i].type.width) | arg(i);
            }
            break;
        case OperationKind::WriteSlice: {
            const int width = op.hi - op.lo + 1;
            const auto field_mask = mask(width) << op.lo;
            result = (arg(0) & ~field_mask) | ((arg(1) & mask(width)) << op.lo);
            break;
        }
        default:
            CHECK(false);
        }
        return cache[id] = result & mask(current.type.width);
    };
    eval_operand = [&](const Operand& value) {
        return value.kind == OperandKind::Literal
            ? value.constant.toU64() & mask(value.type.width)
            : eval_signal(value.node);
    };
    for (const auto& current : program.signals) {
        if (current.name == output) return eval_signal(current.id);
    }
    CHECK(false);
    return 0;
}

static unsigned reachableKind(const Program& program, NodeId root,
                              OperationKind kind) {
    std::vector<NodeId> pending{root};
    std::vector<bool> seen(program.signals.size(), false);
    unsigned count = 0;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (seen[id]) continue;
        seen[id] = true;
        const auto& current = program.signal(id);
        if (!current.driver) continue;
        if (current.driver->kind == kind) ++count;
        for (const auto& operand : current.driver->operands) {
            if (operand.kind == OperandKind::Symbol) pending.push_back(operand.node);
        }
    }
    return count;
}

static void checkEquivalent(const Program& before, const Program& after,
                            const std::string& output) {
    for (unsigned base = 0; base < 256; ++base) {
        for (unsigned a = 0; a < 16; ++a) {
            for (unsigned b = 0; b < 16; ++b) {
                std::map<std::string, std::uint64_t> inputs{
                    {"base", base}, {"a", a}, {"b", b}, {"c", base ^ a ^ b}};
                CHECK(evaluate(before, output, inputs) ==
                      evaluate(after, output, inputs));
            }
        }
    }
}

static Program adjacentProgram() {
    Program program;
    auto base = signal(program, "base", 32);
    auto a = signal(program, "a", 8);
    auto b = signal(program, "b", 8);
    auto c = signal(program, "c", 8);
    auto current = write(program, base, 0, 8, a);
    current = assign(program, current);
    current = write(program, current, 8, 8, b);
    current = assign(program, current);
    current = write(program, current, 16, 8, c);
    makeOutput(program, current);
    return program;
}

static void adjacentWritesBecomeOneComposition() {
    auto before = adjacentProgram();
    MutableProgram graph(before);
    CHECK(coalesceBitRangeUpdates(graph));
    CHECK(!coalesceBitRangeUpdates(graph));
    auto after = graph.finish();
    const auto output_id = before.signals.back().id;
    CHECK(after.signal(output_id).driver->kind == OperationKind::Concat);
    CHECK(after.signal(output_id).driver->operands.size() == 4);
    CHECK(reachableKind(after, output_id, OperationKind::WriteSlice) == 0);
    checkEquivalent(before, after, before.outputs.front());
}

static void overlapsUseNewestValueAndPreserveGaps() {
    Program before;
    auto base = signal(before, "base", 16);
    auto a = signal(before, "a", 8);
    auto b = signal(before, "b", 8);
    auto c = signal(before, "c", 4);
    auto first = write(before, base, 0, 8, a);
    auto second = write(before, first, 4, 8, b);
    auto root = write(before, second, 14, 2, c);
    makeOutput(before, root);
    MutableProgram graph(before);
    CHECK(coalesceBitRangeUpdates(graph));
    auto after = graph.finish();
    CHECK(after.signal(root.node).driver->kind == OperationKind::Concat);
    CHECK(reachableKind(after, root.node, OperationKind::WriteSlice) == 0);
    checkEquivalent(before, after, before.outputs.front());
}

static void fullCoverageDropsTheOldBase() {
    Program program;
    auto base = signal(program, "base", 16);
    auto a = signal(program, "a", 8);
    auto b = signal(program, "b", 8);
    auto low = write(program, base, 0, 8, a);
    auto root = write(program, low, 8, 8, b);
    makeOutput(program, root);
    MutableProgram graph(program);
    CHECK(coalesceBitRangeUpdates(graph));
    const auto& driver = *graph.program().signal(root.node).driver;
    CHECK(driver.kind == OperationKind::Concat);
    for (const auto& operand : driver.operands) {
        CHECK(operand.node != base.node);
    }
}

static void sharedIntermediateIsNotAbsorbed() {
    Program before;
    auto base = signal(before, "base", 16);
    auto a = signal(before, "a", 8);
    auto b = signal(before, "b", 8);
    auto first = write(before, base, 0, 8, a);
    auto root = write(before, first, 8, 8, b);
    makeOutput(before, first);
    makeOutput(before, root);
    MutableProgram graph(before);
    CHECK(coalesceBitRangeUpdates(graph));
    auto after = graph.finish();
    CHECK(after.signal(first.node).driver->kind == OperationKind::Concat);
    CHECK(after.signal(root.node).driver->kind == OperationKind::Concat);
    for (unsigned base_value = 0; base_value < 256; ++base_value) {
        std::map<std::string, std::uint64_t> inputs{
            {"base", base_value}, {"a", base_value ^ 0x55}, {"b", base_value ^ 0xaa}};
        for (const auto& output : before.outputs) {
            CHECK(evaluate(before, output, inputs) == evaluate(after, output, inputs));
        }
    }
}

static void boundsAndOptionsAreConservative() {
    auto program = adjacentProgram();
    MutableProgram disabled(program);
    CHECK(!coalesceBitRangeUpdates(disabled, 0, 64));
    CHECK(disabled.program().signal(program.signals.back().id).driver->kind ==
          OperationKind::WriteSlice);
    MutableProgram pieces(program);
    CHECK(coalesceBitRangeUpdates(pieces, 32, 2));
    // The newest large composition is skipped, but smaller predecessor writes
    // may still be lowered safely; the output remains a WriteSlice expression.
    CHECK(pieces.program().signal(program.signals.back().id).driver->kind ==
          OperationKind::WriteSlice);
    auto none = parseOptions({"none"});
    CHECK(!none.bit_range_update_coalescing);
    auto enabled = parseOptions({"none", "bit-updates"});
    CHECK(enabled.bit_range_update_coalescing);
}

int main() {
    adjacentWritesBecomeOneComposition();
    overlapsUseNewestValueAndPreserveGaps();
    fullCoverageDropsTheOldBase();
    sharedIntermediateIsNotAbsorbed();
    boundsAndOptionsAreConservative();
    return 0;
}
