#pragma once

#include "backend/beir.hpp"

#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pred::beir::opt {
namespace constant_detail {

inline int widthOf(const ValueType& type) {
    return type.width > 0 ? type.width : 1;
}

inline std::size_t limbCount(int width) {
    return width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
}

inline std::uint64_t highMask(int width) {
    int rem = width % 64;
    if (rem == 0) return std::numeric_limits<std::uint64_t>::max();
    return (std::uint64_t{1} << rem) - 1;
}

inline void trim(std::vector<std::uint64_t>& limbs, int width) {
    limbs.resize(limbCount(width), 0);
    if (!limbs.empty()) limbs.back() &= highMask(width);
}

inline bool sameConstant(std::vector<std::uint64_t> lhs,
                         std::vector<std::uint64_t> rhs,
                         int width) {
    trim(lhs, width);
    trim(rhs, width);
    return lhs == rhs;
}

inline Operand literalOperandFromFacts(const ValueFacts& facts,
                                       const ValueType& type,
                                       bool signed_view = false) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type = type;
    operand.signed_view = signed_view || facts.value.signed_view;
    operand.constant = facts.value;
    operand.constant.width = widthOf(type);
    operand.constant.signed_view = operand.signed_view;
    trim(operand.constant.limbs, operand.constant.width);
    operand.text = "const";
    return operand;
}

inline std::vector<NodeId> topologicalOrder(const Program& program) {
    std::vector<std::vector<NodeId>> users(program.signals.size());
    std::vector<std::size_t> indegree(program.signals.size(), 0);
    for (const auto& signal : program.signals) {
        if (signal.id >= program.signals.size()) {
            throw std::runtime_error("BEIR constant folding requires dense NodeId indices");
        }
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind != OperandKind::Symbol) continue;
            if (operand.node >= program.signals.size() || !program.findSignal(operand.node)) {
                throw std::runtime_error("BEIR constant folding found dependency on unknown node");
            }
            users[operand.node].push_back(signal.id);
            ++indegree[signal.id];
        }
    }

    std::queue<NodeId> ready;
    for (const auto& signal : program.signals) {
        if (indegree[signal.id] == 0) ready.push(signal.id);
    }

    std::vector<NodeId> order;
    while (!ready.empty()) {
        NodeId id = ready.front();
        ready.pop();
        order.push_back(id);
        for (NodeId user : users[id]) {
            if (--indegree[user] == 0) ready.push(user);
        }
    }
    if (order.size() != program.signals.size()) {
        throw std::runtime_error("BEIR constant folding requires an acyclic signal dependency graph");
    }
    return order;
}

inline void refreshGeneratedDebug(Operation& op,
                                  const std::string& reason,
                                  const Program& program) {
    std::vector<DebugLoc> source_locs = op.source_locs;
    source_locs.insert(source_locs.end(), op.debug.source_locs.begin(), op.debug.source_locs.end());
    op.debug.origin = DebugOrigin::Generated;
    op.debug.reason = reason;
    op.debug.source_locs = std::move(source_locs);
    op.debug.derived_nodes.clear();
    op.debug.derived_names.clear();
    for (const auto& operand : op.operands) {
        if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
            op.debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            op.debug.derived_names.push_back(operand.text);
        }
    }
    addOperandDebugLocs(op.debug, program, op.operands);
    op.source_locs = op.debug.source_locs;
}

inline void setAssign(Operation& op,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    op.kind = OperationKind::Assign;
    op.op = OpCode::None;
    op.operands.clear();
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.to_width = 0;
    op.hi = -1;
    op.lo = -1;
    op.bit = -1;
    op.times = 0;
    refreshGeneratedDebug(op, reason, program);
}

inline bool canFoldResult(OperationKind kind) {
    return kind != OperationKind::PortRead &&
           kind != OperationKind::Lookup &&
           kind != OperationKind::ArrayAccess &&
           kind != OperationKind::Aggregate &&
           kind != OperationKind::Call;
}

inline bool propagateConstantOperands(Operation& op, const Program& program) {
    bool changed = false;
    for (Operand& operand : op.operands) {
        if (operand.kind != OperandKind::Symbol) continue;
        const Signal* source = program.findSignal(operand.node);
        if (!source || !source->value.valid || !source->value.constant) continue;
        operand = literalOperandFromFacts(source->value, operand.type, operand.signed_view);
        changed = true;
    }
    if (changed) refreshGeneratedDebug(op, "propagated constant operands", program);
    return changed;
}

inline bool foldConstantResult(Operation& op, const Program& program, const ValueFacts& facts) {
    if (!facts.valid || !facts.constant || !canFoldResult(op.kind)) return false;

    const int width = widthOf(op.type);
    if (op.kind == OperationKind::Assign && op.operands.size() == 1 &&
        op.operands[0].kind == OperandKind::Literal &&
        op.operands[0].constant.signed_view == facts.value.signed_view &&
        sameConstant(op.operands[0].constant.limbs, facts.value.limbs, width)) {
        return false;
    }

    setAssign(op,
              literalOperandFromFacts(facts, op.type),
              op.type,
              "constant folded by BEIR constant propagation",
              program);
    return true;
}

} // namespace constant_detail

inline bool foldConstants(MutableProgram& graph) {
    using namespace constant_detail;

    graph.ensureValueFacts();
    bool changed = false;
    const std::vector<NodeId> order = topologicalOrder(graph.program());

    for (NodeId id : order) {
        Signal& signal = graph.program().signal(id);
        if (!signal.driver) continue;

        bool signal_changed = propagateConstantOperands(*signal.driver, graph.program());
        signal_changed = foldConstantResult(*signal.driver, graph.program(), signal.value) || signal_changed;
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }

    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
