#include "predicate/OutputExpressionMap.h"
#include "predicate/DefaultTotalizationPass.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pred {
namespace {

constexpr int kMaxOutputSubstitutionDepth = 4096;

bool isTrueGuard(const ExprPtr& e) {
    return !e || (e->kind == ExprKind::Literal &&
        (e->literal_value == "1" || e->literal_value == "true"));
}

bool isFalseGuard(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal &&
        (e->literal_value == "0" || e->literal_value == "false");
}

bool exprEqual(const ExprPtr& a, const ExprPtr& b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case ExprKind::Literal:
        return a->literal_value == b->literal_value;
    case ExprKind::VarRef:
        return a->var_name == b->var_name;
    case ExprKind::UnaryOp:
        return a->op == b->op && exprEqual(a->operand, b->operand);
    case ExprKind::BinaryOp:
        return a->op == b->op && exprEqual(a->left, b->left) && exprEqual(a->right, b->right);
    default:
        return false;
    }
}

bool complementaryGuards(const ExprPtr& a, const ExprPtr& b) {
    return (a && a->kind == ExprKind::UnaryOp && a->op == "!" && exprEqual(a->operand, b)) ||
           (b && b->kind == ExprKind::UnaryOp && b->op == "!" && exprEqual(b->operand, a));
}

ExprPtr orGuard(ExprPtr a, ExprPtr b) {
    if (isTrueGuard(a) || isTrueGuard(b) || complementaryGuards(a, b)) {
        return make_literal("1", TypeInfo{"bool", 1, false});
    }
    if (!a || isFalseGuard(a)) return b;
    if (!b || isFalseGuard(b)) return a;
    if (exprEqual(a, b)) return a;
    return make_binary("||", std::move(a), std::move(b), TypeInfo{"bool", 1, false});
}

std::string targetName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::ArrayAccess) {
        std::string base = targetName(e->array_base);
        if (base.empty()) return "";
        if (e->index && e->index->kind == ExprKind::Literal) {
            return base + "_" + e->index->literal_value;
        }
    }
    return "";
}

ExprPtr cloneSubstituteImpl(const ExprPtr& e,
                            const std::unordered_map<std::string, ExprPtr>& env,
                            std::unordered_set<std::string>& visiting,
                            std::unordered_map<std::string, ExprPtr>& memo,
                            int depth);

ExprPtr cloneExprShallow(const ExprPtr& e) {
    if (!e) return nullptr;
    auto out = std::make_shared<Expr>(*e);
    return out;
}

ExprPtr cloneSubstituteImpl(const ExprPtr& e,
                            const std::unordered_map<std::string, ExprPtr>& env,
                            std::unordered_set<std::string>& visiting,
                            std::unordered_map<std::string, ExprPtr>& memo,
                            int depth) {
    if (!e) return nullptr;
    if (depth > kMaxOutputSubstitutionDepth) {
        throw std::runtime_error(
            "OutputExpressionMap: expression substitution depth exceeds " +
            std::to_string(kMaxOutputSubstitutionDepth));
    }
    if (e->kind == ExprKind::VarRef) {
        auto it = env.find(e->var_name);
        if (it != env.end()) {
            auto memo_it = memo.find(e->var_name);
            if (memo_it != memo.end()) return memo_it->second;
            if (visiting.count(e->var_name)) return cloneExprShallow(e);
            visiting.insert(e->var_name);
            auto out = cloneSubstituteImpl(it->second, env, visiting, memo, depth + 1);
            visiting.erase(e->var_name);
            memo[e->var_name] = out;
            return out;
        }
        return cloneExprShallow(e);
    }
    auto out = cloneExprShallow(e);
    out->left = cloneSubstituteImpl(e->left, env, visiting, memo, depth + 1);
    out->right = cloneSubstituteImpl(e->right, env, visiting, memo, depth + 1);
    out->operand = cloneSubstituteImpl(e->operand, env, visiting, memo, depth + 1);
    out->array_base = cloneSubstituteImpl(e->array_base, env, visiting, memo, depth + 1);
    out->index = cloneSubstituteImpl(e->index, env, visiting, memo, depth + 1);
    out->struct_base = cloneSubstituteImpl(e->struct_base, env, visiting, memo, depth + 1);
    out->cast_expr = cloneSubstituteImpl(e->cast_expr, env, visiting, memo, depth + 1);
    out->cond = cloneSubstituteImpl(e->cond, env, visiting, memo, depth + 1);
    out->then_expr = cloneSubstituteImpl(e->then_expr, env, visiting, memo, depth + 1);
    out->else_expr = cloneSubstituteImpl(e->else_expr, env, visiting, memo, depth + 1);
    out->base = cloneSubstituteImpl(e->base, env, visiting, memo, depth + 1);
    out->value = cloneSubstituteImpl(e->value, env, visiting, memo, depth + 1);
    out->args.clear();
    for (auto& arg : e->args) out->args.push_back(cloneSubstituteImpl(arg, env, visiting, memo, depth + 1));
    out->parts.clear();
    for (auto& part : e->parts) out->parts.push_back(cloneSubstituteImpl(part, env, visiting, memo, depth + 1));
    return out;
}

