#pragma once

#include "backend/beir.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pred::beir::opt {
namespace predicate_detail {

struct Predicate {
    Operand guard;
    bool when_true = true;
};

struct Context {
    std::vector<Predicate> predicates;
};

inline Context unconditionalContext() {
    return Context{};
}

inline Context guardedContext(Operand guard, bool when_true) {
    Context context;
    context.predicates.push_back(Predicate{std::move(guard), when_true});
    return context;
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

inline bool samePredicate(const Predicate& lhs, const Predicate& rhs) {
    return lhs.when_true == rhs.when_true && sameOperand(lhs.guard, rhs.guard);
}

inline bool operandLess(const Operand& lhs, const Operand& rhs) {
    if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
    if (lhs.node != rhs.node) return lhs.node < rhs.node;
    if (lhs.text != rhs.text) return lhs.text < rhs.text;
    if (lhs.signed_view != rhs.signed_view) return lhs.signed_view < rhs.signed_view;
    if (lhs.type.width != rhs.type.width) return lhs.type.width < rhs.type.width;
    if (lhs.type.array_dims != rhs.type.array_dims) return lhs.type.array_dims < rhs.type.array_dims;
    if (lhs.kind != OperandKind::Literal) return false;
    if (lhs.constant.width != rhs.constant.width) return lhs.constant.width < rhs.constant.width;
    if (lhs.constant.signed_view != rhs.constant.signed_view) {
        return lhs.constant.signed_view < rhs.constant.signed_view;
    }
    return lhs.constant.limbs < rhs.constant.limbs;
}

inline bool predicateLess(const Predicate& lhs, const Predicate& rhs) {
    if (!sameOperand(lhs.guard, rhs.guard)) return operandLess(lhs.guard, rhs.guard);
    return lhs.when_true < rhs.when_true;
}

inline void normalizeContext(Context& context) {
    std::sort(context.predicates.begin(), context.predicates.end(), predicateLess);
    context.predicates.erase(
        std::unique(context.predicates.begin(), context.predicates.end(), samePredicate),
        context.predicates.end());
}

inline bool sameContext(const Context& lhs, const Context& rhs) {
    if (lhs.predicates.size() != rhs.predicates.size()) return false;
    for (std::size_t i = 0; i < lhs.predicates.size(); ++i) {
        if (!samePredicate(lhs.predicates[i], rhs.predicates[i])) return false;
    }
    return true;
}

inline bool isUnconditional(const Context& context) {
    return context.predicates.empty();
}

inline bool hasUnconditional(const std::vector<Context>& contexts) {
    for (const auto& context : contexts) {
        if (isUnconditional(context)) return true;
    }
    return false;
}

inline bool appendContext(std::vector<Context>& contexts, Context context) {
    normalizeContext(context);
    if (isUnconditional(context)) {
        if (contexts.size() == 1 && isUnconditional(contexts.front())) return false;
        contexts.clear();
        contexts.push_back(std::move(context));
        return true;
    }
    if (hasUnconditional(contexts)) return false;
    for (const auto& existing : contexts) {
        if (sameContext(existing, context)) return false;
    }
    contexts.push_back(std::move(context));
    return true;
}

inline bool appendPredicate(std::vector<Predicate>& predicates, Predicate predicate) {
    for (const auto& existing : predicates) {
        if (samePredicate(existing, predicate)) return false;
    }
    predicates.push_back(std::move(predicate));
    return true;
}

inline Context branchContext(const Context& parent, const Operand& guard, bool when_true) {
    (void)parent;
    return guardedContext(guard, when_true);
}

inline const Operation* symbolDriver(const Operand& operand, const Program& program) {
    if (operand.kind != OperandKind::Symbol) return nullptr;
    const Signal* signal = program.findSignal(operand.node);
    if (!signal || !signal->driver) return nullptr;
    return &*signal->driver;
}

inline bool conditionTruthImplies(const Operand& condition,
                                  bool condition_value,
                                  const Operand& target,
                                  bool target_value,
                                  const Program& program,
                                  int depth = 0) {
    if (depth > 8) return false;
    if (sameOperand(condition, target)) return condition_value == target_value;

    const Operation* driver = symbolDriver(condition, program);
    if (!driver) return false;

    if (driver->kind == OperationKind::Unary && driver->op == OpCode::LogicNot &&
        driver->operands.size() == 1) {
        return conditionTruthImplies(driver->operands[0], !condition_value,
                                     target, target_value, program, depth + 1);
    }

    if (driver->kind == OperationKind::Binary && driver->operands.size() == 2) {
        if (driver->op == OpCode::LogicAnd && condition_value) {
            return conditionTruthImplies(driver->operands[0], true, target, target_value,
                                         program, depth + 1) ||
                   conditionTruthImplies(driver->operands[1], true, target, target_value,
                                         program, depth + 1);
        }
        if (driver->op == OpCode::LogicOr && !condition_value) {
            return conditionTruthImplies(driver->operands[0], false, target, target_value,
                                         program, depth + 1) ||
                   conditionTruthImplies(driver->operands[1], false, target, target_value,
                                         program, depth + 1);
        }
    }

    return false;
}

inline bool isKnownZero(const Operand& operand, const Program& program) {
    if (operand.kind == OperandKind::Literal) return operand.constant.isZero();
    if (operand.kind != OperandKind::Symbol) return false;
    const Signal* signal = program.findSignal(operand.node);
    return signal && signal->value.valid && signal->value.constant && signal->value.value.isZero();
}

inline bool contextImpliesBranch(const Context& context,
                                 const Operand& guard,
                                 bool when_true,
                                 const Program& program) {
    if (context.predicates.empty()) return false;
    for (const auto& predicate : context.predicates) {
        if (conditionTruthImplies(predicate.guard, predicate.when_true,
                                  guard, when_true, program)) {
            return true;
        }
    }
    return false;
}

inline bool onlyNeededInBranch(const std::vector<Context>& contexts,
                               const Operand& guard,
                               bool when_true,
                               const Program& program) {
    if (contexts.empty()) return false;
    for (const auto& context : contexts) {
        if (!contextImpliesBranch(context, guard, when_true, program)) return false;
    }
    return true;
}

inline void setAssign(Operation& op,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> source_locs = op.source_locs;
    source_locs.insert(source_locs.end(), op.debug.source_locs.begin(), op.debug.source_locs.end());
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
    op.debug.origin = DebugOrigin::Generated;
    op.debug.reason = reason;
    op.debug.source_locs = std::move(source_locs);
    op.debug.derived_nodes.clear();
    op.debug.derived_names.clear();
    for (const auto& operand_ref : op.operands) {
        if (operand_ref.kind == OperandKind::Symbol && operand_ref.node != kInvalidNodeId) {
            op.debug.derived_nodes.push_back(operand_ref.node);
        } else if (!operand_ref.text.empty()) {
            op.debug.derived_names.push_back(operand_ref.text);
        }
    }
    addOperandDebugLocs(op.debug, program, op.operands);
    op.source_locs = op.debug.source_locs;
}

inline void pushSymbolContext(const Operand& operand,
                              const Context& context,
                              std::vector<std::vector<Context>>& contexts,
                              std::vector<NodeId>& worklist) {
    if (operand.kind != OperandKind::Symbol) return;
    if (operand.node == kInvalidNodeId || operand.node >= contexts.size()) return;
    if (appendContext(contexts[operand.node], context)) worklist.push_back(operand.node);
}

inline void propagateOperandContexts(const Operation& op,
                                     const Context& context,
                                     std::vector<std::vector<Context>>& contexts,
                                     std::vector<NodeId>& worklist) {
    if (op.kind == OperationKind::Ite && op.operands.size() == 3) {
        pushSymbolContext(op.operands[0], context, contexts, worklist);
        pushSymbolContext(op.operands[1], branchContext(context, op.operands[0], true),
                          contexts, worklist);
        pushSymbolContext(op.operands[2], branchContext(context, op.operands[0], false),
                          contexts, worklist);
        return;
    }
    for (const auto& operand : op.operands) {
        pushSymbolContext(operand, context, contexts, worklist);
    }
}

inline std::vector<std::vector<Context>> analyzeDemandContexts(const MutableProgram& graph) {
    const Program& program = graph.program();
    std::vector<std::vector<Context>> contexts(program.signals.size());
    std::vector<NodeId> worklist;
    for (const auto& signal : program.signals) {
        if (graph.isObservable(signal) &&
            appendContext(contexts[signal.id], unconditionalContext())) {
            worklist.push_back(signal.id);
        }
    }

    std::size_t cursor = 0;
    while (cursor < worklist.size()) {
        NodeId id = worklist[cursor++];
        const Signal* signal = program.findSignal(id);
        if (!signal || !signal->driver) continue;
        for (const auto& context : contexts[id]) {
            propagateOperandContexts(*signal->driver, context, contexts, worklist);
        }
    }
    return contexts;
}

inline bool rewriteGuardedDefault(Operation& op,
                                  const std::vector<Context>& contexts,
                                  const Program& program) {
    if (op.kind != OperationKind::Ite || op.operands.size() != 3) return false;
    const Operand& guard = op.operands[0];
    const ValueType type = op.type;
    if (onlyNeededInBranch(contexts, guard, true, program)) {
        setAssign(op, op.operands[1], type,
                  "sank predicate guard and omitted unreachable false branch", program);
        return true;
    }
    if (onlyNeededInBranch(contexts, guard, false, program)) {
        setAssign(op, op.operands[2], type,
                  "sank predicate guard and omitted unreachable true branch", program);
        return true;
    }
    return false;
}

} // namespace predicate_detail

inline bool sinkPredicates(MutableProgram& graph) {
    graph.ensureValueFacts();
    const auto contexts = predicate_detail::analyzeDemandContexts(graph);
    bool changed = false;
    Program& program = graph.program();
    for (auto& signal : program.signals) {
        if (graph.isObservable(signal) || !signal.driver) continue;
        bool signal_changed = predicate_detail::rewriteGuardedDefault(
            *signal.driver, contexts[signal.id], program);
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
