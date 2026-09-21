#pragma once

#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"

#include <algorithm>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred::beir::opt {
namespace boolean_detail {

struct Literal {
    Operand base;
    bool negated = false;
};

struct LiteralHash {
    std::size_t operator()(const Literal& value) const {
        std::uint64_t seed = static_cast<std::uint64_t>(value.base.kind);
        hashCombine(seed, value.base.node);
        hashCombine(seed, std::hash<std::string>{}(value.base.text));
        hashCombine(seed, static_cast<std::uint64_t>(value.base.type.width));
        for (int dim : value.base.type.array_dims) hashCombine(seed, static_cast<std::uint64_t>(dim));
        hashCombine(seed, value.base.signed_view ? 1 : 0);
        if (value.base.kind == OperandKind::Literal) {
            hashCombine(seed, static_cast<std::uint64_t>(value.base.constant.width));
            hashCombine(seed, value.base.constant.signed_view ? 1 : 0);
            for (auto limb : value.base.constant.limbs) hashCombine(seed, limb);
        }
        hashCombine(seed, value.negated ? 1 : 0);
        return static_cast<std::size_t>(seed);
    }
};

struct LiteralEqual {
    bool operator()(const Literal& lhs, const Literal& rhs) const {
        return lhs.negated == rhs.negated &&
               predicate_detail::sameOperand(lhs.base, rhs.base);
    }
};

struct LiteralCache {
    std::unordered_map<NodeId, Operand> resolved;
    std::unordered_map<NodeId, Literal> literals;
};

inline Operand resolveAssign(const Operand& value, const Program& program,
                             LiteralCache* cache = nullptr) {
    if (cache && value.kind == OperandKind::Symbol) {
        auto found = cache->resolved.find(value.node);
        if (found != cache->resolved.end()) return found->second;
    }
    Operand current = value;
    while (current.kind == OperandKind::Symbol) {
        const auto* driver = predicate_detail::symbolDriver(current, program);
        if (!driver || driver->kind != OperationKind::Assign ||
            driver->operands.size() != 1 ||
            !predicate_detail::sameValueType(driver->type, driver->operands[0].type)) break;
        current = driver->operands[0];
    }
    if (cache && value.kind == OperandKind::Symbol) cache->resolved.emplace(value.node, current);
    return current;
}

inline Literal literalOf(Operand value, const Program& program, LiteralCache* cache = nullptr) {
    if (cache && value.kind == OperandKind::Symbol) {
        auto found = cache->literals.find(value.node);
        if (found != cache->literals.end()) return found->second;
    }
    const NodeId original_node = value.kind == OperandKind::Symbol ? value.node : kInvalidNodeId;
    value = resolveAssign(value, program, cache);
    bool negated = false;
    while (const auto* driver = predicate_detail::symbolDriver(value, program)) {
        if (driver->kind != OperationKind::Unary ||
            driver->op != OpCode::LogicNot || driver->operands.size() != 1 ||
            value.type.width != 1 || value.type.isArray()) break;
        negated = !negated;
        value = resolveAssign(driver->operands[0], program, cache);
    }
    Literal result{std::move(value), negated};
    if (cache && original_node != kInvalidNodeId) cache->literals.emplace(original_node, result);
    return result;
}

inline bool sameLiteral(const Literal& lhs, const Literal& rhs) {
    return lhs.negated == rhs.negated &&
           predicate_detail::sameOperand(lhs.base, rhs.base);
}

inline bool complementary(const Literal& lhs, const Literal& rhs) {
    return lhs.negated != rhs.negated &&
           predicate_detail::sameOperand(lhs.base, rhs.base);
}

inline bool literalLess(const Literal& lhs, const Literal& rhs) {
    if (!predicate_detail::sameOperand(lhs.base, rhs.base))
        return predicate_detail::operandLess(lhs.base, rhs.base);
    return lhs.negated < rhs.negated;
}

inline std::optional<bool> boolConstant(const Operand& value, const Program& program) {
    const Operand resolved = resolveAssign(value, program);
    if (resolved.kind != OperandKind::Literal || resolved.type.width != 1 ||
        resolved.type.isArray()) return std::nullopt;
    return !resolved.constant.isZero();
}

inline Operand constant(bool value) {
    Operand out;
    out.kind = OperandKind::Literal;
    out.type = ValueType{1, {}};
    out.constant.width = 1;
    out.constant.limbs = {value ? 1u : 0u};
    return out;
}

inline Operand emit(Program& program,
                    OperationKind kind,
                    OpCode opcode,
                    std::vector<Operand> operands,
                    const DebugInfo& debug,
                    const char* reason) {
    Operation op;
    op.kind = kind;
    op.op = opcode;
    op.type = ValueType{1, {}};
    op.operands = std::move(operands);
    op.debug = debug;
    return width_detail::appendTemp(program, op.type, std::move(op), reason);
}

inline Operand materialize(Program& program,
                           const Literal& value,
                           const DebugInfo& debug) {
    if (!value.negated) return value.base;
    return emit(program, OperationKind::Unary, OpCode::LogicNot, {value.base}, debug,
                "materialized normalized Boolean complement");
}

inline void collectBoolean(const Operand& value,
                           bool conjunction,
                           const Program& program,
                           std::vector<Operand>& leaves,
                           LiteralCache* cache = nullptr) {
    const Operand resolved = resolveAssign(value, program, cache);
    const auto* driver = predicate_detail::symbolDriver(resolved, program);
    const bool matches = driver && driver->kind == OperationKind::Binary &&
        driver->type.width == 1 && !driver->type.isArray() &&
        driver->operands.size() == 2 &&
        (conjunction
             ? (driver->op == OpCode::BitAnd || driver->op == OpCode::LogicAnd)
             : (driver->op == OpCode::BitOr || driver->op == OpCode::LogicOr));
    if (matches) {
        collectBoolean(driver->operands[0], conjunction, program, leaves, cache);
        collectBoolean(driver->operands[1], conjunction, program, leaves, cache);
    } else {
        leaves.push_back(resolved);
    }
}

inline Operand buildBalanced(Program& program,
                             OpCode opcode,
                             std::vector<Operand> values,
                             const DebugInfo& debug) {
    if (values.empty()) return constant(opcode == OpCode::BitAnd);
    while (values.size() > 1) {
        std::vector<Operand> next;
        next.reserve((values.size() + 1) / 2);
        for (std::size_t i = 0; i < values.size(); i += 2) {
            if (i + 1 == values.size()) next.push_back(values[i]);
            else next.push_back(emit(program, OperationKind::Binary, opcode,
                                     {values[i], values[i + 1]}, debug,
                                     "rebuilt normalized Boolean tree"));
        }
        values = std::move(next);
    }
    return values.front();
}

inline void setAssign(Program& program,
                      NodeId id,
                      Operand value,
                      DebugInfo debug,
                      const char* reason) {
    Operation op;
    op.kind = OperationKind::Assign;
    op.type = ValueType{1, {}};
    op.operands = {std::move(value)};
    op.debug = std::move(debug);
    addDebugMessage(op.debug, reason);
    program.signal(id).driver = std::move(op);
}

inline std::vector<Literal> termFactors(const Operand& term,
                                        bool conjunction,
                                        const Program& program,
                                        LiteralCache* cache = nullptr) {
    std::vector<Operand> operands;
    collectBoolean(term, conjunction, program, operands, cache);
    std::vector<Literal> result;
    result.reserve(operands.size());
    for (auto& operand : operands) result.push_back(literalOf(std::move(operand), program, cache));
    std::sort(result.begin(), result.end(), literalLess);
    result.erase(std::unique(result.begin(), result.end(), sameLiteral), result.end());
    return result;
}

inline bool subset(const std::vector<Literal>& lhs,
                   const std::vector<Literal>& rhs) {
    return std::includes(rhs.begin(), rhs.end(), lhs.begin(), lhs.end(), literalLess);
}

// Returns the common factors when two terms differ only by X versus !X.
inline std::optional<std::vector<Literal>> consensus(const std::vector<Literal>& lhs,
                                                     const std::vector<Literal>& rhs) {
    if (lhs.size() != rhs.size() || lhs.empty()) return std::nullopt;
    std::vector<Literal> common;
    std::size_t i = 0, j = 0;
    bool saw_complement = false;
    while (i < lhs.size() && j < rhs.size()) {
        if (sameLiteral(lhs[i], rhs[j])) {
            common.push_back(lhs[i]); ++i; ++j; continue;
        }
        if (!saw_complement && complementary(lhs[i], rhs[j])) {
            saw_complement = true; ++i; ++j; continue;
        }
        return std::nullopt;
    }
    if (i != lhs.size() || j != rhs.size() || !saw_complement) return std::nullopt;
    return common;
}

inline bool simplifyAssociative(Program& program, NodeId id) {
    const Operation original = *program.signal(id).driver;
    if (original.kind != OperationKind::Binary || original.operands.size() != 2 ||
        original.type.width != 1 || original.type.isArray() ||
        (original.op != OpCode::BitAnd && original.op != OpCode::BitOr &&
         original.op != OpCode::LogicAnd && original.op != OpCode::LogicOr)) return false;
    const bool conjunction = original.op == OpCode::BitAnd || original.op == OpCode::LogicAnd;
    const OpCode outer = conjunction ? OpCode::BitAnd : OpCode::BitOr;
    const OpCode inner = conjunction ? OpCode::BitOr : OpCode::BitAnd;
    LiteralCache cache;
    std::vector<Operand> terms;
    collectBoolean(original.operands[0], conjunction, program, terms, &cache);
    collectBoolean(original.operands[1], conjunction, program, terms, &cache);

    bool changed = original.op != outer;
    std::vector<Operand> filtered;
    std::unordered_set<Literal, LiteralHash, LiteralEqual> seen;
    for (auto& term : terms) {
        if (auto value = boolConstant(resolveAssign(term, program, &cache), program)) {
            if (*value != conjunction) {
                setAssign(program, id, constant(*value), original.debug,
                          "folded dominating Boolean constant");
                return true;
            }
            changed = true;
            continue;
        }
        Literal literal = literalOf(term, program, &cache);
        Literal complement = literal;
        complement.negated = !complement.negated;
        bool duplicate = !seen.insert(literal).second;
        if (duplicate) {
            changed = true;
        } else if (seen.count(complement)) {
                setAssign(program, id, constant(!conjunction), original.debug,
                          "folded complementary Boolean operands");
                return true;
        }
        if (!duplicate) {
            filtered.push_back(resolveAssign(term, program, &cache));
        }
    }
    terms = std::move(filtered);

    // Absorption: X | (X & Y) = X, with the dual rule for conjunction.
    std::vector<std::vector<Literal>> factors;
    factors.reserve(terms.size());
    for (const auto& term : terms)
        factors.push_back(termFactors(term, !conjunction, program, &cache));
    bool absorbed = true;
    while (absorbed) {
        absorbed = false;
        for (std::size_t i = 0; !absorbed && i < factors.size(); ++i) {
            for (std::size_t j = i + 1; j < factors.size(); ++j) {
                std::size_t victim = factors.size();
                if (subset(factors[i], factors[j])) victim = j;
                else if (subset(factors[j], factors[i])) victim = i;
                if (victim == factors.size()) continue;
                terms.erase(terms.begin() + victim);
                factors.erase(factors.begin() + victim);
                changed = absorbed = true;
                break;
            }
        }
    }

    // Merge one complementary pair per invocation. The optimizer fixed point
    // handles additional pairs without creating rewrite cycles.
    bool merged = false;
    for (std::size_t i = 0; !merged && i < factors.size(); ++i) {
        for (std::size_t j = i + 1; j < factors.size(); ++j) {
            auto common = consensus(factors[i], factors[j]);
            if (!common) continue;
            std::vector<Operand> common_values;
            for (const auto& value : *common)
                common_values.push_back(materialize(program, value, original.debug));
            Operand replacement = buildBalanced(program, inner, std::move(common_values),
                                                original.debug);
            terms[i] = replacement;
            terms.erase(terms.begin() + j);
            merged = changed = true;
            break;
        }
    }

    if (!changed) return false;
    Operand result = buildBalanced(program, outer, std::move(terms), original.debug);
    setAssign(program, id, std::move(result), original.debug,
              "normalized Boolean associative expression");
    return true;
}

inline bool normalizeIte(Program& program, NodeId id) {
    const Operation original = *program.signal(id).driver;
    if (original.kind != OperationKind::Ite || original.operands.size() != 3 ||
        original.type.width != 1 || original.type.isArray()) return false;
    LiteralCache cache;
    Operand condition = resolveAssign(original.operands[0], program, &cache);
    Operand when_true = resolveAssign(original.operands[1], program, &cache);
    Operand when_false = resolveAssign(original.operands[2], program, &cache);
    const auto true_constant = boolConstant(when_true, program);
    const auto false_constant = boolConstant(when_false, program);
    auto unary = [&](Operand value) {
        return emit(program, OperationKind::Unary, OpCode::LogicNot, {std::move(value)},
                    original.debug, "normalized Boolean Ite complement");
    };
    auto binary = [&](OpCode opcode, Operand lhs, Operand rhs) {
        return emit(program, OperationKind::Binary, opcode,
                    {std::move(lhs), std::move(rhs)}, original.debug,
                    "normalized side-effect-free Boolean Ite");
    };
    Operand result;
    bool matched = true;
    const Literal condition_literal = literalOf(condition, program, &cache);
    const Literal true_literal = literalOf(when_true, program, &cache);
    const Literal false_literal = literalOf(when_false, program, &cache);
    if (sameLiteral(true_literal, condition_literal)) {
        // C ? C : F == C | F
        result = binary(OpCode::BitOr, condition, when_false);
    } else if (complementary(true_literal, condition_literal)) {
        // C ? !C : F == !C & F
        result = binary(OpCode::BitAnd, unary(condition), when_false);
    } else if (sameLiteral(false_literal, condition_literal)) {
        // C ? T : C == C & T
        result = binary(OpCode::BitAnd, condition, when_true);
    } else if (complementary(false_literal, condition_literal)) {
        // C ? T : !C == !C | T
        result = binary(OpCode::BitOr, unary(condition), when_true);
    } else if (true_constant && false_constant) {
        result = *true_constant == *false_constant
            ? constant(*true_constant)
            : (*true_constant ? condition : unary(condition));
    } else if (true_constant && *true_constant) {
        result = binary(OpCode::BitOr, condition, when_false);
    } else if (true_constant && !*true_constant) {
        result = binary(OpCode::BitAnd, unary(condition), when_false);
    } else if (false_constant && !*false_constant) {
        result = binary(OpCode::BitAnd, condition, when_true);
    } else if (false_constant && *false_constant) {
        result = binary(OpCode::BitOr, unary(condition), when_true);
    } else {
        matched = false;
        const Literal fallback = literalOf(when_false, program, &cache);
        std::vector<Operand> factors;
        collectBoolean(condition, true, program, factors, &cache);
        for (std::size_t i = 0; i < factors.size(); ++i) {
            const Literal factor = literalOf(factors[i], program, &cache);
            if (!sameLiteral(factor, fallback) && !complementary(factor, fallback)) continue;
            std::vector<Operand> residual_factors;
            for (std::size_t j = 0; j < factors.size(); ++j)
                if (j != i) residual_factors.push_back(factors[j]);
            Operand residual = buildBalanced(program, OpCode::BitAnd,
                                             std::move(residual_factors), original.debug);
            if (complementary(factor, fallback)) {
                result = binary(OpCode::BitOr, when_false,
                                binary(OpCode::BitAnd, residual, when_true));
            } else {
                result = binary(OpCode::BitAnd, when_false,
                                binary(OpCode::BitOr, unary(residual), when_true));
            }
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    setAssign(program, id, std::move(result), original.debug,
              "normalized side-effect-free Boolean Ite");
    return true;
}

} // namespace boolean_detail

inline bool normalizeBooleanControl(MutableProgram& graph) {
    Program& program = graph.program();
    const std::size_t original_count = program.signals.size();
    const auto order = width_detail::topologicalOrder(program);
    std::vector<std::vector<NodeId>> users(original_count);
    for (const auto& signal : program.signals) {
        if (signal.id >= original_count || !signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind == OperandKind::Symbol && operand.node < original_count)
                users[operand.node].push_back(signal.id);
        }
    }
    std::vector<bool> queued(original_count, false);
    std::queue<NodeId> work;
    for (NodeId id : order) {
        if (id < original_count) {
            work.push(id);
            queued[id] = true;
        }
    }
    bool changed = false;
    while (!work.empty()) {
        const NodeId id = work.front();
        work.pop();
        queued[id] = false;
        if (id >= original_count || !program.signal(id).driver) continue;
        const bool node_changed = boolean_detail::normalizeIte(program, id) ||
                                  boolean_detail::simplifyAssociative(program, id);
        if (!node_changed) continue;
        changed = true;
        for (NodeId user : users[id]) {
            if (!queued[user]) {
                work.push(user);
                queued[user] = true;
            }
        }
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
