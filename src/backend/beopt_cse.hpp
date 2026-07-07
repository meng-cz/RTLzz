#pragma once

#include "backend/beir.hpp"

#include <unordered_map>
#include <utility>

namespace pred::beir::opt {

inline bool mergeCommonExpressions(MutableProgram& graph) {
    std::unordered_map<OperationSignature, Operand, OperationSignatureHash> available;
    std::unordered_map<NodeId, Operand> aliases;
    for (const auto& signal : graph.program().signals) {
        if (graph.isObservable(signal) || !signal.driver || !graph.isCseCandidate(*signal.driver)) continue;
        OperationSignature key = graph.operationSignature(*signal.driver);
        auto it = available.find(key);
        if (it == available.end()) {
            Operand self;
            self.kind = OperandKind::Symbol;
            self.node = signal.id;
            self.text = signal.name;
            self.type = signal.type;
            available.emplace(std::move(key), std::move(self));
        } else {
            aliases[signal.id] = it->second;
        }
    }
    if (aliases.empty()) return false;
    return graph.replaceAliases(aliases);
}

} // namespace pred::beir::opt
