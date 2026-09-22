#pragma once

#include "backend/beir.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <optional>
#include <string>
#include <unordered_set>
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
    std::size_t fingerprint = 0;
};

inline std::size_t predicateHash(const Predicate& predicate) {
    std::uint64_t seed = predicate.when_true ? 1 : 0;
    hashCombine(seed, predicate.guard.node);
    hashCombine(seed, static_cast<std::uint64_t>(predicate.guard.kind));
    hashCombine(seed, std::hash<std::string>{}(predicate.guard.text));
    hashCombine(seed, static_cast<std::uint64_t>(predicate.guard.type.width));
    hashCombine(seed, predicate.guard.signed_view ? 1 : 0);
    if (predicate.guard.kind == OperandKind::Literal) {
        hashCombine(seed, static_cast<std::uint64_t>(predicate.guard.constant.width));
        hashCombine(seed, predicate.guard.constant.signed_view ? 1 : 0);
        for (auto limb : predicate.guard.constant.limbs) hashCombine(seed, limb);
    }
    return static_cast<std::size_t>(seed);
}

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
    std::uint64_t fingerprint = 0;
    for (const auto& predicate : context.predicates)
        hashCombine(fingerprint, predicateHash(predicate));
    context.fingerprint = static_cast<std::size_t>(fingerprint);
}

