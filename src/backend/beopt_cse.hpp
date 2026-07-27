#pragma once

#include "backend/beir.hpp"

#include <unordered_map>
#include <utility>

namespace pred::beir::opt {
namespace cse_detail {

inline Operand symbolOperand(const Signal& signal) {
    Operand operand;
    operand.kind = OperandKind::Symbol;
    operand.node = signal.id;
    operand.text = signal.name;
    operand.type = signal.type;
    return operand;
}

inline void mergeDuplicateDebug(DebugInfo& debug,
                                const Program& program,
                                const Signal& duplicate) {
    addDebugInfoAsRelated(debug, duplicate.debug);
    if (duplicate.driver) {
        addDebugInfoAsRelated(debug, duplicate.driver->debug);
        for (const auto& loc : duplicate.driver->source_locs) addRelatedDebugLoc(debug, loc);
    }
    addDebugMessage(debug, "merged common subexpression from '" + duplicate.name + "'");
    addDebugDerivedOperand(debug, symbolOperand(duplicate));

    Operand name_operand;
    name_operand.kind = OperandKind::Port;
    name_operand.text = duplicate.name;
    name_operand.type = duplicate.type;
    addDebugDerivedOperand(debug, name_operand);
    addOperandDebugLocs(debug, program, symbolOperand(duplicate));
}

inline void mergeDuplicateIntoRepresentative(Program& program,
                                             NodeId representative_id,
                                             const Signal& duplicate) {
    Signal& representative = program.signal(representative_id);
    mergeDuplicateDebug(representative.debug, program, duplicate);
    if (representative.driver) {
        mergeDuplicateDebug(representative.driver->debug, program, duplicate);
        representative.driver->source_locs = representative.driver->debug.source_locs;
    }
}

} // namespace cse_detail

inline bool mergeCommonExpressions(MutableProgram& graph) {
    Program& program = graph.program();
    std::unordered_map<OperationSignature, NodeId, OperationSignatureHash> available;
    std::unordered_map<NodeId, Operand> aliases;
    for (const auto& signal : program.signals) {
        if (graph.isObservable(signal) || !signal.driver || !graph.isCseCandidate(*signal.driver)) continue;
        OperationSignature key = graph.operationSignature(*signal.driver);
        auto it = available.find(key);
        if (it == available.end()) {
            available.emplace(std::move(key), signal.id);
        } else {
            cse_detail::mergeDuplicateIntoRepresentative(program, it->second, signal);
            aliases[signal.id] = cse_detail::symbolOperand(program.signal(it->second));
        }
    }
    if (aliases.empty()) return false;
    return graph.replaceAliases(aliases);
}

} // namespace pred::beir::opt
