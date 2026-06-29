#include "transform/Predicate.h"
#include <queue>
#include <set>

namespace pred {

// The predication pass works by traversing the SSA CFG and computing
// a "path condition" (guard) for each basic block. Assignments in each
// block are emitted with that block's guard. Phi nodes are converted
// to ite expressions.

struct BlockGuard {
    ExprPtr guard;
};

static bool isTrue(const ExprPtr& e) {
    return !e || (e->kind == ExprKind::Literal &&
                  (e->literal_value == "1" || e->literal_value == "true"));
}

static bool exprEqual(const ExprPtr& a, const ExprPtr& b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case ExprKind::Literal:
        return a->literal_value == b->literal_value;
    case ExprKind::VarRef:
        return a->var_name == b->var_name;
    case ExprKind::BinaryOp:
        return a->op == b->op && exprEqual(a->left, b->left) &&
               exprEqual(a->right, b->right);
    case ExprKind::UnaryOp:
        return a->op == b->op && exprEqual(a->operand, b->operand);
    case ExprKind::Cast:
        return a->cast_type.name == b->cast_type.name && exprEqual(a->cast_expr, b->cast_expr);
    case ExprKind::Ternary:
        return exprEqual(a->cond, b->cond) && exprEqual(a->then_expr, b->then_expr) &&
               exprEqual(a->else_expr, b->else_expr);
    case ExprKind::Call:
        if (a->callee != b->callee || a->args.size() != b->args.size()) return false;
        for (size_t i = 0; i < a->args.size(); ++i) {
            if (!exprEqual(a->args[i], b->args[i])) return false;
        }
        return true;
    case ExprKind::ArrayAccess:
        return exprEqual(a->array_base, b->array_base) && exprEqual(a->index, b->index);
    case ExprKind::FieldAccess:
        return a->field_name == b->field_name && exprEqual(a->struct_base, b->struct_base);
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        return a->to_width == b->to_width && exprEqual(a->cast_expr, b->cast_expr);
    case ExprKind::Slice:
        return a->hi == b->hi && a->lo == b->lo && exprEqual(a->base, b->base);
    case ExprKind::BitSelect:
        return a->bit == b->bit && exprEqual(a->base, b->base);
    case ExprKind::WriteSlice:
        return a->hi == b->hi && a->lo == b->lo && exprEqual(a->base, b->base) &&
               exprEqual(a->value, b->value);
    case ExprKind::WriteBit:
        return a->bit == b->bit && exprEqual(a->base, b->base) && exprEqual(a->value, b->value);
    case ExprKind::Concat:
        if (a->parts.size() != b->parts.size()) return false;
        for (size_t i = 0; i < a->parts.size(); ++i) {
            if (!exprEqual(a->parts[i], b->parts[i])) return false;
        }
        return true;
    case ExprKind::Repeat:
        return a->times == b->times && exprEqual(a->operand, b->operand);
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        return exprEqual(a->operand, b->operand);
    }
    return false;
}

static bool isNotOf(const ExprPtr& maybe_not, const ExprPtr& expr) {
    return maybe_not && maybe_not->kind == ExprKind::UnaryOp && maybe_not->op == "!" &&
           exprEqual(maybe_not->operand, expr);
}

static bool complementary(const ExprPtr& a, const ExprPtr& b) {
    return isNotOf(a, b) || isNotOf(b, a);
}

static bool isFalse(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal &&
           (e->literal_value == "0" || e->literal_value == "false");
}

static ExprPtr atomOf(const ExprPtr& e) {
    if (e && e->kind == ExprKind::UnaryOp && e->op == "!") return e->operand;
    return e;
}

static void addAtom(std::vector<ExprPtr>& atoms, const ExprPtr& atom) {
    if (!atom || isTrue(atom) || isFalse(atom)) return;
    for (auto& existing : atoms) {
        if (exprEqual(existing, atom)) return;
    }
    atoms.push_back(atom);
}

static void collectBoolAtoms(const ExprPtr& e, std::vector<ExprPtr>& atoms) {
    if (!e || isTrue(e) || isFalse(e)) return;
    if (e->kind == ExprKind::UnaryOp && e->op == "!") {
        collectBoolAtoms(e->operand, atoms);
        return;
    }
    if (e->kind == ExprKind::BinaryOp && (e->op == "&&" || e->op == "||")) {
        collectBoolAtoms(e->left, atoms);
        collectBoolAtoms(e->right, atoms);
        return;
    }
    addAtom(atoms, e);
}

