#pragma once

#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"
#include <functional>
#include <queue>

namespace pred::beir::opt {
namespace structure_detail {
inline std::unordered_set<NodeId> reachable(const MutableProgram& graph) {
    std::unordered_set<NodeId> live;
    std::vector<NodeId> pending;
    for (const auto& signal : graph.program().signals)
        if (graph.isObservable(signal)) pending.push_back(signal.id);
    while (!pending.empty()) {
        auto id = pending.back(); pending.pop_back();
        if (!live.insert(id).second) continue;
        const auto& signal = graph.program().signal(id);
        if (signal.driver) for (const auto& operand : signal.driver->operands)
            if (operand.kind == OperandKind::Symbol) pending.push_back(operand.node);
    }
    return live;
}
inline std::vector<unsigned> users(const MutableProgram& graph) {
    std::vector<unsigned> result(graph.program().signals.size());
    for (const auto& signal : graph.program().signals) {
        if (graph.isObservable(signal)) ++result[signal.id];
        if (signal.driver) for (const auto& operand : signal.driver->operands)
            if (operand.kind == OperandKind::Symbol) ++result.at(operand.node);
    }
    return result;
}
inline Operand emit(Program& program, OperationKind kind, OpCode opcode,
                    ValueType type, std::vector<Operand> operands, const DebugInfo& debug) {
    Operation op;
    op.kind = kind; op.op = opcode; op.type = type;
    op.operands = std::move(operands); op.debug = debug;
    return width_detail::appendTemp(program, type, std::move(op), "balanced BEIR expression");
}
inline Operand balanced(Program& program, std::vector<Operand> values, OpCode opcode,
                        ValueType type, const DebugInfo& debug) {
    while (values.size() > 1) {
        std::vector<Operand> next;
        for (std::size_t i = 0; i < values.size(); i += 2) {
            if (i + 1 == values.size()) next.push_back(values[i]);
            else next.push_back(emit(program, OperationKind::Binary, opcode, type,
                                     {values[i], values[i + 1]}, debug));
        }
        values = std::move(next);
    }
    return values.front();
}
inline void assign(Program& program, NodeId id, Operand value, const DebugInfo& debug) {
    Operation op;
    op.kind = OperationKind::Assign;
    op.type = program.signal(id).type;
    op.operands = {std::move(value)};
    op.debug = debug;
    program.signal(id).driver = std::move(op);
}
} // namespace structure_detail

inline bool parallelizeExclusiveMuxes(MutableProgram& graph, unsigned max_branches = 8) {
    using namespace structure_detail;
    if (max_branches < 3) return false;
    max_branches = std::min(max_branches, 8u);
    auto& program = graph.program();
    const auto live = reachable(graph);
    auto use_count = users(graph);
    auto order = width_detail::topologicalOrder(program);
    std::unordered_set<NodeId> consumed;
    bool changed = false;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const NodeId root = *it;
        if (!live.count(root) || consumed.count(root)) continue;
        const auto& signal = program.signal(root);
        if (!signal.driver || signal.driver->kind != OperationKind::Ite || signal.type.isArray()) continue;
        const auto type = signal.type;
        auto debug = signal.driver->debug;
        std::vector<Operand> conditions, data;
        std::vector<NodeId> chain;
        Operand fallback;
        NodeId current = root;
        bool valid = true;
        while (true) {
            const auto& op = *program.signal(current).driver;
            if (op.kind != OperationKind::Ite || op.operands.size() != 3 ||
                !predicate_detail::sameValueType(op.type, type) ||
                op.operands[0].type.width != 1 || op.operands[0].type.isArray() ||
                !predicate_detail::sameValueType(op.operands[1].type, type) ||
                !predicate_detail::sameValueType(op.operands[2].type, type)) { valid = false; break; }
            chain.push_back(current);
            conditions.push_back(op.operands[0]); data.push_back(op.operands[1]);
            fallback = op.operands[2];
            const auto* next = predicate_detail::symbolDriver(fallback, program);
            if (!next || next->kind != OperationKind::Ite || use_count[fallback.node] != 1) break;
            if (conditions.size() >= max_branches) { valid = false; break; }
            current = fallback.node;
        }
        if (!valid || conditions.size() < 3) continue;
        predicate_detail::PredicateRelations relations(program);
        for (std::size_t i = 0; valid && i < conditions.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (relations.isExclusive(conditions[i], conditions[j]) != predicate_detail::Proof::Proven) {
                    valid = false; break;
                }
        if (!valid) continue;
        addDebugMessage(debug, "parallelized proven-exclusive mux chain");
        const ValueType boolean{1, {}};
        auto any = balanced(program, conditions, OpCode::BitOr, boolean, debug);
        auto otherwise = emit(program, OperationKind::Unary, OpCode::LogicNot, boolean, {any}, debug);
        conditions.push_back(otherwise); data.push_back(fallback);
        std::vector<Operand> terms;
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            Operation mask;
            mask.kind = OperationKind::Repeat; mask.type = type;
            mask.times = type.width; mask.operands = {conditions[i]}; mask.debug = debug;
            auto bits = width_detail::appendTemp(program, type, std::move(mask), "mux branch mask");
            terms.push_back(emit(program, OperationKind::Binary, OpCode::BitAnd, type, {bits, data[i]}, debug));
        }
        auto result = balanced(program, std::move(terms), OpCode::BitOr, type, debug);
        assign(program, root, result, debug);
        consumed.insert(chain.begin(), chain.end());
        changed = true;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

inline bool balanceAssociativeTrees(MutableProgram& graph, unsigned max_leaves = 32) {
    using namespace structure_detail;
    if (max_leaves < 3) return false;
    max_leaves = std::min(max_leaves, 64u);
    auto& program = graph.program();
    const auto live = reachable(graph);
    const auto use_count = users(graph);
    const auto order = width_detail::topologicalOrder(program);
    std::vector<int> arrival(program.signals.size());
    for (NodeId id : order) {
        const auto& signal = program.signal(id);
        if (!signal.driver) continue;
        int delay = signal.driver->kind == OperationKind::Assign ? 0 : 1;
        if (signal.driver->op == OpCode::Mul) delay = 3;
        for (const auto& operand : signal.driver->operands)
            if (operand.kind == OperandKind::Symbol)
                arrival[id] = std::max(arrival[id], arrival[operand.node]);
        arrival[id] += delay;
    }
    std::unordered_set<NodeId> consumed;
    bool changed = false;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        NodeId root = *it;
        if (!live.count(root) || consumed.count(root) || !program.signal(root).driver) continue;
        const Operation original = *program.signal(root).driver;
        const auto type = original.type;
        auto compatible = [&](const Operation& op) {
            if (op.kind != OperationKind::Binary || op.op != original.op || op.operands.size() != 2 ||
                !predicate_detail::sameValueType(op.type, type)) return false;
            for (const auto& operand : op.operands)
                if (!predicate_detail::sameValueType(operand.type, type) ||
                    operand.signed_view || operand.constant.signed_view) return false;
            return true;
        };
        if (type.isArray() || !predicate_detail::sameValueType(program.signal(root).type, type) ||
            (original.op != OpCode::BitAnd && original.op != OpCode::BitOr &&
             original.op != OpCode::BitXor && original.op != OpCode::Add) || !compatible(original)) continue;
        std::vector<Operand> leaves;
        std::vector<NodeId> expanded;
        bool overflow = false;
        std::function<void(const Operand&, unsigned)> collect = [&](const Operand& value, unsigned depth) {
            if (overflow) return;
            if (depth >= max_leaves || leaves.size() >= max_leaves) { overflow = true; return; }
            const auto* driver = predicate_detail::symbolDriver(value, program);
            if (driver && value.node < use_count.size() && use_count[value.node] == 1 &&
                !consumed.count(value.node) &&
                predicate_detail::sameValueType(program.signal(value.node).type, type) && compatible(*driver)) {
                expanded.push_back(value.node);
                collect(driver->operands[0], depth + 1); collect(driver->operands[1], depth + 1);
            } else leaves.push_back(value);
        };
        collect(original.operands[0], 0); collect(original.operands[1], 0);
        if (overflow || leaves.size() < 3) continue;
        // Huffman-style scheduling: early inputs combine first; late ones stay near the root.
        using Item = std::pair<int, std::size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            int time = leaves[i].kind == OperandKind::Symbol ? arrival.at(leaves[i].node) : 0;
            queue.push({time, i});
        }
        auto preview = queue;
        std::size_t next_id = leaves.size();
        while (preview.size() > 1) {
            auto a = preview.top(); preview.pop(); auto b = preview.top(); preview.pop();
            preview.push({std::max(a.first, b.first) + 1, next_id++});
        }
        if (preview.top().first >= arrival[root]) continue;
        auto debug = original.debug;
        addDebugMessage(debug, "balanced associative tree using input arrival estimates");
        while (queue.size() > 1) {
            auto a = queue.top(); queue.pop(); auto b = queue.top(); queue.pop();
            auto result = emit(program, OperationKind::Binary, original.op, type,
                               {leaves[a.second], leaves[b.second]}, debug);
            queue.push({std::max(a.first, b.first) + 1, leaves.size()});
            leaves.push_back(std::move(result));
        }
        assign(program, root, leaves[queue.top().second], debug);
        consumed.insert(expanded.begin(), expanded.end());
        changed = true;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}
} // namespace pred::beir::opt
