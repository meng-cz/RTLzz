#pragma once

#include "backend/beir.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pred::beir::opt {

namespace assign_chain_detail {

inline DebugInfo generatedDebug(std::string reason, const std::vector<Operand>& operands) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Generated;
    debug.reason = std::move(reason);
    for (const auto& operand : operands) {
        if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
            debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            debug.derived_names.push_back(operand.text);
        }
    }
    return debug;
}

inline bool sameValueType(const ValueType& lhs, const ValueType& rhs) {
    return lhs.width == rhs.width && lhs.array_dims == rhs.array_dims;
}

inline bool sameConstant(const Operand::Constant& lhs, const Operand::Constant& rhs) {
    return lhs.width == rhs.width &&
           lhs.signed_view == rhs.signed_view &&
           lhs.limbs == rhs.limbs;
}

inline bool sameOperand(const Operand& lhs, const Operand& rhs) {
    if (lhs.kind != rhs.kind ||
        lhs.node != rhs.node ||
        lhs.text != rhs.text ||
        lhs.signed_view != rhs.signed_view ||
        !sameValueType(lhs.type, rhs.type)) {
        return false;
    }
    if (lhs.kind == OperandKind::Literal && !sameConstant(lhs.constant, rhs.constant)) return false;
    return true;
}

inline Operation* symbolDriver(Program& program, const Operand& operand) {
    if (operand.kind != OperandKind::Symbol) return nullptr;
    Signal* signal = program.findSignal(operand.node);
    if (!signal || !signal->driver) return nullptr;
    return &*signal->driver;
}

inline bool foldSameGuardMux(Operation& op, MutableProgram& graph) {
    if (op.kind != OperationKind::Ite || op.operands.size() != 3) return false;

    Program& program = graph.program();
    Operation* then_driver = symbolDriver(program, op.operands[1]);
    if (then_driver && then_driver->kind == OperationKind::Ite &&
        then_driver->operands.size() == 3 &&
        sameOperand(op.operands[0], then_driver->operands[0])) {
        if (sameOperand(op.operands[1], then_driver->operands[1])) return false;
        op.operands[1] = then_driver->operands[1];
        op.debug = generatedDebug("folded same-guard mux chain through true branch", op.operands);
        return true;
    }

    Operation* else_driver = symbolDriver(program, op.operands[2]);
    if (else_driver && else_driver->kind == OperationKind::Ite &&
        else_driver->operands.size() == 3 &&
        sameOperand(op.operands[0], else_driver->operands[0])) {
        if (sameOperand(op.operands[2], else_driver->operands[2])) return false;
        op.operands[2] = else_driver->operands[2];
        op.debug = generatedDebug("folded same-guard mux chain through false branch", op.operands);
        return true;
    }

    return false;
}

} // namespace assign_chain_detail

inline bool foldAssignChains(MutableProgram& graph) {
    std::unordered_map<NodeId, Operand> aliases;
    for (const auto& signal : graph.program().signals) {
        if (graph.isObservable(signal) || !signal.driver ||
            signal.driver->kind != OperationKind::Assign ||
            signal.driver->operands.size() != 1) {
            continue;
        }
        Operand replacement = signal.driver->operands.front();
        if (replacement.kind == OperandKind::Symbol && replacement.node == signal.id) continue;
        aliases[signal.id] = std::move(replacement);
    }

    bool changed = false;
    if (!aliases.empty()) changed = graph.replaceAliases(aliases) || changed;

    for (auto& signal : graph.program().signals) {
        if (!signal.driver) continue;
        bool signal_changed = assign_chain_detail::foldSameGuardMux(*signal.driver, graph);
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
