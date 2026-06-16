#include "emitter/TextEmitter.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred {

namespace {

struct PrintBudget {
    int depth = 0;
    int nodes = 0;
    int max_depth = 8192;
    int max_nodes = 1000000;
    std::unordered_set<const Expr*> visiting;
    std::unordered_map<const Expr*, std::string> memo;
};

std::string exprToStringImpl(const ExprPtr& e, PrintBudget& budget) {
    if (!e) return "?";
    auto memo_it = budget.memo.find(e.get());
    if (memo_it != budget.memo.end()) return memo_it->second;
    if (budget.depth > budget.max_depth || budget.nodes++ > budget.max_nodes) {
        throw std::runtime_error("TextEmitter: expression too large to serialize without elision");
    }
    if (budget.visiting.count(e.get())) {
        throw std::runtime_error("TextEmitter: expression cycle detected during serialization");
    }
    budget.visiting.insert(e.get());
    ++budget.depth;

    auto child = [&](const ExprPtr& c) {
        return exprToStringImpl(c, budget);
    };
    auto done = [&](std::string s) {
        --budget.depth;
        budget.visiting.erase(e.get());
        budget.memo[e.get()] = s;
        return s;
    };

    switch (e->kind) {
    case ExprKind::Literal:
        return done(e->literal_value);

    case ExprKind::VarRef:
        return done(e->var_name);

    case ExprKind::BinaryOp:
        return done("(" + child(e->left) + " " + e->op + " " + child(e->right) + ")");

    case ExprKind::UnaryOp:
        return done(e->op + child(e->operand));

    case ExprKind::ArrayAccess:
        return done(child(e->array_base) + "[" + child(e->index) + "]");

    case ExprKind::FieldAccess:
        return done(child(e->struct_base) + "." + e->field_name);

    case ExprKind::Call: {
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->intrinsic == IntrinsicKind::DynamicBitAt) {
            std::string s = e->intrinsic == IntrinsicKind::DynamicBitAt ? "dyn_bit(" : "dyn_slice(";
            for (size_t i = 0; i < e->args.size(); ++i) {
                if (i > 0) s += ", ";
                s += child(e->args[i]);
            }
            return done(s + ")");
        }
        std::string s = e->callee + "(";
        for (size_t i = 0; i < e->args.size(); ++i) {
            if (i > 0) s += ", ";
            s += child(e->args[i]);
        }
        s += ")";
        return done(s);
    }

    case ExprKind::Cast:
        return done("(" + e->cast_type.name + ")" + child(e->cast_expr));

    case ExprKind::Ternary:
        return done("ite(" + child(e->cond) + ", " +
                    child(e->then_expr) + ", " +
                    child(e->else_expr) + ")");

    case ExprKind::ZExt:
        return done("zext<" + std::to_string(e->to_width) + ">(" + child(e->cast_expr) + ")");
    case ExprKind::SExt:
        return done("sext<" + std::to_string(e->to_width) + ">(" + child(e->cast_expr) + ")");
    case ExprKind::Trunc:
        return done("trunc<" + std::to_string(e->to_width) + ">(" + child(e->cast_expr) + ")");
    case ExprKind::Slice:
        return done("slice(" + child(e->base) + ", " + std::to_string(e->hi) + ", " + std::to_string(e->lo) + ")");
    case ExprKind::BitSelect:
        return done("bit_select(" + child(e->base) + ", " + std::to_string(e->bit) + ")");
    case ExprKind::WriteSlice:
        return done("write_slice(" + child(e->base) + ", " + std::to_string(e->hi) + ", " +
                    std::to_string(e->lo) + ", " + child(e->value) + ")");
    case ExprKind::WriteBit:
        return done("write_bit(" + child(e->base) + ", " + std::to_string(e->bit) + ", " +
                    child(e->value) + ")");
    case ExprKind::Concat: {
        std::string s = "concat(";
        for (size_t i = 0; i < e->parts.size(); ++i) {
            if (i > 0) s += ", ";
            s += child(e->parts[i]);
        }
        return done(s + ")");
    }
    case ExprKind::Repeat:
        return done("repeat<" + std::to_string(e->times) + ">(" + child(e->operand) + ")");
    case ExprKind::ReduceOr:
        return done("reduce_or(" + child(e->operand) + ")");
    case ExprKind::ReduceAnd:
        return done("reduce_and(" + child(e->operand) + ")");
    case ExprKind::ReduceXor:
        return done("reduce_xor(" + child(e->operand) + ")");
    }

    return done("?");
}

} // namespace

std::string exprToString(const ExprPtr& e) {
    PrintBudget budget;
    return exprToStringImpl(e, budget);
}

std::string emitText(const PredicateProgram& prog) {
    std::ostringstream os;

    os << "function: " << prog.function_name << "\n";
    if (!prog.param_directions.empty()) {
        os << "param_directions:\n";
        std::vector<std::string> names;
        names.reserve(prog.param_directions.size());
        for (const auto& item : prog.param_directions) names.push_back(item.first);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            os << "  " << name << ": " << prog.param_directions.at(name) << "\n";
        }
    }
    if (!prog.output_default_reasons.empty()) {
        os << "output_default_reasons:\n";
        std::vector<std::string> names;
        names.reserve(prog.output_default_reasons.size());
        for (const auto& item : prog.output_default_reasons) names.push_back(item.first);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            os << "  " << name << ": " << prog.output_default_reasons.at(name) << "\n";
        }
    }
    if (!prog.output_paired_controls.empty()) {
        os << "output_paired_controls:\n";
        std::vector<std::string> names;
        names.reserve(prog.output_paired_controls.size());
        for (const auto& item : prog.output_paired_controls) names.push_back(item.first);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            os << "  " << name << ": " << prog.output_paired_controls.at(name) << "\n";
        }
    }
    os << "assignments: " << prog.assignments.size() << "\n";
    os << "\n";

    for (auto& ga : prog.assignments) {
        std::string guard_str = exprToString(ga.guard);
        std::string target_str = exprToString(ga.target);
        std::string value_str = exprToString(ga.value);

        os << "when (" << guard_str << "): "
           << target_str << " = " << value_str << "\n";
    }

    os << "\noutput_expressions: " << prog.output_expressions.size() << "\n";
    for (const auto& out : prog.output_expressions) {
        os << "output " << out.name << " = " << exprToString(out.expr);
        if (!out.default_policy.empty()) {
            os << " ; default-policy=" << out.default_policy
               << " ; default-applied=" << (out.default_applied ? "true" : "false")
               << " ; assignment-coverage="
               << (out.assignment_coverage.empty() ? "unknown" : out.assignment_coverage);
            if (out.default_applied) {
                os << " ; default=" << (out.default_value.empty() ? "<unknown>" : out.default_value);
            }
        } else if (!out.assignment_coverage.empty()) {
            os << " ; assignment-coverage=" << out.assignment_coverage;
        }
        if (!out.paired_control.empty()) {
            os << " ; paired-control=" << out.paired_control
               << " ; inactive-semantics="
               << (out.inactive_semantics.empty() ? "unknown" : out.inactive_semantics)
               << " ; inactive-value="
               << (out.inactive_value.empty() ? "<none>" : out.inactive_value);
        }
        os << "\n";
    }

    return os.str();
}

} // namespace pred
