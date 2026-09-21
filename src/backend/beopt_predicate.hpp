#pragma once

#include "backend/beir.hpp"

#include <algorithm>
#include <functional>
#include <optional>
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
    Context result = parent;
    appendPredicate(result.predicates, Predicate{guard, when_true});
    return result;
}

inline const Operation* symbolDriver(const Operand& operand, const Program& program) {
    if (operand.kind != OperandKind::Symbol) return nullptr;
    const Signal* signal = program.findSignal(operand.node);
    if (!signal || !signal->driver) return nullptr;
    return &*signal->driver;
}

// Queries prove relations in an exact Boolean abstraction. Unmodelled atoms
// are independent, while equalities of one selector to different constants are
// treated as mutually exclusive. No analysis state survives a query, so graph
// mutations cannot stale a cache.
enum class Proof { Unknown, Proven };

class PredicateRelations {
    const Program& program_;
    struct Formula { int kind; int a; int b; }; // constant, atom, not, and, or
    struct Atom { Operand value; std::optional<Operand> selector, literal; };
    std::vector<Formula> formulas_;
    std::vector<Atom> atoms_;

    int add(int kind, int a = 0, int b = 0) {
        formulas_.push_back({kind, a, b});
        return static_cast<int>(formulas_.size() - 1);
    }
    int build(const Operand& value) {
        if (value.kind == OperandKind::Literal) return add(0, !value.constant.isZero());
        const auto* op = symbolDriver(value, program_);
        if (op && op->kind == OperationKind::Assign && op->operands.size() == 1 &&
            sameValueType(op->type, op->operands[0].type))
            return build(op->operands[0]);
        if (op && op->kind == OperationKind::Unary && op->op == OpCode::LogicNot &&
            op->operands.size() == 1) return add(2, build(op->operands[0]));
        if (op && op->kind == OperationKind::Binary && op->operands.size() == 2 &&
            (op->op == OpCode::LogicAnd || op->op == OpCode::LogicOr ||
             (value.type.width == 1 && op->operands[0].type.width == 1 &&
              op->operands[1].type.width == 1 &&
              (op->op == OpCode::BitAnd || op->op == OpCode::BitOr)))) {
            int left = build(op->operands[0]);
            int right = build(op->operands[1]);
            return add(op->op == OpCode::LogicAnd || op->op == OpCode::BitAnd ? 3 : 4, left, right);
        }
        Atom atom{value, {}, {}};
        if (op && op->kind == OperationKind::Binary && op->op == OpCode::Eq &&
            op->operands.size() == 2) {
            auto left = op->operands[0], right = op->operands[1];
            if (left.kind == OperandKind::Literal) std::swap(left, right);
            // Equal-width unsigned comparisons avoid extension/truncation ambiguity.
            if (right.kind == OperandKind::Literal && !left.type.isArray() &&
                sameValueType(left.type, right.type) && !left.signed_view &&
                !right.signed_view && !right.constant.signed_view &&
                right.constant.width == right.type.width && right.type.width > 0) {
                right.constant.limbs.resize(static_cast<std::size_t>((right.type.width + 63) / 64), 0);
                if (right.type.width % 64)
                    right.constant.limbs.back() &= (std::uint64_t{1} << (right.type.width % 64)) - 1;
                atom.selector = left;
                atom.literal = right;
            }
        }
        for (std::size_t i = 0; i < atoms_.size(); ++i) {
            if (sameOperand(value, atoms_[i].value) ||
                (atom.selector && atoms_[i].selector &&
                 sameOperand(*atom.selector, *atoms_[i].selector) &&
                 sameConstant(atom.literal->constant, atoms_[i].literal->constant)))
                return add(1, static_cast<int>(i));
        }
        atoms_.push_back(std::move(atom));
        return add(1, static_cast<int>(atoms_.size() - 1));
    }
    bool evaluate(int id, const std::vector<bool>& values) const {
        const auto& f = formulas_[id];
        switch (f.kind) {
        case 0: return f.a;
        case 1: return values.at(static_cast<std::size_t>(f.a));
        case 2: return !evaluate(f.a, values);
        case 3: return evaluate(f.a, values) && evaluate(f.b, values);
        default: return evaluate(f.a, values) || evaluate(f.b, values);
        }
    }
    bool admissible(const std::vector<bool>& values) const {
        for (std::size_t i = 0; i < atoms_.size(); ++i) {
            if (!values[i] || !atoms_[i].selector) continue;
            for (std::size_t j = 0; j < i; ++j) {
                if (values[j] && atoms_[j].selector &&
                    sameOperand(*atoms_[i].selector, *atoms_[j].selector) &&
                    !sameConstant(atoms_[i].literal->constant, atoms_[j].literal->constant))
                    return false;
            }
        }
        return true;
    }
public:
    explicit PredicateRelations(const Program& program) : program_(program) {}
    Proof implies(const Context& context, Predicate target) {
        formulas_.clear(); atoms_.clear();
        std::vector<std::pair<int, bool>> facts;
        for (const auto& p : context.predicates) facts.push_back({build(p.guard), p.when_true});
        const int goal = build(target.guard);
        std::vector<bool> values(atoms_.size(), false);
        std::function<bool(std::size_t)> has_counterexample = [&](std::size_t index) {
            if (index != values.size()) {
                values[index] = false;
                if (has_counterexample(index + 1)) return true;
                values[index] = true;
                return has_counterexample(index + 1);
            }
            if (!admissible(values)) return false;
            for (const auto& fact : facts)
                if (evaluate(fact.first, values) != fact.second) return false;
            return evaluate(goal, values) != target.when_true;
        };
        return has_counterexample(0) ? Proof::Unknown : Proof::Proven;
    }
    Proof implies(const Operand& a, const Operand& b) {
        return implies(guardedContext(a, true), Predicate{b, true});
    }
    Proof isExclusive(const Operand& a, const Operand& b) {
        return implies(guardedContext(a, true), Predicate{b, false});
    }
};

inline bool conditionTruthImplies(const Operand& condition, bool condition_value,
                                  const Operand& target, bool target_value,
                                  const Program& program, int = 0) {
    return PredicateRelations(program).implies(guardedContext(condition, condition_value),
                                               Predicate{target, target_value}) == Proof::Proven;
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
    return PredicateRelations(program).implies(context, Predicate{guard, when_true}) == Proof::Proven;
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
    DebugInfo previous_debug = op.debug;
    op.kind = OperationKind::Assign;
    op.op = OpCode::None;
    op.operands.clear();
    op.signed_truncation = false;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.to_width = 0;
    op.hi = -1;
    op.lo = -1;
    op.bit = -1;
    op.times = 0;
    op.debug.origin = DebugOrigin::Generated;
    addDebugInfoLocs(op.debug, previous_debug);
    addDebugLocs(op.debug, source_locs);
    addDebugMessage(op.debug, reason);
    op.debug.reason = reason;
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
    if (op.kind == OperationKind::Case && hasValidCaseShape(op)) {
        Context remaining = context;
        for (std::size_t branch = 0; branch < caseBranchCount(op); ++branch) {
            const Operand& condition = op.operands[branch * 2];
            pushSymbolContext(condition, remaining, contexts, worklist);
            pushSymbolContext(op.operands[branch * 2 + 1],
                              branchContext(remaining, condition, true),
                              contexts, worklist);
            remaining = branchContext(remaining, condition, false);
        }
        pushSymbolContext(op.operands.back(), remaining, contexts, worklist);
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
