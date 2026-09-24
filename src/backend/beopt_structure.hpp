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

// Paths to distinct leaves disagree at their first divergent tree edge.  This
// proves mutual exclusion structurally, even for overlapping original tests.
inline bool parallelizeExclusiveMuxes(MutableProgram& graph, unsigned max_branches = 1024) {
    using namespace structure_detail;
    if (max_branches < 3) return false;
    max_branches = std::min(max_branches, 4096u);
    auto& program = graph.program();
    const auto live = reachable(graph);
    const auto use_count = users(graph);
    const auto order = width_detail::topologicalOrder(program);
    std::unordered_set<NodeId> consumed;
    bool changed = false;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const NodeId root = *it;
        if (!live.count(root) || consumed.count(root)) continue;
        const auto original = program.signal(root).driver;
        const auto type = program.signal(root).type;
        if (!original || original->kind != OperationKind::Ite || type.isArray()) continue;
        struct Edge { Operand condition; bool positive; };
        struct Leaf { Operand value; std::vector<Edge> path; };
        std::vector<Leaf> leaves;
        std::vector<Edge> path;
        std::vector<NodeId> expanded;
        bool overflow = false;
        std::function<void(Operand, bool)> collect = [&](Operand value, bool is_root) {
            if (overflow) return;
            const auto* op = predicate_detail::symbolDriver(value, program);
            bool descend = op && op->kind == OperationKind::Ite &&
                op->operands.size() == 3 && !value.signed_view &&
                !value.constant.signed_view &&
                predicate_detail::sameValueType(op->type, type) &&
                op->operands[0].type.width == 1 && !op->operands[0].type.isArray() &&
                predicate_detail::sameValueType(op->operands[1].type, type) &&
                predicate_detail::sameValueType(op->operands[2].type, type) &&
                (is_root || (value.node < use_count.size() && use_count[value.node] == 1 &&
                             !consumed.count(value.node)));
            if (!descend) {
                if (leaves.size() >= max_branches + 1u) { overflow = true; return; }
                leaves.push_back({value, path});
                return;
            }
            if (expanded.size() >= max_branches) { overflow = true; return; }
            expanded.push_back(value.node);
            path.push_back({op->operands[0], true});
            collect(op->operands[1], false);
            path.back().positive = false;
            collect(op->operands[2], false);
            path.pop_back();
        };
        Operand root_value;
        root_value.kind = OperandKind::Symbol; root_value.node = root; root_value.type = type;
        collect(root_value, true);
        // A single mux already has the desired shape. Keep shared subgraphs as
        // leaves, so flattening never duplicates their data computations.
        if (overflow || expanded.size() < 2) continue;
        auto debug = original->debug;
        addDebugMessage(debug, "flattened mux tree using structurally exclusive full paths");
        Operation result;
        result.kind = OperationKind::Case; result.type = type; result.debug = debug;
        for (std::size_t i = 0; i + 1 < leaves.size(); ++i) {
            std::vector<Operand> terms;
            for (const auto& edge : leaves[i].path) {
                auto term = edge.condition;
                if (!edge.positive)
                    term = emit(program, OperationKind::Unary, OpCode::LogicNot,
                                {1, {}}, {term}, debug);
                terms.push_back(term);
            }
            result.operands.push_back(balanced(program, std::move(terms), OpCode::BitAnd,
                                               {1, {}}, debug));
            result.operands.push_back(leaves[i].value);
        }
        result.operands.push_back(leaves.back().value);
        program.signal(root).driver = std::move(result);
        consumed.insert(expanded.begin(), expanded.end());
        changed = true;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

inline bool balanceAssociativeTrees(MutableProgram& graph, unsigned max_leaves = 1024) {
    using namespace structure_detail;
    if (max_leaves < 3) return false;
    max_leaves = std::min(max_leaves, 4096u);
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