inline bool sameContext(const Context& lhs, const Context& rhs) {
    if (lhs.fingerprint != rhs.fingerprint) return false;
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
// treated as mutually exclusive. Standalone queries reset their cache; the
// predicate-sinking pass reuses it only while the BEIR graph is unchanged.
enum class Proof { Unknown, Proven };
enum class ProofOutcome { Proven, Counterexample, ResourceLimit };

struct PredicateLimits {
    // These apply to each query's reachable cone, not the snapshot cache.
    // Predicate sinking is deliberately conservative by default. Rules 1-6
    // handle cheap proofs; SAT is only attempted for small residual queries.
    std::size_t max_formulas = 64;
    std::size_t max_atoms = 8;
    std::size_t max_candidate_ites = 128;
    std::size_t max_contexts_per_candidate = 64;
};

inline Proof asProof(ProofOutcome outcome) {
    return outcome == ProofOutcome::Proven ? Proof::Proven : Proof::Unknown;
}

class PredicateRelations {
    const Program& program_;
    struct Formula { int kind; int a; int b; }; // constant, atom, not, and, or
    struct Atom { Operand value; std::optional<Operand> selector, literal; };
    std::vector<Formula> formulas_;
    std::vector<Atom> atoms_;
    struct OperandHash {
        std::size_t operator()(const Operand& value) const {
            std::uint64_t seed = static_cast<std::uint64_t>(value.kind);
            hashCombine(seed, value.node);
            hashCombine(seed, std::hash<std::string>{}(value.text));
            hashCombine(seed, static_cast<std::uint64_t>(value.type.width));
            hashCombine(seed, value.signed_view ? 1 : 0);
            if (value.kind == OperandKind::Literal) {
                hashCombine(seed, static_cast<std::uint64_t>(value.constant.width));
                hashCombine(seed, value.constant.signed_view ? 1 : 0);
                for (auto limb : value.constant.limbs) hashCombine(seed, limb);
            }
            return static_cast<std::size_t>(seed);
        }
    };
    struct OperandEqual {
        bool operator()(const Operand& lhs, const Operand& rhs) const {
            return sameOperand(lhs, rhs);
        }
    };
    struct SelectorKey {
        Operand selector;
        Operand::Constant literal;
    };
    struct SelectorHash {
        std::size_t operator()(const SelectorKey& key) const {
            OperandHash operand_hash;
            std::uint64_t seed = operand_hash(key.selector);
            hashCombine(seed, static_cast<std::uint64_t>(key.literal.width));
            hashCombine(seed, key.literal.signed_view ? 1 : 0);
            for (auto limb : key.literal.limbs) hashCombine(seed, limb);
            return static_cast<std::size_t>(seed);
        }
    };
    struct SelectorEqual {
        bool operator()(const SelectorKey& lhs, const SelectorKey& rhs) const {
            return sameOperand(lhs.selector, rhs.selector) &&
                   sameConstant(lhs.literal, rhs.literal);
        }
    };
    std::unordered_map<Operand, int, OperandHash, OperandEqual> formula_cache_;
    std::unordered_map<Operand, std::size_t, OperandHash, OperandEqual> atom_cache_;
    std::unordered_map<SelectorKey, std::size_t, SelectorHash, SelectorEqual> selector_cache_;
    bool snapshot_ = false;
    PredicateLimits limits_;

    int add(int kind, int a = 0, int b = 0) {
        formulas_.push_back({kind, a, b});
        return static_cast<int>(formulas_.size() - 1);
    }
    int build(const Operand& value) {
        if (auto found = formula_cache_.find(value); found != formula_cache_.end())
            return found->second;
        int formula = -1;
        if (value.kind == OperandKind::Literal) {
            formula = add(0, !value.constant.isZero());
            formula_cache_.emplace(value, formula);
            return formula;
        }
        const auto* op = symbolDriver(value, program_);
        if (op && op->kind == OperationKind::Assign && op->operands.size() == 1 &&
            sameValueType(op->type, op->operands[0].type)) {
            formula = build(op->operands[0]);
            formula_cache_.emplace(value, formula);
            return formula;
        }
        if (op && op->kind == OperationKind::Unary && op->op == OpCode::LogicNot &&
            op->operands.size() == 1) {
            formula = add(2, build(op->operands[0]));
            formula_cache_.emplace(value, formula);
            return formula;
        }
        if (op && op->kind == OperationKind::Binary && op->operands.size() == 2 &&
            (op->op == OpCode::LogicAnd || op->op == OpCode::LogicOr ||
             (value.type.width == 1 && op->operands[0].type.width == 1 &&
              op->operands[1].type.width == 1 &&
              (op->op == OpCode::BitAnd || op->op == OpCode::BitOr)))) {
            int left = build(op->operands[0]);
            int right = build(op->operands[1]);
            formula = add(op->op == OpCode::LogicAnd || op->op == OpCode::BitAnd ? 3 : 4, left, right);
            formula_cache_.emplace(value, formula);
            return formula;
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
        auto found = atom_cache_.find(value);
        if (found != atom_cache_.end()) {
            formula = add(1, static_cast<int>(found->second));
            formula_cache_.emplace(value, formula);
            return formula;
        }
        if (atom.selector) {
            SelectorKey key{*atom.selector, atom.literal->constant};
            auto selector_found = selector_cache_.find(key);
            if (selector_found != selector_cache_.end()) {
                atom_cache_.emplace(value, selector_found->second);
                formula = add(1, static_cast<int>(selector_found->second));
                formula_cache_.emplace(value, formula);
                return formula;
            }
        }
        atoms_.push_back(std::move(atom));
        const std::size_t atom_id = atoms_.size() - 1;
        atom_cache_.emplace(value, atom_id);
        if (atoms_.back().selector) {
            selector_cache_.emplace(
                SelectorKey{*atoms_.back().selector, atoms_.back().literal->constant}, atom_id);
        }
        formula = add(1, static_cast<int>(atom_id));
        formula_cache_.emplace(value, formula);
        return formula;
    }
    class SatSolver {
        std::vector<std::vector<int>> clauses_;
        std::vector<std::vector<std::size_t>> occurrences_;
        std::vector<int> unit_literals_;
        std::vector<int8_t> assigned_;
        std::vector<int> trail_;
        std::size_t propagation_cursor_ = 0;
        std::unordered_map<int, int> formula_var_;
        int atom_base_ = 0;
        bool limit_exceeded_ = false;

        void clause(std::initializer_list<int> literals) {
            clauses_.emplace_back(literals);
            if (literals.size() == 1) unit_literals_.push_back(*literals.begin());
            const std::size_t clause_id = clauses_.size() - 1;
            for (int literal : literals)
                occurrences_[static_cast<std::size_t>(literal > 0 ? literal : -literal)]
                    .push_back(clause_id);
        }
        bool assign(int literal) {
            const auto variable = static_cast<std::size_t>(literal > 0 ? literal : -literal);
            const int8_t value = literal > 0 ? 1 : 0;
            if (assigned_[variable] != -1) return assigned_[variable] == value;
            assigned_[variable] = value;
            trail_.push_back(literal);
            return true;
        }
        void undo(std::size_t mark) {
            while (trail_.size() > mark) {
                const int literal = trail_.back();
                assigned_[static_cast<std::size_t>(literal > 0 ? literal : -literal)] = -1;
                trail_.pop_back();
            }
            propagation_cursor_ = mark;
        }
        bool propagate() {
            while (propagation_cursor_ < trail_.size()) {
                const int assigned_literal = trail_[propagation_cursor_++];
                const auto variable = static_cast<std::size_t>(assigned_literal > 0
                                                                    ? assigned_literal : -assigned_literal);
                for (std::size_t clause_id : occurrences_[variable]) {
                    const auto& clause = clauses_[clause_id];
                    bool satisfied = false;
                    int unassigned = 0;
                    int last = 0;
                    for (int literal : clause) {
                        const int8_t value = assigned_[static_cast<std::size_t>(literal > 0 ? literal : -literal)];
                        if (value == -1) { ++unassigned; last = literal; }
                        else if ((value == 1) == (literal > 0)) { satisfied = true; break; }
                    }
                    if (satisfied) continue;
                    if (!unassigned) return false;
                    if (unassigned == 1) {
                        if (!assign(last)) return false;
                    }
                }
            }
            return true;
        }
        bool search() {
            if (!propagate()) return false;
            int choice = 0;
            std::size_t shortest = static_cast<std::size_t>(-1);
            for (const auto& clause : clauses_) {
                bool satisfied = false;
                std::size_t remaining = 0;
                int candidate = 0;
                for (int literal : clause) {
                    const int variable = literal > 0 ? literal : -literal;
                    const int8_t value = assigned_[static_cast<std::size_t>(variable)];
                    if (value == -1) {
                        ++remaining;
                        if (!candidate || variable > atom_base_) candidate = variable;
                    } else if ((value == 1) == (literal > 0)) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied && remaining < shortest) {
                    shortest = remaining;
                    choice = candidate;
                }
            }
            if (!choice) return true;
            const std::size_t mark = trail_.size();
            if (assign(choice) && search()) return true;
            undo(mark);
            if (assign(-choice) && search()) return true;
            undo(mark);
            return false;
        }
    public:
        SatSolver(const std::vector<Formula>& formulas, const std::vector<Atom>& atoms,
                  const std::vector<std::pair<int, bool>>& facts, int goal,
                  PredicateLimits limits) {
            // Snapshot formula caches may contain other queries. Encode only
            // nodes reachable from this query's assumptions and goal.
            std::unordered_set<int> seen;
            std::vector<int> pending{goal};
            for (const auto& fact : facts) pending.push_back(fact.first);
            while (!pending.empty()) {
                const int id = pending.back(); pending.pop_back();
                if (!seen.insert(id).second) continue;
                if (seen.size() > limits.max_formulas) {
                    limit_exceeded_ = true;
                    return;
                }
                const Formula& f = formulas[static_cast<std::size_t>(id)];
                if (f.kind == 2 || f.kind == 3 || f.kind == 4) pending.push_back(f.a);
                if (f.kind == 3 || f.kind == 4) pending.push_back(f.b);
            }
            std::vector<int> selected(seen.begin(), seen.end());
            std::sort(selected.begin(), selected.end());
            formula_var_.reserve(selected.size());
            for (std::size_t i = 0; i < selected.size(); ++i)
                formula_var_.emplace(selected[i], static_cast<int>(i + 1));
            atom_base_ = static_cast<int>(selected.size());
            std::vector<std::size_t> selected_atoms;
            for (int id : selected) {
                const Formula& f = formulas[static_cast<std::size_t>(id)];
                if (f.kind == 1) selected_atoms.push_back(static_cast<std::size_t>(f.a));
            }
            std::sort(selected_atoms.begin(), selected_atoms.end());
            selected_atoms.erase(std::unique(selected_atoms.begin(), selected_atoms.end()),
                                 selected_atoms.end());
            if (selected_atoms.size() > limits.max_atoms) {
                limit_exceeded_ = true;
                return;
            }
            std::unordered_map<std::size_t, int> atom_var;
            atom_var.reserve(selected_atoms.size());
            for (std::size_t i = 0; i < selected_atoms.size(); ++i)
                atom_var.emplace(selected_atoms[i], atom_base_ + static_cast<int>(i) + 1);
            assigned_.assign(selected.size() + selected_atoms.size() + 1, -1);
            occurrences_.resize(assigned_.size());
            for (int id : selected) {
                const int variable = formula_var_.at(id);
                const Formula& f = formulas[static_cast<std::size_t>(id)];
                switch (f.kind) {
                case 0: clause({f.a ? variable : -variable}); break;
                case 1: {
                    const int atom = atom_var.at(static_cast<std::size_t>(f.a));
                    clause({-variable, atom});
                    clause({variable, -atom});
                    break;
                }
                case 2: {
                    const int child = formula_var_.at(f.a);
                    clause({-variable, -child});
                    clause({variable, child});
                    break;
                }
                case 3: {
                    const int left = formula_var_.at(f.a), right = formula_var_.at(f.b);
                    clause({-variable, left});
                    clause({-variable, right});
                    clause({variable, -left, -right});
                    break;
                }
                default: {
                    const int left = formula_var_.at(f.a), right = formula_var_.at(f.b);
                    clause({variable, -left});
                    clause({variable, -right});
                    clause({-variable, left, right});
                    break;
                }
                }
            }
            for (std::size_t i = 0; i < selected_atoms.size(); ++i) {
                const Atom& current = atoms[selected_atoms[i]];
                if (!current.selector) continue;
                for (std::size_t j = 0; j < i; ++j) {
                    const Atom& previous = atoms[selected_atoms[j]];
                    if (previous.selector &&
                        sameOperand(*current.selector, *previous.selector) &&
                        !sameConstant(current.literal->constant, previous.literal->constant))
                        clause({-atom_var.at(selected_atoms[i]), -atom_var.at(selected_atoms[j])});
                }
            }
        }
        std::optional<bool> satisfiable(const std::vector<std::pair<int, bool>>& facts,
                                        int goal, bool goal_value) {
            if (limit_exceeded_) return std::nullopt;
            for (int literal : unit_literals_)
                if (!assign(literal)) { undo(0); return false; }
            for (const auto& fact : facts)
                if (!assign(fact.second ? formula_var_.at(fact.first) : -formula_var_.at(fact.first))) {
                    undo(0); return false;
                }
            if (!assign(goal_value ? formula_var_.at(goal) : -formula_var_.at(goal))) {
                undo(0); return false;
            }
            const bool result = search();
            undo(0);
            return result;
        }
    };
    void resetQuery() {
        formulas_.clear(); atoms_.clear(); formula_cache_.clear();
        atom_cache_.clear(); selector_cache_.clear();
    }
    std::pair<std::vector<std::pair<int, bool>>, int> buildQuery(const Context& context,
                                                                  const Operand& guard) {
        if (!snapshot_) resetQuery();
        std::vector<std::pair<int, bool>> facts;
        facts.reserve(context.predicates.size());
        for (const auto& p : context.predicates) facts.push_back({build(p.guard), p.when_true});
        return {std::move(facts), build(guard)};
    }
public:
    explicit PredicateRelations(const Program& program, bool snapshot = false,
                                PredicateLimits limits = {})
        : program_(program), snapshot_(snapshot), limits_(limits) {}
    ProofOutcome impliesDetailed(const Context& context, Predicate target) {
        auto [facts, goal] = buildQuery(context, target.guard);
        SatSolver solver(formulas_, atoms_, facts, goal, limits_);
        const auto counterexample = solver.satisfiable(facts, goal, !target.when_true);
        if (!counterexample) return ProofOutcome::ResourceLimit;
        return *counterexample ? ProofOutcome::Counterexample : ProofOutcome::Proven;
    }
    Proof implies(const Context& context, Predicate target) {
        return asProof(impliesDetailed(context, std::move(target)));
    }
    std::pair<ProofOutcome, ProofOutcome> classifyDetailed(const Context& context,
                                                           const Operand& guard) {
        auto [facts, goal] = buildQuery(context, guard);
        SatSolver solver(formulas_, atoms_, facts, goal, limits_);
        const auto can_be_false = solver.satisfiable(facts, goal, false);
        const auto can_be_true = solver.satisfiable(facts, goal, true);
        auto outcome = [](std::optional<bool> satisfiable) {
            if (!satisfiable) return ProofOutcome::ResourceLimit;
            return *satisfiable ? ProofOutcome::Counterexample : ProofOutcome::Proven;
        };
        return {outcome(can_be_false), outcome(can_be_true)};
    }
    std::pair<Proof, Proof> classify(const Context& context, const Operand& guard) {
        const auto [when_true, when_false] = classifyDetailed(context, guard);
        return {asProof(when_true), asProof(when_false)};
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
                              std::vector<NodeId>& worklist,
                              const std::vector<bool>* relevant) {
    if (operand.kind != OperandKind::Symbol) return;
    if (operand.node == kInvalidNodeId || operand.node >= contexts.size()) return;
    if (relevant && !(*relevant)[operand.node]) return;
    if (appendContext(contexts[operand.node], context)) worklist.push_back(operand.node);
}

inline bool needsContext(const Operand& operand, const std::vector<bool>* relevant) {
    return operand.kind == OperandKind::Symbol &&
           (!relevant || (operand.node < relevant->size() && (*relevant)[operand.node]));
}

inline void propagateOperandContexts(const Operation& op,
                                     const Context& context,
                                     std::vector<std::vector<Context>>& contexts,
                                     std::vector<NodeId>& worklist,
                                     const std::vector<bool>* relevant) {
    if (op.kind == OperationKind::Ite && op.operands.size() == 3) {
        pushSymbolContext(op.operands[0], context, contexts, worklist, relevant);
        if (needsContext(op.operands[1], relevant))
            pushSymbolContext(op.operands[1], branchContext(context, op.operands[0], true),
                              contexts, worklist, relevant);
        if (needsContext(op.operands[2], relevant))
            pushSymbolContext(op.operands[2], branchContext(context, op.operands[0], false),
                              contexts, worklist, relevant);
        return;
    }
    if (op.kind == OperationKind::Case && hasValidCaseShape(op)) {
        Context remaining = context;
        for (std::size_t branch = 0; branch < caseBranchCount(op); ++branch) {
            const Operand& condition = op.operands[branch * 2];
            pushSymbolContext(condition, remaining, contexts, worklist, relevant);
            if (needsContext(op.operands[branch * 2 + 1], relevant))
                pushSymbolContext(op.operands[branch * 2 + 1],
                                  branchContext(remaining, condition, true),
                                  contexts, worklist, relevant);
            remaining = branchContext(remaining, condition, false);
        }
        pushSymbolContext(op.operands.back(), remaining, contexts, worklist, relevant);
        return;
    }
    for (const auto& operand : op.operands) {
        pushSymbolContext(operand, context, contexts, worklist, relevant);
    }
}

// An Ite with an unconditional use cannot have one branch selected for every
// use. Find those uses without constructing any predicate contexts.
inline std::vector<bool> unconditionallyReachable(const MutableProgram& graph) {
    const Program& program = graph.program();
    std::vector<bool> reachable(program.signals.size(), false);
    std::vector<NodeId> worklist;
    auto enqueue = [&](const Operand& operand) {
        if (operand.kind != OperandKind::Symbol || operand.node >= reachable.size() ||
            reachable[operand.node]) return;
        reachable[operand.node] = true;
        worklist.push_back(operand.node);
    };
    for (const auto& signal : program.signals) {
        if (!graph.isObservable(signal)) continue;
        if (!reachable[signal.id]) {
            reachable[signal.id] = true;
            worklist.push_back(signal.id);
        }
    }
    for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
        const Signal* signal = program.findSignal(worklist[cursor]);
        if (!signal || !signal->driver) continue;
        const Operation& op = *signal->driver;
        if (op.kind == OperationKind::Ite && op.operands.size() == 3) {
            enqueue(op.operands[0]);
        } else if (op.kind == OperationKind::Case && hasValidCaseShape(op)) {
            enqueue(op.operands[0]);
        } else {
            for (const auto& operand : op.operands) enqueue(operand);
        }
    }
    return reachable;
}

// Only users on a path from an output to a candidate need demand contexts.
inline std::vector<bool> relevantToCandidates(const Program& program,
                                              const std::vector<bool>& candidates) {
    std::vector<std::vector<NodeId>> users(program.signals.size());
    for (const auto& signal : program.signals) {
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind == OperandKind::Symbol && operand.node < users.size())
                users[operand.node].push_back(signal.id);
        }
    }
    std::vector<bool> relevant = candidates;
    std::vector<NodeId> worklist;
    for (const auto& signal : program.signals)
        if (candidates[signal.id]) worklist.push_back(signal.id);
    for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor) {
        for (NodeId user : users[worklist[cursor]]) {
            if (relevant[user]) continue;
            relevant[user] = true;
            worklist.push_back(user);
        }
    }
    return relevant;
}