ExprPtr cloneSubstitute(const ExprPtr& e,
                        const std::unordered_map<std::string, ExprPtr>& env) {
    std::unordered_set<std::string> visiting;
    std::unordered_map<std::string, ExprPtr> memo;
    return cloneSubstituteImpl(e, env, visiting, memo, 0);
}

ExprPtr cloneExprDeep(const ExprPtr& e) {
    static const std::unordered_map<std::string, ExprPtr> empty_env;
    return cloneSubstitute(e, empty_env);
}

bool containsName(const std::vector<std::string>& values, const std::string& name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

std::string stripSsaSuffix(const std::string& name) {
    size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return name;
    for (size_t i = pos + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, pos);
}

bool numericUnderscoreSuffix(const std::string& suffix) {
    if (suffix.empty() || suffix.front() == '_' || suffix.back() == '_') return false;
    bool saw_digit = false;
    for (char ch : suffix) {
        if (ch == '_') continue;
        if (ch < '0' || ch > '9') return false;
        saw_digit = true;
    }
    return saw_digit;
}

bool hasFlattenedBinding(const std::unordered_map<std::string, ExprPtr>& env,
                         const std::string& name) {
    std::string prefix = name + "_";
    for (const auto& item : env) {
        if (item.first.rfind(prefix, 0) != 0) continue;
        std::string suffix = item.first.substr(prefix.size());
        if (numericUnderscoreSuffix(suffix)) return true;
    }
    return false;
}

std::vector<std::string> flattenedBindings(const std::unordered_map<std::string, ExprPtr>& env,
                                           const std::string& name) {
    std::vector<std::string> out;
    std::string prefix = name + "_";
    for (const auto& item : env) {
        if (item.first.rfind(prefix, 0) != 0) continue;
        std::string suffix = item.first.substr(prefix.size());
        if (numericUnderscoreSuffix(suffix)) {
            out.push_back(item.first);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> flattenedSymbols(const std::unordered_map<std::string, TypeInfo>& symbols,
                                          const std::string& name) {
    std::vector<std::string> out;
    std::string prefix = name + "_";
    bool base_is_array = false;
    auto base_it = symbols.find(name);
    if (base_it != symbols.end()) {
        base_is_array = base_it->second.is_array;
    }
    for (const auto& item : symbols) {
        if (item.first.rfind(prefix, 0) != 0 || item.second.is_array) continue;
        if (base_is_array && !numericUnderscoreSuffix(item.first.substr(prefix.size()))) {
            continue;
        }
        if (!base_is_array || numericUnderscoreSuffix(item.first.substr(prefix.size()))) {
            out.push_back(item.first);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool exprContainsAnyVar(const ExprPtr& e, const std::unordered_set<std::string>& names) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) return names.count(e->var_name) > 0;
    if (exprContainsAnyVar(e->left, names) || exprContainsAnyVar(e->right, names) ||
        exprContainsAnyVar(e->operand, names) || exprContainsAnyVar(e->array_base, names) ||
        exprContainsAnyVar(e->index, names) || exprContainsAnyVar(e->struct_base, names) ||
        exprContainsAnyVar(e->cast_expr, names) || exprContainsAnyVar(e->cond, names) ||
        exprContainsAnyVar(e->then_expr, names) || exprContainsAnyVar(e->else_expr, names) ||
        exprContainsAnyVar(e->base, names) || exprContainsAnyVar(e->value, names)) {
        return true;
    }
    for (auto& arg : e->args) {
        if (exprContainsAnyVar(arg, names)) return true;
    }
    for (auto& part : e->parts) {
        if (exprContainsAnyVar(part, names)) return true;
    }
    return false;
}

bool exprContainsVarPrefix(const ExprPtr& e, const std::string& prefix) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) {
        return e->var_name.rfind(prefix, 0) == 0;
    }
    if (exprContainsVarPrefix(e->left, prefix) ||
        exprContainsVarPrefix(e->right, prefix) ||
        exprContainsVarPrefix(e->operand, prefix) ||
        exprContainsVarPrefix(e->array_base, prefix) ||
        exprContainsVarPrefix(e->index, prefix) ||
        exprContainsVarPrefix(e->struct_base, prefix) ||
        exprContainsVarPrefix(e->cast_expr, prefix) ||
        exprContainsVarPrefix(e->cond, prefix) ||
        exprContainsVarPrefix(e->then_expr, prefix) ||
        exprContainsVarPrefix(e->else_expr, prefix) ||
        exprContainsVarPrefix(e->base, prefix) ||
        exprContainsVarPrefix(e->value, prefix)) {
        return true;
    }
    for (const auto& arg : e->args) {
        if (exprContainsVarPrefix(arg, prefix)) return true;
    }
    for (const auto& part : e->parts) {
        if (exprContainsVarPrefix(part, prefix)) return true;
    }
    return false;
}

bool exprContainsVar(const ExprPtr& e, const std::string& name) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) return e->var_name == name;
    if (exprContainsVar(e->left, name) ||
        exprContainsVar(e->right, name) ||
        exprContainsVar(e->operand, name) ||
        exprContainsVar(e->array_base, name) ||
        exprContainsVar(e->index, name) ||
        exprContainsVar(e->struct_base, name) ||
        exprContainsVar(e->cast_expr, name) ||
        exprContainsVar(e->cond, name) ||
        exprContainsVar(e->then_expr, name) ||
        exprContainsVar(e->else_expr, name) ||
        exprContainsVar(e->base, name) ||
        exprContainsVar(e->value, name)) {
        return true;
    }
    for (const auto& arg : e->args) {
        if (exprContainsVar(arg, name)) return true;
    }
    for (const auto& part : e->parts) {
        if (exprContainsVar(part, name)) return true;
    }
    return false;
}

std::string baseNameOfSsa(const std::string& name) {
    return stripSsaSuffix(name);
}

std::string semanticReasonFor(const PredicateProgram& program, const std::string& name) {
    auto it = program.output_default_reasons.find(name);
    if (it != program.output_default_reasons.end()) return it->second;
    std::size_t best = 0;
    std::string reason;
    for (const auto& item : program.output_default_reasons) {
        const std::string prefix = item.first + "_";
        if (name.rfind(prefix, 0) == 0 && item.first.size() > best &&
            numericUnderscoreSuffix(name.substr(prefix.size()))) {
            best = item.first.size();
            reason = item.second;
        }
    }
    return reason;
}

std::string pairedControlFor(const PredicateProgram& program, const std::string& name) {
    auto it = program.output_paired_controls.find(name);
    if (it != program.output_paired_controls.end()) return it->second;
    std::size_t best = 0;
    std::string control;
    for (const auto& item : program.output_paired_controls) {
        const std::string prefix = item.first + "_";
        if (name.rfind(prefix, 0) == 0 && item.first.size() > best &&
            numericUnderscoreSuffix(name.substr(prefix.size()))) {
            best = item.first.size();
            const std::string suffix = name.substr(item.first.size());
            auto control_type = program.symbols.find(item.second);
            control = control_type != program.symbols.end() && control_type->second.is_array
                ? item.second + suffix
                : item.second;
        }
    }
    return control;
}

ExprPtr defaultValueFor(const std::string& name, const TypeInfo& type) {
    return makeDefaultTotalizedValue(name, type);
}

DefaultTotalizationDecision classifyDefaultForProgram(const PredicateProgram& program,
                                                      const std::string& name,
                                                      const TypeInfo& type) {
    return classifyDefaultTotalization(name, type, semanticReasonFor(program, name));
}

std::string defaultLiteralForMetadata(const std::string& name, const TypeInfo& type) {
    auto value = defaultValueFor(name, type);
    if (value && value->kind == ExprKind::Literal) return value->literal_value;
    return "";
}

std::string defaultDiagnostic(const std::string& name,
                              const DefaultTotalizationDecision& decision,
                              const std::string& default_value) {
    std::string msg = "default_applied: " + name +
        " default_value=" + default_value + " default_reason=" + decision.reason;
    if (decision.guard_controlled) {
        msg += " guard_relation=guarded";
    } else {
        msg += " guard_relation=none";
    }
    return msg;
}

std::string defaultGuardRelation(const DefaultTotalizationDecision& decision) {
    return decision.guard_controlled ? "guarded" : "none";
}

struct GuardedDefinition {
    ExprPtr guard;
    ExprPtr value;
};

using GuardedDefinitions = std::unordered_map<std::string, std::vector<GuardedDefinition>>;

ExprPtr simplifyOutputExpr(const ExprPtr& e, const GuardedDefinitions& defs);

ExprPtr controlExprForOutput(const PredicateProgram& program,
                             const std::unordered_map<std::string, ExprPtr>& env,
                             const std::unordered_map<std::string, std::string>& latest_for_output,
                             const std::unordered_map<std::string, ExprPtr>& final_substitutions,
                             const GuardedDefinitions& guarded_definitions,
                             const std::string& control_name) {
    if (control_name.empty()) return nullptr;
    auto direct = env.find(control_name);
    if (direct != env.end()) {
        return simplifyOutputExpr(cloneSubstitute(direct->second, final_substitutions),
                                  guarded_definitions);
    }
    auto latest = latest_for_output.find(control_name);
    if (latest != latest_for_output.end()) {
        auto latest_env = env.find(latest->second);
        if (latest_env != env.end()) {
            return simplifyOutputExpr(cloneSubstitute(latest_env->second, final_substitutions),
                                      guarded_definitions);
        }
    }

    TypeInfo control_type = make_bool_type();
    auto symbol_it = program.symbols.find(control_name);
    if (symbol_it != program.symbols.end() && symbol_it->second.width > 0) {
        control_type = symbol_it->second;
    }

    auto dir_it = program.param_directions.find(control_name);
    if (dir_it != program.param_directions.end() &&
        dir_it->second == "Input") {
        return make_var(control_name + "_0", control_type);
    }
    return make_var(control_name, control_type);
}

bool isLiteral(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal;
}

ExprPtr literalForGuardedDefinition(const std::string& name,
                                    const ExprPtr& guard,
                                    const GuardedDefinitions& defs) {
    auto it = defs.find(name);
    if (it == defs.end()) return nullptr;
    for (const auto& def : it->second) {
        if (exprEqual(def.guard, guard) && isLiteral(def.value)) {
            return def.value;
        }
    }
    return nullptr;
}

ExprPtr unconditionalDefinition(const ExprPtr& e,
                                const GuardedDefinitions& defs) {
    if (!e || e->kind != ExprKind::VarRef) return e;
    auto it = defs.find(e->var_name);
    if (it == defs.end()) return e;
    for (const auto& def : it->second) {
        if (isTrueGuard(def.guard)) return def.value;
    }
    return e;
}

ExprPtr simplifyUnary(const ExprPtr& e) {
    if (!e || e->kind != ExprKind::UnaryOp) return e;
    if (e->op == "!") {
        if (isTrueGuard(e->operand)) return make_literal("false", TypeInfo{"bool", 1, false, true, "bool"});
        if (isFalseGuard(e->operand)) return make_literal("true", TypeInfo{"bool", 1, false, true, "bool"});
    }
    return e;
}

ExprPtr simplifyBinary(const ExprPtr& e) {
    if (!e || e->kind != ExprKind::BinaryOp) return e;
    if (e->op == "&&") {
        if (isFalseGuard(e->left) || isFalseGuard(e->right)) {
            return make_literal("false", TypeInfo{"bool", 1, false, true, "bool"});
        }
        if (isTrueGuard(e->left)) return e->right;
        if (isTrueGuard(e->right)) return e->left;
        if (exprEqual(e->left, e->right)) return e->left;
    } else if (e->op == "||") {
        if (isTrueGuard(e->left) || isTrueGuard(e->right)) {
            return make_literal("true", TypeInfo{"bool", 1, false, true, "bool"});
        }
        if (isFalseGuard(e->left)) return e->right;
        if (isFalseGuard(e->right)) return e->left;
        if (exprEqual(e->left, e->right)) return e->left;
    }
    return e;
}

ExprPtr resizeForFullOverwrite(const ExprPtr& value, const TypeInfo& target_type) {
    if (!value || target_type.width <= 0) return value;
    int value_width = value->type.width;
    if (value_width <= 0 || value_width == target_type.width) {
        auto out = cloneExprDeep(value);
        if (out) out->type = target_type;
        return out;
    }
    if (value_width > target_type.width) {
        return make_trunc(cloneExprDeep(value), target_type.width, target_type.is_signed);
    }
    if (value->type.is_signed || value->type.hw_kind == "signed_view") {
        return make_sext(cloneExprDeep(value), target_type.width);
    }
    return make_zext(cloneExprDeep(value), target_type.width);
}

ExprPtr simplifyFullOverwrite(const ExprPtr& e) {
    if (!e) return e;
    if (e->kind == ExprKind::WriteSlice && e->base && e->base->type.width > 0 &&
        e->lo == 0 && e->hi == e->base->type.width - 1) {
        TypeInfo target_type = e->type.width > 0 ? e->type : e->base->type;
        return resizeForFullOverwrite(e->value, target_type);
    }
    if (e->kind == ExprKind::WriteBit && e->base && e->base->type.width == 1 && e->bit == 0) {
        TypeInfo target_type = e->type.width > 0 ? e->type : e->base->type;
        return resizeForFullOverwrite(e->value, target_type);
    }
    return e;
}

ExprPtr simplifySlice(const ExprPtr& e) {
    if (!e || e->kind != ExprKind::Slice || !e->base || e->base->kind != ExprKind::Slice) {
        return e;
    }

    const auto& inner = e->base;
    if (!inner->base || e->lo < 0 || e->hi < e->lo || inner->lo < 0 || inner->hi < inner->lo) {
        return e;
    }

    int inner_width = inner->hi - inner->lo + 1;
    if (e->lo == 0 && e->hi == inner_width - 1) {
        auto out = cloneExprDeep(inner);
        if (e->type.width > 0) out->type = e->type;
        return out;
    }

    int new_lo = inner->lo + e->lo;
    int new_hi = inner->lo + e->hi;
    if (new_hi > inner->hi) return e;

    TypeInfo out_type = e->type.width > 0 ? e->type : TypeInfo{};
    if (out_type.width <= 0) {
        out_type = inner->type;
        out_type.width = new_hi - new_lo + 1;
        out_type.name = "UInt<" + std::to_string(out_type.width) + ">";
        out_type.hw_kind = "uint";
        out_type.is_hw_int = true;
        out_type.is_signed = false;
    }
    return make_slice(cloneExprDeep(inner->base), new_hi, new_lo, out_type);
}

bool isLogicalNegation(const ExprPtr& candidate, const ExprPtr& value) {
    return candidate && candidate->kind == ExprKind::UnaryOp &&
           candidate->op == "!" && exprEqual(candidate->operand, value);
}

ExprPtr simplifyTernary(const ExprPtr& e, const GuardedDefinitions& defs) {
    if (!e || e->kind != ExprKind::Ternary) return e;
    if (isTrueGuard(e->cond)) return e->then_expr;
    if (isFalseGuard(e->cond)) return e->else_expr;
    if (e->then_expr && e->then_expr->kind == ExprKind::VarRef) {
        if (auto replacement = literalForGuardedDefinition(e->then_expr->var_name, e->cond, defs)) {
            e->then_expr = cloneExprDeep(replacement);
        }
    }
    if (e->else_expr && e->else_expr->kind == ExprKind::VarRef) {
        auto not_cond = make_unary("!", cloneExprDeep(e->cond), TypeInfo{"bool", 1, false});
        if (auto replacement = literalForGuardedDefinition(e->else_expr->var_name, not_cond, defs)) {
            e->else_expr = cloneExprDeep(replacement);
        }
    }
    if (exprEqual(e->then_expr, e->else_expr)) return e->then_expr;
    if (isTrueGuard(e->then_expr) && isFalseGuard(e->else_expr)) return e->cond;
    if (isFalseGuard(e->then_expr) && isTrueGuard(e->else_expr)) {
        return make_unary("!", e->cond, TypeInfo{"bool", 1, false});
    }
    ExprPtr resolved_else = unconditionalDefinition(e->else_expr, defs);
    if (resolved_else && resolved_else->kind == ExprKind::Ternary &&
        isLogicalNegation(resolved_else->cond, e->cond)) {
        return make_ternary(cloneExprDeep(e->cond),
                            cloneExprDeep(e->then_expr),
                            cloneExprDeep(resolved_else->then_expr),
                            e->type);
    }
    if (resolved_else && resolved_else->kind == ExprKind::Ternary &&
        exprEqual(resolved_else->cond, e->cond)) {
        return make_ternary(cloneExprDeep(e->cond),
                            cloneExprDeep(e->then_expr),
                            cloneExprDeep(resolved_else->else_expr),
                            e->type);
    }
    ExprPtr resolved_then = unconditionalDefinition(e->then_expr, defs);
    if (resolved_then && resolved_then->kind == ExprKind::Ternary &&
        exprEqual(resolved_then->cond, e->cond)) {
        return make_ternary(cloneExprDeep(e->cond),
                            cloneExprDeep(resolved_then->then_expr),
                            cloneExprDeep(e->else_expr),
                            e->type);
    }
    return e;
}

ExprPtr simplifyOutputExpr(const ExprPtr& e, const GuardedDefinitions& defs) {
    if (!e) return nullptr;
    auto out = cloneExprShallow(e);
    out->left = simplifyOutputExpr(e->left, defs);
    out->right = simplifyOutputExpr(e->right, defs);
    out->operand = simplifyOutputExpr(e->operand, defs);
    out->array_base = simplifyOutputExpr(e->array_base, defs);
    out->index = simplifyOutputExpr(e->index, defs);
    out->struct_base = simplifyOutputExpr(e->struct_base, defs);
    out->cast_expr = simplifyOutputExpr(e->cast_expr, defs);
    out->cond = simplifyOutputExpr(e->cond, defs);
    out->then_expr = simplifyOutputExpr(e->then_expr, defs);
    out->else_expr = simplifyOutputExpr(e->else_expr, defs);
    out->base = simplifyOutputExpr(e->base, defs);
    out->value = simplifyOutputExpr(e->value, defs);
    out->args.clear();
    for (auto& arg : e->args) out->args.push_back(simplifyOutputExpr(arg, defs));
    out->parts.clear();
    for (auto& part : e->parts) out->parts.push_back(simplifyOutputExpr(part, defs));

    out = simplifyUnary(out);
    out = simplifyBinary(out);
    out = simplifyTernary(out, defs);
    out = simplifySlice(out);
    out = simplifyFullOverwrite(out);
    return out;
}

ExprPtr mergeWithGuard(const ExprPtr& guard,
                       const ExprPtr& value,
                       const ExprPtr& previous,
                       const TypeInfo& type) {
    if (isTrueGuard(guard)) return value;
    return make_ternary(guard, value, previous, type.width > 0 ? type : value->type);
}

std::string coverageForOutput(const std::string& output_name,
                              const std::string& source_name,
                              bool defaulted,
                              bool fully_covered,
                              const std::string& semantic_reason,
                              const DefaultTotalizationDecision& default_decision,
                              const std::unordered_map<std::string, bool>& has_unconditional_update,
                              const std::unordered_map<std::string, bool>& has_guarded_update) {
    auto read_flag = [](const std::unordered_map<std::string, bool>& map,
                        const std::string& key) {
        auto it = map.find(key);
        return it != map.end() && it->second;
    };
    bool uncond = read_flag(has_unconditional_update, output_name) ||
                  read_flag(has_unconditional_update, source_name);
    bool guarded = read_flag(has_guarded_update, output_name) ||
                   read_flag(has_guarded_update, source_name);

    if (defaulted) {
        if (default_decision.guard_controlled) return "guarded";
        return "default_only";
    }
    if (semantic_reason == "valid_default_false" && uncond && guarded) {
        return "explicit_initialization_plus_guarded_update";
    }
    if (uncond && !guarded) return "unconditional";
    if (!uncond && guarded) return "guarded";
    if (uncond && guarded) return "mixed_unconditional_and_guarded";
    if (fully_covered) return "unconditional";
    return "unknown";
}

} // namespace

