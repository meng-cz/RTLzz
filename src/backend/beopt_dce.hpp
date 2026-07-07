#pragma once

#include "backend/beir.hpp"

#include <unordered_set>
#include <vector>

namespace pred::beir::opt {

inline bool eliminateDeadNodes(MutableProgram& graph) {
    const Program& program = graph.program();
    std::unordered_set<NodeId> live;
    std::vector<NodeId> stack;
    auto push = [&](NodeId id) {
        if (id == kInvalidNodeId || id >= program.signals.size()) return;
        if (live.insert(id).second) stack.push_back(id);
    };

    for (const auto& signal : program.signals) {
        if (graph.isObservable(signal)) push(signal.id);
    }
    for (const auto& aggregate : program.aggregates) {
        for (NodeId id : aggregate.element_nodes) push(id);
    }

    while (!stack.empty()) {
        NodeId id = stack.back();
        stack.pop_back();
        const Signal* signal = program.findSignal(id);
        if (!signal || !signal->driver) continue;
        for (const auto& operand : signal->driver->operands) {
            if (operand.kind == OperandKind::Symbol) push(operand.node);
        }
    }

    if (live.size() == program.signals.size()) return false;
    graph.compact(live);
    return true;
}

} // namespace pred::beir::opt