static int atomIndex(const std::vector<ExprPtr>& atoms, const ExprPtr& e) {
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (exprEqual(atoms[i], e)) return static_cast<int>(i);
    }
    return -1;
}

static bool evalBoolExpr(const ExprPtr& e, const std::vector<ExprPtr>& atoms, unsigned mask) {
    if (isTrue(e)) return true;
    if (isFalse(e)) return false;
    if (!e) return true;
    if (e->kind == ExprKind::UnaryOp && e->op == "!") {
        return !evalBoolExpr(e->operand, atoms, mask);
    }
    if (e->kind == ExprKind::BinaryOp && e->op == "&&") {
        return evalBoolExpr(e->left, atoms, mask) && evalBoolExpr(e->right, atoms, mask);
    }
    if (e->kind == ExprKind::BinaryOp && e->op == "||") {
        return evalBoolExpr(e->left, atoms, mask) || evalBoolExpr(e->right, atoms, mask);
    }
    int idx = atomIndex(atoms, atomOf(e));
    if (idx < 0) return false;
    bool value = ((mask >> idx) & 1U) != 0;
    if (e->kind == ExprKind::UnaryOp && e->op == "!") value = !value;
    return value;
}

static bool isTautology(const ExprPtr& e) {
    std::vector<ExprPtr> atoms;
    collectBoolAtoms(e, atoms);
    if (atoms.empty()) return isTrue(e);
    if (atoms.size() > 8) return false;
    unsigned total = 1U << atoms.size();
    for (unsigned mask = 0; mask < total; ++mask) {
        if (!evalBoolExpr(e, atoms, mask)) return false;
    }
    return true;
}

static void splitAnd(const ExprPtr& e, ExprPtr& left, ExprPtr& right) {
    if (e && e->kind == ExprKind::BinaryOp && e->op == "&&") {
        left = e->left;
        right = e->right;
    } else {
        left = e;
        right = nullptr;
    }
}

static ExprPtr make_or_guard(ExprPtr a, ExprPtr b) {
    if (isTrue(a) || isTrue(b)) return make_true_guard();
    if (exprEqual(a, b)) return a;
    if (complementary(a, b)) return make_true_guard();

    ExprPtr a_l, a_r, b_l, b_r;
    splitAnd(a, a_l, a_r);
    splitAnd(b, b_l, b_r);
    if (a_l && a_r && b_l && b_r) {
        if (exprEqual(a_l, b_l) && complementary(a_r, b_r)) return a_l;
        if (exprEqual(a_l, b_r) && complementary(a_r, b_l)) return a_l;
        if (exprEqual(a_r, b_l) && complementary(a_l, b_r)) return a_r;
        if (exprEqual(a_r, b_r) && complementary(a_l, b_l)) return a_r;
    }

    auto candidate = make_binary("||", a, b, TypeInfo{"bool", 1, false});
    if (isTautology(candidate)) return make_true_guard();
    return candidate;
}

static std::vector<BlockId> successorsOf(const SSABlock& block) {
    if (block.term.kind == TermKind::Jump) return {block.term.jump_target};
    if (block.term.kind == TermKind::Branch) {
        return {block.term.true_target, block.term.false_target};
    }
    return {};
}