void buildOutputExpressionMap(PredicateProgram& program) {
    program.output_expressions.clear();

    std::unordered_map<std::string, ExprPtr> env;
    std::unordered_map<std::string, ExprPtr> seeded_default_values;
    std::unordered_map<std::string, TypeInfo> types;
    std::unordered_map<std::string, bool> has_default;
    std::unordered_map<std::string, ExprPtr> guard_coverage;
    std::unordered_map<std::string, ExprPtr> literal_definitions;
    GuardedDefinitions guarded_definitions;
    std::unordered_map<std::string, bool> has_unconditional_update;
    std::unordered_map<std::string, bool> has_guarded_update;
    std::vector<std::string> order;
    std::unordered_set<std::string> seeded_default_names;

    auto seed_output = [&](const std::string& output_name) {
        TypeInfo type = program.symbols.count(output_name) ? program.symbols.at(output_name) : TypeInfo{};
        std::string initial_name = output_name + "_0";
        env[initial_name] = defaultValueFor(output_name, type);
        seeded_default_values[initial_name] = env[initial_name];
        types[initial_name] = type;
        has_default[initial_name] = true;
        seeded_default_names.insert(initial_name);
    };
    for (const auto& output_name : program.outputs) {
        if (!semanticReasonFor(program, output_name).empty()) {
            seed_output(output_name);
        }
        for (const auto& flattened_name : flattenedSymbols(program.symbols, output_name)) {
            if (!semanticReasonFor(program, flattened_name).empty()) {
                seed_output(flattened_name);
            }
        }
    }

    for (auto& assign : program.assignments) {
        std::string name = targetName(assign.target);
        if (name.empty()) {
            program.diagnostics.push_back("OutputExpressionMap skipped non-scalar target");
            continue;
        }
        assign.value = simplifyOutputExpr(assign.value, guarded_definitions);
        if (!containsName(order, name)) order.push_back(name);

        TypeInfo type = assign.type.width > 0 ? assign.type :
            (assign.target ? assign.target->type : TypeInfo{});
        if (type.width <= 0 && assign.value) type = assign.value->type;
        types[name] = type;

        ExprPtr raw_rhs = cloneExprDeep(assign.value);
        bool raw_rhs_depends_on_seeded_default = exprContainsAnyVar(raw_rhs, seeded_default_names);
        ExprPtr rhs = cloneSubstitute(raw_rhs, seeded_default_values);
        // Output initial SSA versions are policy seeds, not free variables.
        // Persist their explicit values into the assignment graph as well as
        // the final output map so every downstream consumer sees a closed IR.
        assign.value = simplifyOutputExpr(cloneExprDeep(rhs), guarded_definitions);
        auto update_binding = [&](const std::string& binding_name,
                                  const ExprPtr& binding_rhs,
                                  bool rhs_depends_on_seeded_default) {
            ExprPtr prev;
            bool defaulted = false;
            auto prev_it = env.find(binding_name);
            if (prev_it != env.end()) {
                prev = prev_it->second;
                defaulted = has_default[binding_name];
            } else {
                prev = defaultValueFor(binding_name, type);
                defaulted = true;
            }
            env[binding_name] = mergeWithGuard(assign.guard, binding_rhs, prev, type);
            env[binding_name] = simplifyOutputExpr(env[binding_name], guarded_definitions);
            types[binding_name] = type;
            guarded_definitions[binding_name].push_back({
                assign.guard ? cloneExprDeep(assign.guard) : make_literal("1", TypeInfo{"bool", 1, false}),
                cloneExprDeep(binding_rhs)
            });
            if (isTrueGuard(assign.guard) && isLiteral(binding_rhs)) {
                literal_definitions[binding_name] = cloneExprDeep(binding_rhs);
            }
            if (isTrueGuard(assign.guard)) {
                has_unconditional_update[binding_name] = true;
            } else {
                has_guarded_update[binding_name] = true;
            }
            if (!rhs_depends_on_seeded_default) {
                guard_coverage[binding_name] = guard_coverage.count(binding_name)
                    ? orGuard(guard_coverage[binding_name], assign.guard)
                    : (assign.guard ? cloneExprDeep(assign.guard) : make_literal("1", TypeInfo{"bool", 1, false}));
            }
            has_default[binding_name] = defaulted && !isTrueGuard(guard_coverage[binding_name]);
        };

        update_binding(name, rhs, raw_rhs_depends_on_seeded_default);
        std::string logical_name = baseNameOfSsa(name);
        bool logical_is_array = program.symbols.count(logical_name) && program.symbols[logical_name].is_array;
        if (logical_name != name && !logical_is_array) {
            update_binding(logical_name, rhs, raw_rhs_depends_on_seeded_default);
        }
    }

    std::vector<std::string> requested_outputs = program.outputs;
    std::unordered_map<std::string, std::string> latest_for_output;
    for (const auto& name : order) {
        latest_for_output[baseNameOfSsa(name)] = name;
    }

    if (requested_outputs.empty()) requested_outputs = order;

    std::vector<std::string> expanded_outputs;
    std::vector<std::string> pending = requested_outputs;
    for (size_t idx = 0; idx < pending.size(); ++idx) {
        const auto& output_name = pending[idx];
        auto expected = flattenedSymbols(program.symbols, output_name);
        if (!expected.empty()) {
            for (const auto& expected_name : expected) {
                if (env.find(expected_name) == env.end()) {
                    TypeInfo expected_type = program.symbols.count(expected_name) ? program.symbols[expected_name] : TypeInfo{};
                    auto decision = classifyDefaultForProgram(program, expected_name, expected_type);
                    if (decision.allowed) {
                        program.diagnostics.push_back(defaultDiagnostic(
                            expected_name, decision, defaultLiteralForMetadata(expected_name, expected_type)));
                    } else {
                        program.diagnostics.push_back(
                            "missing_assignment_for_non_defaultable_output: " + expected_name +
                            " source=" + output_name + " covered=false seeded=false");
                    }
                }
            }
            for (const auto& child : expected) {
                if (child != output_name && !containsName(pending, child)) {
                    pending.push_back(child);
                }
            }
            continue;
        }

        auto declared_output = program.symbols.find(output_name);
        if (declared_output != program.symbols.end() && !declared_output->second.is_array) {
            if (!containsName(expanded_outputs, output_name)) {
                expanded_outputs.push_back(output_name);
            }
            continue;
        }

        if (declared_output == program.symbols.end() && hasFlattenedBinding(env, output_name)) {
            auto flattened = flattenedBindings(env, output_name);
            for (const auto& child : flattened) {
                if (child != output_name && !containsName(pending, child)) {
                    pending.push_back(child);
                }
            }
            if (!flattened.empty()) continue;
        }
        if (!containsName(expanded_outputs, output_name)) {
            expanded_outputs.push_back(output_name);
        }
    }

    program.outputs.clear();
    for (const auto& output_name : expanded_outputs) {
        std::string source_name = output_name;
        auto latest_it = latest_for_output.find(output_name);
        if (env.find(output_name) != env.end()) {
            source_name = output_name;
        } else if (latest_it != latest_for_output.end()) {
            source_name = latest_it->second;
        }

        auto it = env.find(source_name);
        TypeInfo type = types.count(source_name) ? types[source_name] :
            (program.symbols.count(output_name) ? program.symbols[output_name] :
             (program.symbols.count(source_name) ? program.symbols[source_name] : TypeInfo{}));
        bool uses_seeded_default = it != env.end() && exprContainsAnyVar(it->second, seeded_default_names);
        auto final_substitutions = seeded_default_values;
        for (const auto& item : literal_definitions) {
            final_substitutions[item.first] = item.second;
        }
        ExprPtr expr = it != env.end()
            ? simplifyOutputExpr(cloneSubstitute(it->second, final_substitutions), guarded_definitions)
            : defaultValueFor(output_name, type);
        std::string semantic_reason = semanticReasonFor(program, output_name);
        std::string paired_control = pairedControlFor(program, output_name);
        bool disabled_data_totalized = false;
        if (!paired_control.empty() && paired_control != output_name) {
            auto control_expr = controlExprForOutput(program,
                                                     env,
                                                     latest_for_output,
                                                     final_substitutions,
                                                     guarded_definitions,
                                                     paired_control);
            if (control_expr) {
                expr = simplifyOutputExpr(
                    make_ternary(control_expr,
                                 cloneExprDeep(expr),
                                 defaultValueFor(output_name, type),
                                 type.width > 0 ? type : (expr ? expr->type : TypeInfo{})),
                    guarded_definitions);
                disabled_data_totalized = true;
            }
        }
        bool has_undefined_phi =
            exprContainsVarPrefix(expr, "__undefined_phi_");
        bool fully_covered = guard_coverage.count(source_name) && isTrueGuard(guard_coverage[source_name]);
        bool defaulted = disabled_data_totalized || it == env.end() || (!fully_covered &&
                         (uses_seeded_default || has_default[source_name]));
        if (has_undefined_phi) {
            defaulted = true;
            program.diagnostics.push_back(
                "missing_assignment_for_non_defaultable_output: " + output_name +
                " source=" + source_name +
                " covered=false seeded=false");
        } else if (defaulted) {
            auto decision = classifyDefaultForProgram(program, output_name, type);
            if (decision.allowed) {
                program.diagnostics.push_back(defaultDiagnostic(
                    output_name, decision, defaultLiteralForMetadata(output_name, type)));
            } else {
                program.diagnostics.push_back("missing_assignment_for_non_defaultable_output: " + output_name +
                                              " source=" + source_name +
                                              " covered=" + (fully_covered ? "true" : "false") +
                                              " seeded=" + (uses_seeded_default ? "true" : "false"));
            }
        }
        auto default_decision = classifyDefaultForProgram(program, output_name, type);
        std::string output_default_reason = defaulted ? default_decision.reason : semantic_reason;
        std::string output_default_guard_relation =
            output_default_reason.empty() ? "" : defaultGuardRelation(default_decision);
        std::string assignment_coverage = coverageForOutput(output_name,
                                                            source_name,
                                                            defaulted,
                                                            fully_covered,
                                                            semantic_reason,
                                                            default_decision,
                                                            has_unconditional_update,
                                                            has_guarded_update);
        std::string inactive_value;
        std::string inactive_semantics;
        if (!paired_control.empty()) {
            inactive_value = defaultLiteralForMetadata(output_name, type);
            inactive_semantics = "disabled_data";
        }
        program.output_expressions.push_back({
            output_name,
            expr,
            type,
            defaulted,
            defaulted ? defaultLiteralForMetadata(output_name, type) : "",
            output_default_reason,
            assignment_coverage,
            output_default_reason,
            defaulted,
            assignment_coverage,
            output_default_guard_relation,
            paired_control,
            paired_control,
            inactive_value,
            inactive_semantics
        });
        program.outputs.push_back(output_name);
    }

    if (program.inputs.empty()) {
        if (!program.param_directions.empty()) {
            for (const auto& item : program.param_directions) {
                if (item.second == "Input" &&
                    !containsName(program.outputs, item.first)) {
                    program.inputs.push_back(item.first);
                }
            }
        } else {
            for (const auto& item : program.symbols) {
                if (!containsName(program.outputs, item.first) && item.second.width > 0) {
                    program.inputs.push_back(item.first);
                }
            }
        }
        std::sort(program.inputs.begin(), program.inputs.end());
    }
}

} // namespace pred