inline std::vector<std::vector<Context>> analyzeDemandContexts(
    const MutableProgram& graph, const std::vector<bool>* relevant = nullptr) {
    const Program& program = graph.program();
    std::vector<std::vector<Context>> contexts(program.signals.size());
    std::vector<NodeId> worklist;
    for (const auto& signal : program.signals) {
        if (graph.isObservable(signal) && (!relevant || (*relevant)[signal.id]) &&
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
            propagateOperandContexts(*signal->driver, context, contexts, worklist, relevant);
        }
    }
    return contexts;
}

// This mirrors the proof's Boolean decomposition. Hash collisions only admit
// extra SAT queries; they cannot authorize a rewrite.
class PredicateSupport {
    const Program& program_;
    std::map<Operand, std::vector<std::size_t>, decltype(&operandLess)> cache_{&operandLess};

    void collect(const Operand& value, std::unordered_set<NodeId>& visited,
                 std::unordered_set<std::size_t>& support) {
        if (value.kind == OperandKind::Literal) return;
        if (value.kind == OperandKind::Symbol && !visited.insert(value.node).second) return;
        if (const auto found = cache_.find(value); found != cache_.end()) {
            support.insert(found->second.begin(), found->second.end());
            return;
        }
        const Operation* op = symbolDriver(value, program_);
        if (op && op->kind == OperationKind::Assign && op->operands.size() == 1 &&
            sameValueType(op->type, op->operands[0].type)) {
            const auto& child = of(op->operands[0]);
            support.insert(child.begin(), child.end());
        } else if (op && op->kind == OperationKind::Unary && op->op == OpCode::LogicNot &&
                   op->operands.size() == 1) {
            const auto& child = of(op->operands[0]);
            support.insert(child.begin(), child.end());
        } else if (op && op->kind == OperationKind::Binary && op->operands.size() == 2 &&
                   (op->op == OpCode::LogicAnd || op->op == OpCode::LogicOr ||
                    (value.type.width == 1 && op->operands[0].type.width == 1 &&
                     op->operands[1].type.width == 1 &&
                     (op->op == OpCode::BitAnd || op->op == OpCode::BitOr)))) {
            for (const auto& operand : op->operands) {
                const auto& child = of(operand);
                support.insert(child.begin(), child.end());
            }
        } else {
            support.insert(predicateHash(Predicate{value, true}));
            if (op && op->kind == OperationKind::Binary && op->op == OpCode::Eq &&
                op->operands.size() == 2) {
                for (const auto& operand : op->operands)
                    if (operand.kind != OperandKind::Literal)
                        support.insert(predicateHash(Predicate{operand, true}));
            }
        }
    }
public:
    explicit PredicateSupport(const Program& program) : program_(program) {}

