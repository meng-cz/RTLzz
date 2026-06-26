#include "predicate/PredicateVerifier.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace pred {
namespace {

std::string exprName(const ExprPtr& e) {
    if (!e) return "<null>";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    return "<expr>";
}

std::string exprKindName(ExprKind kind) {
    switch (kind) {
    case ExprKind::Literal: return "Literal";
    case ExprKind::VarRef: return "VarRef";
    case ExprKind::BinaryOp: return "BinaryOp";
    case ExprKind::UnaryOp: return "UnaryOp";
    case ExprKind::ArrayAccess: return "ArrayAccess";
    case ExprKind::FieldAccess: return "FieldAccess";
    case ExprKind::Call: return "Call";
    case ExprKind::Cast: return "Cast";
    case ExprKind::Ternary: return "Ternary";
    case ExprKind::ZExt: return "ZExt";
    case ExprKind::SExt: return "SExt";
    case ExprKind::Trunc: return "Trunc";
    case ExprKind::Slice: return "Slice";
    case ExprKind::BitSelect: return "BitSelect";
    case ExprKind::WriteSlice: return "WriteSlice";
    case ExprKind::WriteBit: return "WriteBit";
    case ExprKind::Concat: return "Concat";
    case ExprKind::Repeat: return "Repeat";
    case ExprKind::ReduceOr: return "ReduceOr";
    case ExprKind::ReduceAnd: return "ReduceAnd";
    case ExprKind::ReduceXor: return "ReduceXor";
    }
    return "Unknown";
}

PredicateVerifyResult fail(const std::string& message) {
    return {false, message};
}

struct VerifyState {
    const PredicateProgram* program = nullptr;
    std::unordered_set<const Expr*> verified;
    std::unordered_set<const Expr*> visiting;
};

bool vectorContains(const std::vector<std::string>& values, const std::string& name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

bool isBuiltinDivRemType(const TypeInfo& type) {
    if (type.width <= 0 || type.name == "bool" || type.hw_kind == "bool") return false;
    if (type.hw_kind == "builtin") return true;
    return type.name == "char" || type.name == "signed char" ||
           type.name == "unsigned char" || type.name == "short" ||
           type.name == "unsigned short" || type.name == "int" ||
           type.name == "unsigned int" || type.name == "long" ||
           type.name == "unsigned long" || type.name == "long long" ||
           type.name == "unsigned long long" || type.name == "uint8_t" ||
           type.name == "uint16_t" || type.name == "uint32_t" ||
           type.name == "uint64_t" || type.name == "int8_t" ||
           type.name == "int16_t" || type.name == "int32_t" ||
           type.name == "int64_t";
}

bool exprContainsVar(const ExprPtr& e, const std::string& name) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) return e->var_name == name;
    if (exprContainsVar(e->left, name) || exprContainsVar(e->right, name) ||
        exprContainsVar(e->operand, name) || exprContainsVar(e->array_base, name) ||
        exprContainsVar(e->index, name) || exprContainsVar(e->struct_base, name) ||
        exprContainsVar(e->cast_expr, name) || exprContainsVar(e->cond, name) ||
        exprContainsVar(e->then_expr, name) || exprContainsVar(e->else_expr, name) ||
        exprContainsVar(e->base, name) || exprContainsVar(e->value, name)) {
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

std::string targetName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::ArrayAccess) {
        std::string base = targetName(e->array_base);
        if (base.empty() || !e->index || e->index->kind != ExprKind::Literal) return "";
        return base + "_" + e->index->literal_value;
    }
    return "";
}

std::string stripSsaSuffix(const std::string& name) {
    size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return name;
    for (size_t i = pos + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, pos);
}

bool hasSsaZeroSuffix(const std::string& name) {
    size_t pos = name.rfind('_');
    return pos != std::string::npos && pos + 2 == name.size() && name[pos + 1] == '0';
}

std::string stripTrailingFlattenIndex(const std::string& name) {
    size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return name;
    for (size_t i = pos + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, pos);
}

