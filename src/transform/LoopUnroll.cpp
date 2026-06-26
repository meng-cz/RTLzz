#include "transform/LoopUnroll.h"
#include <optional>
#include <cstdlib>
#include <string>

namespace pred {

static TypeInfo boolType() {
    return TypeInfo{"bool", 1, false};
}

static StmtPtr makeAssignStmt(const std::string& name, ExprPtr value, TypeInfo type) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Assign;
    s->assign_target = make_var(name, type);
    s->assign_value = std::move(value);
    return s;
}

static StmtPtr makeDeclStmt(const std::string& name, TypeInfo type, ExprPtr init = nullptr) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Decl;
    s->decl_name = name;
    s->decl_type = type;
    if (init) s->decl_init = std::move(init);
    return s;
}

static StmtPtr wrapIf(ExprPtr cond, StmtPtr stmt) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::If;
    s->if_cond = std::move(cond);
    s->if_then.push_back(std::move(stmt));
    return s;
}

// Try to evaluate a literal expression to an integer constant.
static std::optional<int64_t> evalConstInt(const ExprPtr& e) {
    if (!e) return std::nullopt;
    if (e->kind == ExprKind::Literal) {
        try {
            return std::stoll(e->literal_value, nullptr, 0);
        } catch (...) {
            return std::nullopt;
        }
    }
    if (e->kind == ExprKind::Cast || e->kind == ExprKind::ZExt ||
        e->kind == ExprKind::SExt || e->kind == ExprKind::Trunc) {
        return evalConstInt(e->cast_expr);
    }
    if (e->kind == ExprKind::UnaryOp && e->op == "-") {
        auto value = evalConstInt(e->operand);
        if (value) return -*value;
    }
    return std::nullopt;
}

// Try to extract loop parameters from a for-loop:
//   for (var = start; var < end; var++ / var += step)
// Returns (var_name, start, end, step) if static.
struct LoopParams {
    std::string var;
    int64_t start;
    int64_t end;
    int64_t step;
    std::string cmp_op; // "<", "<=", ">", ">="
};