    const std::vector<std::size_t>& of(const Operand& value) {
        if (const auto found = cache_.find(value); found != cache_.end()) return found->second;
        std::unordered_set<NodeId> visited;
        std::unordered_set<std::size_t> support;
        collect(value, visited, support);
        auto [found, inserted] = cache_.emplace(value, std::vector<std::size_t>(support.begin(), support.end()));
        return found->second;
    }
};

class PredicateDemandIndex {
    std::vector<bool> eligible_;
public:
    PredicateDemandIndex(const Program& program,
                         const std::vector<std::vector<Context>>& contexts,
                         const std::vector<NodeId>& candidates,
                         PredicateSupport& support, std::size_t max_contexts)
        : eligible_(program.signals.size(), false) {
        struct Posting { NodeId candidate; std::size_t context; };
        std::unordered_map<std::size_t, std::vector<Posting>> inverted;
        std::vector<std::vector<bool>> covered(program.signals.size());
        std::vector<std::size_t> counts(program.signals.size(), 0);
        for (NodeId id : candidates) {
            const auto& demands = contexts[id];
            if (demands.empty() || demands.size() > max_contexts || hasUnconditional(demands))
                continue;
            covered[id].resize(demands.size(), false);
            for (std::size_t i = 0; i < demands.size(); ++i) {
                std::unordered_set<std::size_t> atoms;
                for (const auto& predicate : demands[i].predicates) {
                    const auto& keys = support.of(predicate.guard);
                    atoms.insert(keys.begin(), keys.end());
                }
                for (auto atom : atoms) inverted[atom].push_back({id, i});
            }
        }
        // Join each atom's demand postings with the candidates using that atom.
        std::unordered_map<std::size_t, std::unordered_set<NodeId>> guards;
        for (NodeId id : candidates) {
            if (covered[id].empty()) continue;
            for (auto atom : support.of(program.signal(id).driver->operands[0]))
                guards[atom].insert(id);
        }
        for (const auto& [atom, postings] : inverted) {
            const auto found = guards.find(atom);
            if (found == guards.end()) continue;
            for (const auto& posting : postings) {
                if (!found->second.count(posting.candidate)) continue;
                if (!covered[posting.candidate][posting.context]) {
                    covered[posting.candidate][posting.context] = true;
                    ++counts[posting.candidate];
                }
            }
        }
        for (NodeId id : candidates)
            eligible_[id] = !covered[id].empty() && counts[id] == covered[id].size();
    }

