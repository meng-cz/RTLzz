#pragma once

#include "ast/AST.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace pred {

// A single guarded assignment in the final predicate form.
// Semantics: when guard is true, target := value.
struct GuardedAssign {
    ExprPtr guard;      // predicate condition (nullptr or literal "1" means always)
    ExprPtr target;     // LHS (variable, array element, struct field)
    ExprPtr value;      // RHS expression (may contain ite)
    TypeInfo type;      // type of the assignment
    DebugLoc debug_loc; // best source location for this assignment
};

struct OutputExpression {
    std::string name;
    ExprPtr expr;
    TypeInfo type;
    bool has_default = false;
    std::string default_value;
    std::string default_reason;
    std::string guard_relation; // Backward-compatible alias for assignment_coverage.
    std::string default_policy;
    bool default_applied = false;
    std::string assignment_coverage;
    std::string default_guard_relation;
    std::string paired_control;
    std::string valid_when;
    std::string inactive_value;
    std::string inactive_semantics;
};

// The final output: a flat list of guarded assignments with no control flow.
struct PredicateProgram {
    std::string function_name;
    std::vector<GuardedAssign> assignments;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<OutputExpression> output_expressions;
    std::vector<std::string> diagnostics;
    // Symbol table: variable name -> type info
    std::unordered_map<std::string, TypeInfo> symbols;
    std::unordered_map<std::string, std::string> param_directions;
    std::unordered_map<std::string, DebugLoc> param_debug_locs;
    std::unordered_map<std::string, std::string> output_default_reasons;
    std::unordered_map<std::string, std::string> output_paired_controls;
    std::unordered_map<std::string, std::vector<std::string>> lookup_tables;
};

// Helper: create an ite (if-then-else) expression
inline ExprPtr make_ite(ExprPtr cond, ExprPtr then_val, ExprPtr else_val, TypeInfo type = {}) {
    return make_ternary(std::move(cond), std::move(then_val), std::move(else_val), type);
}

// Helper: create a conjunction (a && b)
inline ExprPtr make_and(ExprPtr a, ExprPtr b) {
    if (!a || (a->kind == ExprKind::Literal && a->literal_value == "1")) return b;
    if (!b || (b->kind == ExprKind::Literal && b->literal_value == "1")) return a;
    return make_binary("&&", std::move(a), std::move(b), TypeInfo{"bool", 1, false});
}

// Helper: create a negation (!a)
inline ExprPtr make_not(ExprPtr a) {
    if (!a) return make_literal("1");
    return make_unary("!", std::move(a), TypeInfo{"bool", 1, false});
}

// Helper: true guard
inline ExprPtr make_true_guard() {
    return make_literal("1", TypeInfo{"bool", 1, false});
}

} // namespace pred