std::string directionForBaseName(const PredicateProgram& program, const std::string& name) {
    std::string current = name;
    while (!current.empty()) {
        auto it = program.param_directions.find(current);
        if (it != program.param_directions.end()) return it->second;
        std::string parent = stripTrailingFlattenIndex(current);
        if (parent == current) break;
        current = parent;
    }
    size_t best = 0;
    std::string direction;
    for (const auto& item : program.param_directions) {
        const std::string prefix = item.first + "_";
        if (name.rfind(prefix, 0) == 0 && item.first.size() > best) {
            best = item.first.size();
            direction = item.second;
        }
    }
    if (!direction.empty()) return direction;
    return "";
}

using DefinitionMap = std::unordered_map<std::string, ExprPtr>;

PredicateVerifyResult verifyClosedExpr(const ExprPtr& e,
                                       const PredicateProgram& program,
                                       const DefinitionMap& definitions,
                                       std::unordered_set<std::string>& resolving) {
    if (!e) return {};
    if (e->kind == ExprKind::VarRef) {
        auto def_it = definitions.find(e->var_name);
        if (def_it != definitions.end()) {
            if (resolving.count(e->var_name)) {
                return fail("PredicateVerifier: output dependency cycle through '" + e->var_name + "'");
            }
            resolving.insert(e->var_name);
            auto result = verifyClosedExpr(def_it->second, program, definitions, resolving);
            resolving.erase(e->var_name);
            return result;
        }

        std::string base = stripSsaSuffix(e->var_name);
        bool seed = hasSsaZeroSuffix(e->var_name);
        std::string direction = directionForBaseName(program, base);
        if (seed && !direction.empty()) {
            if (direction == "Input" || direction == "InOut") return {};
            return fail("PredicateVerifier: output expression depends on unseeded Output initial value '" +
                        e->var_name + "'");
        }
        if (!direction.empty() && (direction == "Input" || direction == "InOut")) {
            return {};
        }
        return fail("PredicateVerifier: output expression has unresolved variable '" +
                    e->var_name + "'");
    }

    auto check = [&](const ExprPtr& child) {
        return verifyClosedExpr(child, program, definitions, resolving);
    };

    if (auto r = check(e->left); !r.ok) return r;
    if (auto r = check(e->right); !r.ok) return r;
    if (auto r = check(e->operand); !r.ok) return r;
    if (auto r = check(e->array_base); !r.ok) return r;
    if (auto r = check(e->index); !r.ok) return r;
    if (auto r = check(e->struct_base); !r.ok) return r;
    if (auto r = check(e->cast_expr); !r.ok) return r;
    if (auto r = check(e->cond); !r.ok) return r;
    if (auto r = check(e->then_expr); !r.ok) return r;
    if (auto r = check(e->else_expr); !r.ok) return r;
    if (auto r = check(e->base); !r.ok) return r;
    if (auto r = check(e->value); !r.ok) return r;
    for (const auto& arg : e->args) {
        if (auto r = check(arg); !r.ok) return r;
    }
    for (const auto& part : e->parts) {
        if (auto r = check(part); !r.ok) return r;
    }
    return {};
}