    bool mayBenefitFromProof(NodeId candidate) const {
        return candidate < eligible_.size() && eligible_[candidate];
    }
};

// Compatibility helper for callers that analyze one guard outside the sink
// pass. sinkPredicates uses PredicateDemandIndex to avoid rebuilding this map.
inline bool mayBenefitFromProof(const Operand& guard, const std::vector<Context>& contexts,
                                PredicateSupport& support) {
    if (contexts.empty()) return false;
    const auto& guard_atoms = support.of(guard);
    std::unordered_set<std::size_t> keys(guard_atoms.begin(), guard_atoms.end());
    for (const auto& context : contexts) {
        if (isUnconditional(context)) return false;
        bool overlap = false;
        for (const auto& predicate : context.predicates) {
            const auto& atoms = support.of(predicate.guard);
            for (auto atom : atoms) if (keys.count(atom)) { overlap = true; break; }
            if (overlap) break;
        }
        if (!overlap) return false;
    }
    return !guard_atoms.empty();
}

class SnapshotPredicateRelations {
    struct Key {
        Context context;
        Operand guard;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const {
            std::uint64_t seed = key.context.fingerprint;
            hashCombine(seed, predicateHash(Predicate{key.guard, true}));
            return static_cast<std::size_t>(seed);
        }
    };
    struct KeyEqual {
        bool operator()(const Key& lhs, const Key& rhs) const {
            return sameContext(lhs.context, rhs.context) && sameOperand(lhs.guard, rhs.guard);
        }
    };
    PredicateRelations relations_;
    const Program& program_;
    PredicateLimits limits_;
    std::unordered_map<Key, std::pair<ProofOutcome, ProofOutcome>, KeyHash, KeyEqual> cache_;
    mutable std::unordered_map<std::size_t,
                               std::unordered_map<std::size_t, std::vector<Predicate>>> context_index_;
    mutable std::unordered_map<std::uint64_t, std::int8_t> structural_memo_;