static std::optional<LoopParams> extractForParams(const StmtPtr& s) {
    if (!s || s->kind != StmtKind::For) return std::nullopt;

    LoopParams params;

    // Extract init: var = start
    if (!s->for_init) return std::nullopt;
    if (s->for_init->kind == StmtKind::Decl) {
        params.var = s->for_init->decl_name;
        if (!s->for_init->decl_init.has_value()) return std::nullopt;
        auto start = evalConstInt(s->for_init->decl_init.value());
        if (!start) return std::nullopt;
        params.start = *start;
    } else if (s->for_init->kind == StmtKind::Assign) {
        if (!s->for_init->assign_target || s->for_init->assign_target->kind != ExprKind::VarRef)
            return std::nullopt;
        params.var = s->for_init->assign_target->var_name;
        auto start = evalConstInt(s->for_init->assign_value);
        if (!start) return std::nullopt;
        params.start = *start;
    } else {
        return std::nullopt;
    }

    // Extract condition: var < end / var <= end / var > end / var >= end
    if (!s->for_cond || s->for_cond->kind != ExprKind::BinaryOp)
        return std::nullopt;
    auto& cond = s->for_cond;
    if (!cond->left || cond->left->kind != ExprKind::VarRef)
        return std::nullopt;
    if (cond->left->var_name != params.var)
        return std::nullopt;

    params.cmp_op = cond->op;
    auto end_val = evalConstInt(cond->right);
    if (!end_val) return std::nullopt;
    params.end = *end_val;

    // Extract step: var++ / var += step / var = var + step
    if (!s->for_step) return std::nullopt;
    auto& step = s->for_step;

    if (step->kind == ExprKind::UnaryOp) {
        // var++ or ++var
        if (step->op == "++" || step->op == "--") {
            params.step = (step->op == "++") ? 1 : -1;
        } else {
            return std::nullopt;
        }
    } else if (step->kind == ExprKind::BinaryOp) {
        // var += N or var = var + N
        if (step->op == "+=" || step->op == "-=") {
            auto val = evalConstInt(step->right);
            if (!val) return std::nullopt;
            params.step = (step->op == "+=") ? *val : -(*val);
        } else if (step->op == "=") {
            // var = var + N
            if (step->right && step->right->kind == ExprKind::BinaryOp) {
                auto val = evalConstInt(step->right->right);
                if (!val) return std::nullopt;
                params.step = (step->right->op == "+") ? *val : -(*val);
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    return params;
}

static int64_t computeIterations(const LoopParams& p) {
    if (p.step == 0) return -1;
    int64_t count = 0;
    if (p.step > 0) {
        if (p.cmp_op == "<") count = (p.end - p.start + p.step - 1) / p.step;
        else if (p.cmp_op == "<=") count = (p.end - p.start + p.step) / p.step;
        else return -1;
    } else {
        if (p.cmp_op == ">") count = (p.start - p.end + (-p.step) - 1) / (-p.step);
        else if (p.cmp_op == ">=") count = (p.start - p.end + (-p.step)) / (-p.step);
        else return -1;
    }
    return (count > 0) ? count : 0;
}

static std::optional<int64_t> initialValueFor(const std::vector<StmtPtr>& emitted,
                                              const std::string& var) {
    for (auto it = emitted.rbegin(); it != emitted.rend(); ++it) {
        auto& s = *it;
        if (!s) continue;
        if (s->kind == StmtKind::Decl && s->decl_name == var && s->decl_init.has_value()) {
            return evalConstInt(s->decl_init.value());
        }
        if (s->kind == StmtKind::Assign && s->assign_target &&
            s->assign_target->kind == ExprKind::VarRef &&
            s->assign_target->var_name == var) {
            return evalConstInt(s->assign_value);
        }
    }
    return std::nullopt;
}

static std::optional<int64_t> stepFromStmt(const StmtPtr& s, const std::string& var) {
    if (!s) return std::nullopt;
    ExprPtr e;
    if (s->kind == StmtKind::ExprStmt) e = s->expr_stmt;
    else if (s->kind == StmtKind::Assign) {
        if (!s->assign_target || s->assign_target->kind != ExprKind::VarRef ||
            s->assign_target->var_name != var) return std::nullopt;
        e = make_binary("=", s->assign_target, s->assign_value, s->assign_target->type);
    } else {
        return std::nullopt;
    }

    if (!e) return std::nullopt;
    if (e->kind == ExprKind::UnaryOp && e->operand &&
        e->operand->kind == ExprKind::VarRef && e->operand->var_name == var) {
        if (e->op == "++") return 1;
        if (e->op == "--") return -1;
    }
    if (e->kind == ExprKind::BinaryOp) {
        if ((e->op == "+=" || e->op == "-=") && e->left &&
            e->left->kind == ExprKind::VarRef && e->left->var_name == var) {
            auto val = evalConstInt(e->right);
            if (!val) return std::nullopt;
            return e->op == "+=" ? *val : -(*val);
        }
        if (e->op == "=" && e->right && e->right->kind == ExprKind::BinaryOp &&
            e->right->left && e->right->left->kind == ExprKind::VarRef &&
            e->right->left->var_name == var) {
            auto val = evalConstInt(e->right->right);
            if (!val) return std::nullopt;
            if (e->right->op == "+") return *val;
            if (e->right->op == "-") return -(*val);
        }
    }
    return std::nullopt;
}

static std::optional<LoopParams> extractWhileParams(const StmtPtr& s,
                                                    const std::vector<StmtPtr>& emitted) {
    if (!s || (s->kind != StmtKind::While && s->kind != StmtKind::DoWhile)) return std::nullopt;
    if (!s->while_cond || s->while_cond->kind != ExprKind::BinaryOp ||
        !s->while_cond->left || s->while_cond->left->kind != ExprKind::VarRef) {
        return std::nullopt;
    }
    if (s->while_body.empty()) return std::nullopt;
    LoopParams params;
    params.var = s->while_cond->left->var_name;
    params.cmp_op = s->while_cond->op;
    auto end = evalConstInt(s->while_cond->right);
    auto start = initialValueFor(emitted, params.var);
    auto step = stepFromStmt(s->while_body.back(), params.var);
    if (!start || !end || !step) return std::nullopt;
    params.start = *start;
    params.end = *end;
    params.step = *step;
    return params;
}

// Substitute all occurrences of var_name with a literal value in an expression.
static ExprPtr substituteVar(const ExprPtr& e, const std::string& var, int64_t value) {
    if (!e) return nullptr;

    if (e->kind == ExprKind::VarRef && e->var_name == var) {
        return make_literal(std::to_string(value), e->type);
    }

    // Deep copy with substitution
    auto result = std::make_shared<Expr>(*e);
    if (result->left) result->left = substituteVar(result->left, var, value);
    if (result->right) result->right = substituteVar(result->right, var, value);
    if (result->operand) result->operand = substituteVar(result->operand, var, value);
    if (result->array_base) result->array_base = substituteVar(result->array_base, var, value);
    if (result->index) result->index = substituteVar(result->index, var, value);
    if (result->struct_base) result->struct_base = substituteVar(result->struct_base, var, value);
    if (result->cast_expr) result->cast_expr = substituteVar(result->cast_expr, var, value);
    if (result->cond) result->cond = substituteVar(result->cond, var, value);
    if (result->then_expr) result->then_expr = substituteVar(result->then_expr, var, value);
    if (result->else_expr) result->else_expr = substituteVar(result->else_expr, var, value);
    if (result->base) result->base = substituteVar(result->base, var, value);
    if (result->value) result->value = substituteVar(result->value, var, value);
    for (auto& arg : result->args) {
        arg = substituteVar(arg, var, value);
    }
    for (auto& part : result->parts) {
        part = substituteVar(part, var, value);
    }
    return result;
}

// Substitute in statements (deep copy with var replacement)
static StmtPtr substituteStmt(const StmtPtr& s, const std::string& var, int64_t value);

static std::vector<StmtPtr> substituteStmts(const std::vector<StmtPtr>& stmts,
                                             const std::string& var, int64_t value) {
    std::vector<StmtPtr> result;
    for (auto& s : stmts) {
        auto sub = substituteStmt(s, var, value);
        if (sub) result.push_back(sub);
    }
    return result;
}

static StmtPtr substituteStmt(const StmtPtr& s, const std::string& var, int64_t value) {
    if (!s) return nullptr;
    auto result = std::make_shared<Stmt>(*s);

    switch (s->kind) {
    case StmtKind::Assign:
        result->assign_target = substituteVar(s->assign_target, var, value);
        result->assign_value = substituteVar(s->assign_value, var, value);
        break;
    case StmtKind::Decl:
        if (s->decl_init.has_value())
            result->decl_init = substituteVar(s->decl_init.value(), var, value);
        break;
    case StmtKind::If:
        result->if_cond = substituteVar(s->if_cond, var, value);
        result->if_then = substituteStmts(s->if_then, var, value);
        result->if_else = substituteStmts(s->if_else, var, value);
        break;
    case StmtKind::For:
        result->for_cond = substituteVar(s->for_cond, var, value);
        result->for_step = substituteVar(s->for_step, var, value);
        result->for_body = substituteStmts(s->for_body, var, value);
        break;
    case StmtKind::While:
    case StmtKind::DoWhile:
        result->while_cond = substituteVar(s->while_cond, var, value);
        result->while_body = substituteStmts(s->while_body, var, value);
        break;
    case StmtKind::Switch:
        result->switch_expr = substituteVar(s->switch_expr, var, value);
        for (auto& c : result->switch_cases) {
            if (c.value.has_value()) {
                c.value = substituteVar(c.value.value(), var, value);
            }
            c.body = substituteStmts(c.body, var, value);
        }
        break;
    case StmtKind::Block:
        result->block_stmts = substituteStmts(s->block_stmts, var, value);
        break;
    case StmtKind::Return:
        if (s->return_value.has_value())
            result->return_value = substituteVar(s->return_value.value(), var, value);
        break;
    case StmtKind::ExprStmt:
        result->expr_stmt = substituteVar(s->expr_stmt, var, value);
        break;
    default:
        break;
    }
    return result;
}

static std::vector<StmtPtr> lowerLoopControl(const std::vector<StmtPtr>& stmts,
                                             const std::string& active_var,
                                             const std::string& continue_var,
                                             bool inside_switch = false);

static bool containsLoopControl(const std::vector<StmtPtr>& stmts, bool inside_switch = false) {
    for (auto& s : stmts) {
        if (!s) continue;
        if (s->kind == StmtKind::Break) {
            if (!inside_switch) return true;
            continue;
        }
        if (s->kind == StmtKind::Continue) return true;
        if (s->kind == StmtKind::If &&
            (containsLoopControl(s->if_then, inside_switch) ||
             containsLoopControl(s->if_else, inside_switch))) return true;
        if (s->kind == StmtKind::Block && containsLoopControl(s->block_stmts, inside_switch)) return true;
        if (s->kind == StmtKind::Switch) {
            for (auto& c : s->switch_cases) {
                if (containsLoopControl(c.body, true)) return true;
            }
        }
    }
    return false;
}

static StmtPtr lowerLoopControlStmt(const StmtPtr& s,
                                    const std::string& active_var,
                                    const std::string& continue_var,
                                    bool inside_switch = false) {
    if (!s) return nullptr;
    if (s->kind == StmtKind::Break) {
        if (inside_switch) return s;
        return makeAssignStmt(active_var, make_literal("false", boolType()), boolType());
    }
    if (s->kind == StmtKind::Continue) {
        return makeAssignStmt(continue_var, make_literal("true", boolType()), boolType());
    }
    auto r = std::make_shared<Stmt>(*s);
    if (s->kind == StmtKind::If) {
        r->if_then = lowerLoopControl(s->if_then, active_var, continue_var, inside_switch);
        r->if_else = lowerLoopControl(s->if_else, active_var, continue_var, inside_switch);
    } else if (s->kind == StmtKind::Block) {
        r->block_stmts = lowerLoopControl(s->block_stmts, active_var, continue_var, inside_switch);
    } else if (s->kind == StmtKind::Switch) {
        for (auto& c : r->switch_cases) {
            c.body = lowerLoopControl(c.body, active_var, continue_var, true);
        }
    }
    return r;
}

static std::vector<StmtPtr> lowerLoopControl(const std::vector<StmtPtr>& stmts,
                                             const std::string& active_var,
                                             const std::string& continue_var,
                                             bool inside_switch) {
    std::vector<StmtPtr> out;
    for (auto& s : stmts) {
        auto lowered = lowerLoopControlStmt(s, active_var, continue_var, inside_switch);
        if (!lowered) continue;
        auto guard = make_binary("&&",
            make_var(active_var, boolType()),
            make_unary("!", make_var(continue_var, boolType()), boolType()),
            boolType());
        out.push_back(wrapIf(guard, lowered));
    }
    return out;
}

// Recursively process statements, unrolling loops
static std::vector<StmtPtr> processStmts(const std::vector<StmtPtr>& stmts,
                                          const UnrollConfig& config,
                                          std::string& error,
                                          int& loop_id) {
    std::vector<StmtPtr> result;

    for (auto& s : stmts) {
        if (!s) continue;
        if (!error.empty()) break;

        if (s->kind == StmtKind::For) {
            auto params = extractForParams(s);
            if (!params) {
                error = "Cannot statically analyze for-loop (non-constant bounds or step)";
                break;
            }
            int64_t iters = computeIterations(*params);
            if (iters < 0) {
                error = "Cannot determine iteration count for loop variable '" + params->var + "'";
                break;
            }
            if (iters > config.max_iterations) {
                error = "Loop iteration count " + std::to_string(iters) +
                        " exceeds maximum " + std::to_string(config.max_iterations);
                break;
            }

            bool needs_control_predicates = containsLoopControl(s->for_body);
            std::string active_var = "__loop_active_" + std::to_string(loop_id++);
            if (needs_control_predicates) {
                result.push_back(makeDeclStmt(active_var, boolType(), make_literal("true", boolType())));
            }

            // Unroll: for each iteration, substitute loop var and emit body.
            // break/continue become explicit predicate state so the CFG remains
            // acyclic after unrolling.
            int64_t val = params->start;
            for (int64_t i = 0; i < iters; ++i) {
                std::string continue_var = active_var + "_continue_" + std::to_string(i);
                if (needs_control_predicates) {
                    result.push_back(makeDeclStmt(continue_var, boolType(), make_literal("false", boolType())));
                }
                auto unrolled_body = substituteStmts(s->for_body, params->var, val);
                if (needs_control_predicates) {
                    unrolled_body = lowerLoopControl(unrolled_body, active_var, continue_var);
                }
                // Recursively process (handles nested loops)
                auto processed = processStmts(unrolled_body, config, error, loop_id);
                if (!error.empty()) break;
                for (auto& stmt : processed) {
                    result.push_back(std::move(stmt));
                }
                val += params->step;
            }
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::DoWhile) {
            auto params = extractWhileParams(s, result);
            if (!params) {
                error = "Cannot statically analyze while/do-while loop (non-constant bounds or step)";
                break;
            }
            int64_t iters = computeIterations(*params);
            if (s->kind == StmtKind::DoWhile && iters == 0) iters = 1;
            if (iters < 0) {
                error = "Cannot determine iteration count for loop variable '" + params->var + "'";
                break;
            }
            if (iters > config.max_iterations) {
                error = "Loop iteration count " + std::to_string(iters) +
                        " exceeds maximum " + std::to_string(config.max_iterations);
                break;
            }

            std::vector<StmtPtr> body = s->while_body;
            body.pop_back();
            if (containsLoopControl(body)) {
                error = "Cannot statically analyze while/do-while loop: break/continue before canonical final step is unsupported";
                break;
            }
            bool needs_control_predicates = containsLoopControl(body);
            std::string active_var = "__loop_active_" + std::to_string(loop_id++);
            if (needs_control_predicates) {
                result.push_back(makeDeclStmt(active_var, boolType(), make_literal("true", boolType())));
            }
            int64_t val = params->start;
            for (int64_t i = 0; i < iters; ++i) {
                std::string continue_var = active_var + "_continue_" + std::to_string(i);
                if (needs_control_predicates) {
                    result.push_back(makeDeclStmt(continue_var, boolType(), make_literal("false", boolType())));
                }
                auto unrolled_body = substituteStmts(body, params->var, val);
                if (needs_control_predicates) {
                    unrolled_body = lowerLoopControl(unrolled_body, active_var, continue_var);
                }
                auto processed = processStmts(unrolled_body, config, error, loop_id);
                if (!error.empty()) break;
                for (auto& stmt : processed) result.push_back(std::move(stmt));
                val += params->step;
            }
            result.push_back(makeAssignStmt(params->var, make_literal(std::to_string(val), TypeInfo{"int", 32, true}), TypeInfo{"int", 32, true}));
        } else if (s->kind == StmtKind::If) {
            auto processed = std::make_shared<Stmt>(*s);
            processed->if_then = processStmts(s->if_then, config, error, loop_id);
            if (!error.empty()) break;
            processed->if_else = processStmts(s->if_else, config, error, loop_id);
            if (!error.empty()) break;
            result.push_back(processed);
        } else if (s->kind == StmtKind::Block) {
            auto processed = std::make_shared<Stmt>(*s);
            processed->block_stmts = processStmts(s->block_stmts, config, error, loop_id);
            if (!error.empty()) break;
            result.push_back(processed);
        } else if (s->kind == StmtKind::Break || s->kind == StmtKind::Continue) {
            error = "break/continue is only supported inside statically unrolled for-loops";
            break;
        } else {
            result.push_back(s);
        }
    }

    return result;
}

UnrollResult unrollLoops(const std::vector<StmtPtr>& body, const UnrollConfig& config) {
    UnrollResult result;
    int loop_id = 0;
    result.body = processStmts(body, config, result.error, loop_id);
    return result;
}

} // namespace pred