PredicateVerifyResult verifyExpr(const ExprPtr& e, VerifyState& state) {
    if (!e) return {};
    if (state.verified.count(e.get())) return {};
    if (state.visiting.count(e.get())) {
        return fail("PredicateVerifier: expression graph contains a cycle");
    }
    state.visiting.insert(e.get());

    auto finish = [&state, &e](PredicateVerifyResult result = {}) {
        state.visiting.erase(e.get());
        if (result.ok) state.verified.insert(e.get());
        return result;
    };

    auto verify_child = [&state](const ExprPtr& child) {
        return verifyExpr(child, state);
    };

    auto expression_width_must_be_known = [](ExprKind kind) {
        return kind != ExprKind::Literal;
    };
    if (expression_width_must_be_known(e->kind) && e->type.width <= 0) {
        return finish(fail("PredicateVerifier: expression width must be known for " +
                           exprKindName(e->kind) + " " + exprName(e)));
    }

    switch (e->kind) {
    case ExprKind::BinaryOp:
        if (auto r = verify_child(e->left); !r.ok) return finish(r);
        if (auto r = verify_child(e->right); !r.ok) return finish(r);
        if ((e->op == "/" || e->op == "%")) {
            if (!e->left || !e->right ||
                !isBuiltinDivRemType(e->left->type) ||
                !isBuiltinDivRemType(e->right->type) ||
                e->left->type.is_signed != e->right->type.is_signed ||
                e->left->type.width != e->right->type.width ||
                e->type.width != e->left->type.width) {
                return finish(fail("PredicateVerifier: only canonical builtin division/modulo may appear in Predicate IR"));
            }
        }
        return finish();
    case ExprKind::UnaryOp:
        return finish(verify_child(e->operand));
    case ExprKind::ArrayAccess:
        return finish(fail("PredicateVerifier: unlowered ArrayAccess in Predicate IR"));
    case ExprKind::FieldAccess:
        return finish(fail("PredicateVerifier: unlowered FieldAccess in Predicate IR"));
    case ExprKind::Call:
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->callee == "__dynamic_range_at") {
            if (e->type.width <= 0) return finish(fail("PredicateVerifier: dynamic range_at width must be known"));
            for (auto& arg : e->args) {
                if (auto r = verify_child(arg); !r.ok) return finish(r);
            }
            if (e->args.size() < 2 || !e->args[0] || !e->args[1] ||
                e->args[0]->type.width <= 0 || e->args[1]->type.width <= 0) {
                return finish(fail("PredicateVerifier: dynamic range_at operands must have known widths"));
            }
            return finish();
        }
        if (e->intrinsic == IntrinsicKind::DynamicBitAt || e->callee == "__dynamic_bit_at") {
            if (e->type.width != 1) return finish(fail("PredicateVerifier: dynamic bit_at width must be 1"));
            for (auto& arg : e->args) {
                if (auto r = verify_child(arg); !r.ok) return finish(r);
            }
            if (e->args.size() < 2 || !e->args[0] || !e->args[1] ||
                e->args[0]->type.width <= 0 || e->args[1]->type.width <= 0) {
                return finish(fail("PredicateVerifier: dynamic bit_at operands must have known widths"));
            }
            return finish();
        }
        if (e->callee != "lookup") {
            return finish(fail("PredicateVerifier: unlowered call '" + e->callee + "' in Predicate IR"));
        }
        for (auto& arg : e->args) {
            if (auto r = verify_child(arg); !r.ok) return finish(r);
        }
        if (e->args.size() < 2) {
            return finish(fail("PredicateVerifier: lookup requires table and index operands"));
        }
        if (!e->args.front() || e->args.front()->kind != ExprKind::Literal) {
            return finish(fail("PredicateVerifier: lookup table operand must be a literal table id"));
        }
        if (state.program &&
            state.program->lookup_tables.find(e->args.front()->literal_value) ==
                state.program->lookup_tables.end()) {
            return finish(fail("PredicateVerifier: lookup table '" +
                               e->args.front()->literal_value + "' is not serialized"));
        }
        if (!e->args[1] || e->args[1]->type.width <= 0) {
            return finish(fail("PredicateVerifier: lookup index width must be known"));
        }
        if (e->type.width <= 0) return finish(fail("PredicateVerifier: lookup width must be known"));
        return finish();
    case ExprKind::Cast:
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        return finish(verify_child(e->cast_expr));
    case ExprKind::Ternary:
        if (auto r = verify_child(e->cond); !r.ok) return finish(r);
        if (auto r = verify_child(e->then_expr); !r.ok) return finish(r);
        return finish(verify_child(e->else_expr));
    case ExprKind::Slice:
        if (!e->base) return finish(fail("PredicateVerifier: slice without base"));
        if (e->base->type.width <= 0) return finish(fail("PredicateVerifier: slice base width must be known"));
        if (e->lo < 0 || e->hi < e->lo) {
            return finish(fail("PredicateVerifier: invalid slice range on " + exprName(e->base)));
        }
        if (e->base->type.width > 0 && e->hi >= e->base->type.width) {
            return finish(fail("PredicateVerifier: slice out of bounds on " + exprName(e->base)));
        }
        if (e->type.width > 0 && e->type.width != e->hi - e->lo + 1) {
            return finish(fail("PredicateVerifier: slice width mismatch on " + exprName(e->base)));
        }
        return finish(verify_child(e->base));
    case ExprKind::BitSelect:
        if (!e->base) return finish(fail("PredicateVerifier: bit_select without base"));
        if (e->base->type.width <= 0) return finish(fail("PredicateVerifier: bit_select base width must be known"));
        if (e->bit < 0) return finish(fail("PredicateVerifier: invalid bit_select index on " + exprName(e->base)));
        if (e->base->type.width > 0 && e->bit >= e->base->type.width) {
            return finish(fail("PredicateVerifier: bit_select out of bounds on " + exprName(e->base)));
        }
        if (e->type.width > 0 && e->type.width != 1) {
            return finish(fail("PredicateVerifier: bit_select result width must be 1"));
        }
        return finish(verify_child(e->base));
    case ExprKind::WriteSlice: {
        if (!e->base) return finish(fail("PredicateVerifier: write_slice without base"));
        if (e->base->type.width <= 0) return finish(fail("PredicateVerifier: write_slice base width must be known"));
        if (e->lo < 0 || e->hi < e->lo) {
            return finish(fail("PredicateVerifier: invalid write_slice range on " + exprName(e->base)));
        }
        if (e->base->type.width > 0 && e->hi >= e->base->type.width) {
            return finish(fail("PredicateVerifier: write_slice out of bounds on " + exprName(e->base)));
        }
        if (auto r = verify_child(e->base); !r.ok) return finish(r);
        int slice_width = e->hi - e->lo + 1;
        if (e->value && e->value->type.width > 0 && e->value->type.width > slice_width) {
            return finish(fail("PredicateVerifier: write_slice value width exceeds slice width"));
        }
        return finish(verify_child(e->value));
    }
    case ExprKind::WriteBit:
        if (!e->base) return finish(fail("PredicateVerifier: write_bit without base"));
        if (e->base->type.width <= 0) return finish(fail("PredicateVerifier: write_bit base width must be known"));
        if (e->bit < 0) return finish(fail("PredicateVerifier: invalid write_bit index on " + exprName(e->base)));
        if (e->base->type.width > 0 && e->bit >= e->base->type.width) {
            return finish(fail("PredicateVerifier: write_bit out of bounds on " + exprName(e->base)));
        }
        if (auto r = verify_child(e->base); !r.ok) return finish(r);
        if (e->value && e->value->type.width > 1) {
            return finish(fail("PredicateVerifier: write_bit value width must be <= 1"));
        }
        return finish(verify_child(e->value));
    case ExprKind::Concat: {
        int width = 0;
        for (auto& part : e->parts) {
            if (auto r = verify_child(part); !r.ok) return finish(r);
            if (!part || part->type.width <= 0) {
                return finish(fail("PredicateVerifier: concat part width must be known"));
            }
            width += part->type.width;
        }
        if (e->type.width <= 0) {
            return finish(fail("PredicateVerifier: concat width must be known"));
        }
        if (e->type.width != width) {
            return finish(fail("PredicateVerifier: concat width mismatch"));
        }
        return finish();
    }
    case ExprKind::Repeat:
        if (e->times <= 0) return finish(fail("PredicateVerifier: repeat count must be positive"));
        if (auto r = verify_child(e->operand); !r.ok) return finish(r);
        if (!e->operand || e->operand->type.width <= 0 || e->type.width <= 0) {
            return finish(fail("PredicateVerifier: repeat widths must be known"));
        }
        if (e->type.width != e->operand->type.width * e->times) {
            return finish(fail("PredicateVerifier: repeat width mismatch"));
        }
        return finish();
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        if (e->type.width != 1) {
            return finish(fail("PredicateVerifier: reduce result width must be 1"));
        }
        if (!e->operand || e->operand->type.width <= 0) {
            return finish(fail("PredicateVerifier: reduce operand width must be known"));
        }
        return finish(verify_child(e->operand));
    default:
        return finish();
    }
}

} // namespace