    static std::uint64_t memoKey(std::size_t fingerprint, NodeId node) {
        std::uint64_t key = static_cast<std::uint64_t>(fingerprint);
        hashCombine(key, node);
        return key;
    }

    const auto& contextIndex(const Context& context) const {
        auto found = context_index_.find(context.fingerprint);
        if (found != context_index_.end()) return found->second;
        std::unordered_map<std::size_t, std::vector<Predicate>> index;
        for (const auto& predicate : context.predicates)
            index[predicateHash(Predicate{predicate.guard, true})].push_back(predicate);
        return context_index_.emplace(context.fingerprint, std::move(index)).first->second;
    }

    static std::optional<bool> constantTruth(const Operand& value) {
        if (value.kind != OperandKind::Literal || value.type.width != 1 || value.type.isArray())
            return std::nullopt;
        return !value.constant.isZero();
    }

    std::optional<bool> indexedContextTruth(const Context& context, const Operand& value) const {
        const auto& index = contextIndex(context);
        const auto found = index.find(predicateHash(Predicate{value, true}));
        if (found == index.end()) return std::nullopt;
        std::optional<bool> result;
        for (const auto& predicate : found->second) {
            // Cheap identity check first; sameOperand is the collision-safe fallback.
            if (predicate.guard.kind != value.kind || predicate.guard.node != value.node ||
                predicate.guard.text != value.text || !sameOperand(predicate.guard, value))
                continue;
            if (result && *result != predicate.when_true) return std::nullopt;
            result = predicate.when_true;
        }
        return result;
    }

