#pragma once

#include "backend/beir.hpp"

#include <unordered_map>
#include <utility>

namespace pred::beir::opt {

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
    if (aliases.empty()) return false;
    return graph.replaceAliases(aliases);
}

} // namespace pred::beir::opt