// Compute the guard (path condition) for each block by propagating
// conditions in topological order. After static loop unrolling the CFG is
// acyclic; using BFS can process a merge before all predecessors arrive.
static std::vector<ExprPtr> computeBlockGuards(const SSAProgram& ssa) {
    int n = static_cast<int>(ssa.blocks.size());
    std::vector<ExprPtr> guards(n, nullptr);
    guards[ssa.entry] = make_true_guard();

    std::vector<int> indegree(n, 0);
    for (auto& block : ssa.blocks) {
        for (BlockId succ : successorsOf(block)) {
            if (succ >= 0 && succ < n) ++indegree[succ];
        }
    }

    std::queue<BlockId> ready;
    for (int i = 0; i < n; ++i) {
        if (indegree[i] == 0) ready.push(i);
    }

    std::vector<BlockId> order;
    while (!ready.empty()) {
        BlockId id = ready.front();
        ready.pop();
        order.push_back(id);
        for (BlockId succ : successorsOf(ssa.blocks[id])) {
            if (succ >= 0 && succ < n && --indegree[succ] == 0) ready.push(succ);
        }
    }

    if (order.size() != ssa.blocks.size()) {
        order.clear();
        for (auto& block : ssa.blocks) order.push_back(block.id);
    }

    for (BlockId id : order) {
        auto& block = ssa.blocks[id];
        ExprPtr my_guard = guards[id];
        if (!my_guard) continue;

        auto add_guard = [&](BlockId target, ExprPtr edge_guard) {
            if (target < 0 || target >= n) return;
            guards[target] = guards[target] ? make_or_guard(guards[target], edge_guard) : edge_guard;
        };

        if (block.term.kind == TermKind::Jump) {
            add_guard(block.term.jump_target, my_guard);
        } else if (block.term.kind == TermKind::Branch) {
            ExprPtr cond = block.term.condition;
            add_guard(block.term.true_target, make_and(my_guard, cond));
            add_guard(block.term.false_target, make_and(my_guard, make_not(cond)));
        }
    }

    return guards;
}

// Convert a phi node to an ite expression.
// phi(x_1 from B1, x_2 from B2) with B1 guard g1, B2 guard g2
// => ite(g1, x_1, x_2)  (simplified for 2 operands)
// For N operands: nested ite chain.
static ExprPtr phiToIte(const PhiNode& phi,
                        const std::vector<ExprPtr>& block_guards) {
    if (phi.operands.empty()) return nullptr;
    if (phi.operands.size() == 1) {
        return make_var(phi.operands[0].second.str(), phi.type);
    }

    // Build nested ite from last to first
    ExprPtr result = make_var(phi.operands.back().second.str(), phi.type);

    for (int i = static_cast<int>(phi.operands.size()) - 2; i >= 0; --i) {
        auto& [pred_block, ssa_var] = phi.operands[i];
        ExprPtr guard = block_guards[pred_block];
        ExprPtr val = make_var(ssa_var.str(), phi.type);
        result = make_ite(guard, val, result, phi.type);
    }

    return result;
}

static ExprPtr phiIncomingGuard(const PhiNode& phi,
                                const std::vector<ExprPtr>& block_guards) {
    ExprPtr guard;
    for (const auto& [pred_block, value] : phi.operands) {
        (void)value;
        if (pred_block < 0 ||
            pred_block >= static_cast<BlockId>(block_guards.size())) {
            continue;
        }
        guard = guard
            ? make_or_guard(std::move(guard), block_guards[pred_block])
            : block_guards[pred_block];
    }
    return guard;
}

PredicateProgram predicate(const SSAProgram& ssa) {
    PredicateProgram prog;
    prog.symbols = ssa.var_types;

    // Compute block guards
    auto guards = computeBlockGuards(ssa);

    // Process each block
    for (auto& block : ssa.blocks) {
        ExprPtr block_guard = guards[block.id];
        if (!block_guard) block_guard = make_true_guard();

        // Convert phi nodes to assignments with ite
        for (auto& phi : block.phis) {
            auto value = phiToIte(phi, guards);
            if (!value) continue;
            GuardedAssign ga;
            ga.guard = phiIncomingGuard(phi, guards);
            if (!ga.guard) ga.guard = block_guard;
            ga.target = make_var(phi.result.str(), phi.type);
            ga.value = std::move(value);
            ga.type = phi.type;
            prog.assignments.push_back(ga);
        }

        // Convert regular assignments
        for (auto& s : block.stmts) {
            if (!s) continue;

            if (s->kind == StmtKind::Assign) {
                GuardedAssign ga;
                ga.guard = block_guard;
                ga.target = s->assign_target;
                ga.value = s->assign_value;
                if (s->assign_target) ga.type = s->assign_target->type;
                ga.debug_loc = s->debug_loc;
                prog.assignments.push_back(ga);
            } else if (s->kind == StmtKind::Decl && s->decl_init.has_value()) {
                GuardedAssign ga;
                ga.guard = block_guard;
                ga.target = make_var(s->decl_name, s->decl_type);
                ga.value = s->decl_init.value();
                ga.type = s->decl_type;
                ga.debug_loc = s->debug_loc;
                prog.assignments.push_back(ga);
            }
        }
    }

    return prog;
}

} // namespace pred