PredicateVerifyResult verifyPredicateProgram(const PredicateProgram& program) {
    for (const auto& diagnostic : program.diagnostics) {
        if (diagnostic.rfind("missing_assignment_for_non_defaultable_output:", 0) == 0) {
            return fail("PredicateVerifier: " + diagnostic);
        }
    }

    VerifyState state;
    state.program = &program;
    DefinitionMap definitions;
    for (const auto& assign : program.assignments) {
        std::string name = targetName(assign.target);
        if (!name.empty()) definitions[name] = assign.value;
    }

    for (const auto& output_name : program.outputs) {
        auto dir_it = program.param_directions.find(output_name);
        if (dir_it != program.param_directions.end() && dir_it->second == "Input") {
            return fail("PredicateVerifier: output parameter '" + output_name +
                        "' is marked Input in param_directions");
        }
    }

    for (const auto& assign : program.assignments) {
        if (auto r = verifyExpr(assign.guard, state); !r.ok) return r;
        if (auto r = verifyExpr(assign.target, state); !r.ok) return r;
        if (auto r = verifyExpr(assign.value, state); !r.ok) return r;
    }
    if (program.outputs.empty() && program.output_expressions.empty()) {
        return fail("PredicateVerifier: missing OutputExpressionMap");
    }
    for (const auto& out : program.output_expressions) {
        if (!vectorContains(program.outputs, out.name)) {
            return fail("PredicateVerifier: output expression '" + out.name +
                        "' is not listed in outputs");
        }
        auto dir_it = program.param_directions.find(out.name);
        if (dir_it != program.param_directions.end() && dir_it->second == "Input") {
            return fail("PredicateVerifier: output expression '" + out.name +
                        "' is marked Input in param_directions");
        }
        if (out.name.empty()) return fail("PredicateVerifier: unnamed output expression");
        if (!out.expr) return fail("PredicateVerifier: missing output expression for '" + out.name + "'");
        if (out.type.width <= 0 && out.expr->type.width <= 0) {
            return fail("PredicateVerifier: unknown output width for '" + out.name + "'");
        }
        if (!out.paired_control.empty()) {
            const bool known_control =
                vectorContains(program.outputs, out.paired_control) ||
                vectorContains(program.inputs, out.paired_control) ||
                program.param_directions.find(out.paired_control) != program.param_directions.end();
            if (!known_control) {
                return fail("PredicateVerifier: paired control '" + out.paired_control +
                            "' for output '" + out.name + "' is not a known port");
            }
            auto control_type = program.symbols.find(out.paired_control);
            if (control_type != program.symbols.end() &&
                control_type->second.width > 0 && control_type->second.width != 1) {
                return fail("PredicateVerifier: paired control '" + out.paired_control +
                            "' for output '" + out.name + "' must be 1-bit/bool");
            }
            if (out.inactive_semantics != "disabled_data") {
                return fail("PredicateVerifier: paired output '" + out.name +
                            "' must declare inactive_semantics=disabled_data");
            }
            if (out.inactive_value.empty()) {
                return fail("PredicateVerifier: paired output '" + out.name +
                            "' must declare inactive_value");
            }
        }
        if ((out.default_policy == "wdata_default_zero_when_wen_false" ||
             out.default_policy == "payload_default_zero_when_valid_false") &&
            out.paired_control.empty()) {
            return fail("PredicateVerifier: disabled data output '" + out.name +
                        "' is missing paired_control");
        }
        if (auto r = verifyExpr(out.expr, state); !r.ok) return r;
        std::unordered_set<std::string> resolving;
        if (auto r = verifyClosedExpr(out.expr, program, definitions, resolving); !r.ok) {
            return r;
        }
        if (out.has_default && exprContainsVar(out.expr, out.name + "_0")) {
            return fail("PredicateVerifier: defaulted output '" + out.name +
                        "' still depends on its incoming SSA value");
        }
    }
    for (const auto& output_name : program.outputs) {
        bool found = false;
        for (const auto& out : program.output_expressions) {
            if (out.name == output_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            return fail("PredicateVerifier: missing output expression for output '" +
                        output_name + "'");
        }
    }
    return {};
}

} // namespace pred