    std::optional<bool> structuralTruth(const Context& context, const Operand& value,
                                        unsigned depth = 0) const {
        if (depth > 64) return std::nullopt;
        if (value.kind == OperandKind::Symbol) {
            const auto key = memoKey(context.fingerprint, value.node);
            const auto found = structural_memo_.find(key);
            if (found != structural_memo_.end()) {
                if (found->second < 0) return std::nullopt;
                return found->second != 0;
            }
        }
        std::optional<bool> result;
        if (const auto constant = constantTruth(value)) return constant;
        if (const auto exact = indexedContextTruth(context, value)) result = exact;
        if (result) {
            if (value.kind == OperandKind::Symbol)
                structural_memo_[memoKey(context.fingerprint, value.node)] = *result ? 1 : 0;
            return result;
        }
        const Operation* driver = symbolDriver(value, program_);
        if (!driver) result = std::nullopt;
        else if (driver->kind == OperationKind::Assign && driver->operands.size() == 1 &&
                 sameValueType(driver->type, driver->operands[0].type))
            result = structuralTruth(context, driver->operands[0], depth + 1);
        else if (driver->kind == OperationKind::Unary && driver->op == OpCode::LogicNot &&
                 driver->operands.size() == 1) {
            if (const auto nested = structuralTruth(context, driver->operands[0], depth + 1))
                result = !*nested;
        } else if (!driver || driver->kind != OperationKind::Binary || driver->operands.size() != 2) {
            result = std::nullopt;
        } else {
            const bool is_and = driver->op == OpCode::LogicAnd || driver->op == OpCode::BitAnd;
            const bool is_or = driver->op == OpCode::LogicOr || driver->op == OpCode::BitOr;
            if (is_and || is_or) {
                const auto lhs = structuralTruth(context, driver->operands[0], depth + 1);
                const auto rhs = structuralTruth(context, driver->operands[1], depth + 1);
                if (is_and) {
                    if ((lhs && !*lhs) || (rhs && !*rhs)) result = false;
                    else if (lhs && rhs && *lhs && *rhs) result = true;
                } else {
                    if ((lhs && *lhs) || (rhs && *rhs)) result = true;
                    else if (lhs && rhs && !*lhs && !*rhs) result = false;
                }
            }
        }
        if (value.kind == OperandKind::Symbol)
            structural_memo_[memoKey(context.fingerprint, value.node)] = result ? (*result ? 1 : 0) : -1;
        return result;
    }
public:
    explicit SnapshotPredicateRelations(const Program& program, PredicateLimits limits = {})
        : relations_(program, true, limits), program_(program), limits_(limits) {}
    std::pair<ProofOutcome, ProofOutcome> classify(const Context& context, const Operand& guard) {
        // Layers 1-5: conservative identity, constant, literal-set and structural proofs.
        if (const auto truth = structuralTruth(context, guard)) {
            return {*truth ? ProofOutcome::Counterexample : ProofOutcome::Proven,
                    *truth ? ProofOutcome::Proven : ProofOutcome::Counterexample};
        }
        Key key{context, guard};
        if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
        if (context.predicates.size() > limits_.max_formulas)
            return {ProofOutcome::ResourceLimit, ProofOutcome::ResourceLimit};
        auto result = relations_.classifyDetailed(context, guard);
        cache_.emplace(std::move(key), result);
        return result;
    }
};

inline std::optional<bool> guardedDefaultSelection(const Operation& op,
                                                    const std::vector<Context>& contexts,
                                                    SnapshotPredicateRelations& relations) {
    if (op.kind != OperationKind::Ite || op.operands.size() != 3 || contexts.empty())
        return std::nullopt;
    bool always_true = true, always_false = true;
    for (const auto& context : contexts) {
        const auto [proves_true, proves_false] = relations.classify(context, op.operands[0]);
        always_true &= proves_true == ProofOutcome::Proven;
        always_false &= proves_false == ProofOutcome::Proven;
        if (!always_true && !always_false) return std::nullopt;
    }
    if (always_true) return true;
    if (always_false) return false;
    return std::nullopt;
}

} // namespace predicate_detail

inline bool sinkPredicates(MutableProgram& graph,
                           predicate_detail::PredicateLimits limits = {}) {
    Program& program = graph.program();
    const auto unconditional = predicate_detail::unconditionallyReachable(graph);
    std::vector<bool> candidates(program.signals.size(), false);
    bool any_candidate = false;
    for (const auto& signal : program.signals) {
        if (!graph.isObservable(signal) && signal.driver &&
            signal.driver->kind == OperationKind::Ite && signal.driver->operands.size() == 3 &&
            !unconditional[signal.id]) {
            candidates[signal.id] = true;
            any_candidate = true;
        }
    }
    if (!any_candidate) return false;
    const auto relevant = predicate_detail::relevantToCandidates(program, candidates);
    const auto contexts = predicate_detail::analyzeDemandContexts(graph, &relevant);
    predicate_detail::PredicateSupport support(program);
    std::vector<NodeId> ranked_candidates;
    for (const auto& signal : program.signals)
        if (candidates[signal.id]) ranked_candidates.push_back(signal.id);
    for (NodeId id : ranked_candidates) support.of(program.signal(id).driver->operands[0]);
    std::sort(ranked_candidates.begin(), ranked_candidates.end(),
              [&](NodeId lhs, NodeId rhs) {
                  return support.of(program.signal(lhs).driver->operands[0]).size() >
                         support.of(program.signal(rhs).driver->operands[0]).size();
              });
    if (ranked_candidates.size() > limits.max_candidate_ites)
        ranked_candidates.resize(limits.max_candidate_ites);
    std::vector<bool> selected_candidates(program.signals.size(), false);
    for (NodeId id : ranked_candidates) selected_candidates[id] = true;
    const predicate_detail::PredicateDemandIndex demand_index(
        program, contexts, ranked_candidates, support, limits.max_contexts_per_candidate);
    predicate_detail::SnapshotPredicateRelations relations(program, limits);
    std::vector<std::pair<NodeId, bool>> rewrites;
    for (const auto& signal : program.signals) {
        if (!selected_candidates[signal.id] ||
            contexts[signal.id].size() > limits.max_contexts_per_candidate) {
            continue;
        }
        if (!demand_index.mayBenefitFromProof(signal.id)) continue;
        const auto selected = predicate_detail::guardedDefaultSelection(
            *signal.driver, contexts[signal.id], relations);
        if (selected) rewrites.emplace_back(signal.id, *selected);
    }
    for (const auto& [id, take_true] : rewrites) {
        Signal& signal = program.signal(id);
        Operation& op = *signal.driver;
        const ValueType type = op.type;
        Operand value = op.operands[take_true ? 1 : 2];
        predicate_detail::setAssign(op, std::move(value), type,
                                    take_true
                                        ? "sank predicate guard and omitted unreachable false branch"
                                        : "sank predicate guard and omitted unreachable true branch",
                                    program);
        signal.debug = op.debug;
    }
    if (!rewrites.empty()) graph.markValueFactsDirty();
    return !rewrites.empty();
}

} // namespace pred::beir::opt
