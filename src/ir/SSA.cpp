#include "ir/SSA.h"
#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <stack>

namespace pred {

// --- Dominator tree computation (simple iterative algorithm) ---

struct DomInfo {
    std::vector<BlockId> idom;  // immediate dominator
    std::vector<std::set<BlockId>> dom_frontier;
    int num_blocks;
};

static DomInfo computeDominators(const CFG& cfg) {
    int n = static_cast<int>(cfg.blocks.size());
    DomInfo info;
    info.num_blocks = n;
    info.idom.assign(n, -1);
    info.dom_frontier.resize(n);

    BlockId entry = cfg.entry;
    info.idom[entry] = entry;

    // Compute reverse postorder
    std::vector<BlockId> rpo;
    std::vector<bool> visited(n, false);
    std::function<void(BlockId)> dfs = [&](BlockId b) {
        visited[b] = true;
        auto* bb = cfg.blocks[b].get();
        for (auto succ : bb->successors) {
            if (!visited[succ]) dfs(succ);
        }
        rpo.push_back(b);
    };
    dfs(entry);
    std::reverse(rpo.begin(), rpo.end());

    // Map block to rpo index
    std::vector<int> rpo_idx(n, -1);
    for (int i = 0; i < static_cast<int>(rpo.size()); ++i) {
        rpo_idx[rpo[i]] = i;
    }

    auto intersect = [&](BlockId b1, BlockId b2) -> BlockId {
        while (b1 != b2) {
            while (rpo_idx[b1] > rpo_idx[b2]) b1 = info.idom[b1];
            while (rpo_idx[b2] > rpo_idx[b1]) b2 = info.idom[b2];
        }
        return b1;
    };

    // Iterative dominator computation
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto b : rpo) {
            if (b == entry) continue;
            auto* bb = cfg.blocks[b].get();
            BlockId new_idom = -1;
            for (auto pred : bb->predecessors) {
                if (info.idom[pred] == -1) continue;
                if (new_idom == -1) {
                    new_idom = pred;
                } else {
                    new_idom = intersect(new_idom, pred);
                }
            }
            if (new_idom != -1 && info.idom[b] != new_idom) {
                info.idom[b] = new_idom;
                changed = true;
            }
        }
    }

    // Compute dominance frontiers
    for (int b = 0; b < n; ++b) {
        auto* bb = cfg.blocks[b].get();
        if (bb->predecessors.size() < 2) continue;
        for (auto pred : bb->predecessors) {
            BlockId runner = pred;
            while (runner != -1 && runner != info.idom[b]) {
                info.dom_frontier[runner].insert(b);
                runner = info.idom[runner];
                if (runner == info.idom[runner]) break; // reached entry
            }
        }
    }

    return info;
}

// --- Collect variable definitions per block ---

struct VarDefs {
    // var_name -> set of blocks that define it
    std::unordered_map<std::string, std::set<BlockId>> def_blocks;
    // var_name -> type
    std::unordered_map<std::string, TypeInfo> types;
};

static void collectDefsFromExpr(const ExprPtr& e, const std::string& context_var,
                                 VarDefs& defs, BlockId block) {
    // Assignment targets are handled at stmt level
}

static void collectDefs(const CFG& cfg, VarDefs& defs) {
    for (auto& bb : cfg.blocks) {
        for (auto& s : bb->stmts) {
            if (!s) continue;
            if (s->kind == StmtKind::Decl) {
                if (s->decl_init.has_value()) {
                    defs.def_blocks[s->decl_name].insert(bb->id);
                    defs.types[s->decl_name] = s->decl_type;
                }
            } else if (s->kind == StmtKind::Assign) {
                if (s->assign_target && s->assign_target->kind == ExprKind::VarRef) {
                    defs.def_blocks[s->assign_target->var_name].insert(bb->id);
                    if (defs.types.find(s->assign_target->var_name) == defs.types.end()) {
                        defs.types[s->assign_target->var_name] = s->assign_target->type;
                    }
                }
            }
        }
    }
}

// --- Place phi nodes ---

static std::unordered_map<BlockId, std::set<std::string>>
placePhi(const VarDefs& defs, const DomInfo& dom) {
    std::unordered_map<BlockId, std::set<std::string>> phi_placement;

    for (auto& [var, def_set] : defs.def_blocks) {
        std::queue<BlockId> worklist;
        std::set<BlockId> processed;
        for (auto b : def_set) worklist.push(b);

        while (!worklist.empty()) {
            BlockId b = worklist.front();
            worklist.pop();
            for (auto df : dom.dom_frontier[b]) {
                if (phi_placement[df].insert(var).second) {
                    if (processed.find(df) == processed.end()) {
                        processed.insert(df);
                        worklist.push(df);
                    }
                }
            }
        }
    }

    return phi_placement;
}

// --- Rename variables (SSA numbering) ---

struct RenameState {
    std::unordered_map<std::string, std::stack<int>> var_stack;
    std::unordered_map<std::string, int> counter;
    std::set<std::string> allowed_symbols;
    std::string error;
    std::string context;
    std::vector<std::string> expr_stack;

    int newVersion(const std::string& var) {
        int v = counter[var]++;
        var_stack[var].push(v);
        return v;
    }

    int currentVersion(const std::string& var) {
        if (var_stack[var].empty()) return -1;
        return var_stack[var].top();
    }

    void popVersion(const std::string& var) {
        if (!var_stack[var].empty()) var_stack[var].pop();
    }
};

static std::string exprLabel(const ExprPtr& e) {
    if (!e) return "null";
    switch (e->kind) {
    case ExprKind::VarRef: return "VarRef(" + e->var_name + ")";
    case ExprKind::BinaryOp: return "BinaryOp(" + e->op + ")";
    case ExprKind::UnaryOp: return "UnaryOp(" + e->op + ")";
    case ExprKind::FieldAccess: return "FieldAccess(" + e->field_name + ")";
    case ExprKind::Call: return "Call(" + e->callee + ")";
    case ExprKind::ArrayAccess: return "ArrayAccess";
    case ExprKind::Cast: return "Cast";
    case ExprKind::Ternary: return "Ternary";
    case ExprKind::Slice: return "Slice";
    case ExprKind::BitSelect: return "BitSelect";
    case ExprKind::WriteSlice: return "WriteSlice";
    case ExprKind::WriteBit: return "WriteBit";
    case ExprKind::Concat: return "Concat";
    case ExprKind::Repeat: return "Repeat";
    case ExprKind::ReduceOr: return "ReduceOr";
    case ExprKind::ReduceAnd: return "ReduceAnd";
    case ExprKind::ReduceXor: return "ReduceXor";
    default: return "Expr";
    }
}

static std::string exprStackString(const RenameState& state) {
    std::ostringstream os;
    for (size_t i = 0; i < state.expr_stack.size(); ++i) {
        if (i) os << " -> ";
        os << state.expr_stack[i];
    }
    return os.str();
}

static ExprPtr renameExprUses(const ExprPtr& e, RenameState& state) {
    if (!e) return nullptr;
    if (!state.error.empty()) return nullptr;
    state.expr_stack.push_back(exprLabel(e));

    if (e->kind == ExprKind::VarRef) {
        auto result = std::make_shared<Expr>(*e);
        int ver = state.currentVersion(e->var_name);
        if (ver < 0) {
            state.error = "SSA read before definition for variable '" + e->var_name + "'";
            if (!state.context.empty()) state.error += " while renaming " + state.context;
            auto stack = exprStackString(state);
            if (!stack.empty()) state.error += " in " + stack;
            state.expr_stack.pop_back();
            return result;
        }
        result->var_name = e->var_name + "_" + std::to_string(ver);
        state.expr_stack.pop_back();
        return result;
    }

    auto result = std::make_shared<Expr>(*e);
    if (result->left) result->left = renameExprUses(result->left, state);
    if (result->right) result->right = renameExprUses(result->right, state);
    if (result->operand) result->operand = renameExprUses(result->operand, state);
    if (result->array_base) result->array_base = renameExprUses(result->array_base, state);
    if (result->index) result->index = renameExprUses(result->index, state);
    if (result->struct_base) result->struct_base = renameExprUses(result->struct_base, state);
    if (result->cast_expr) result->cast_expr = renameExprUses(result->cast_expr, state);
    if (result->cond) result->cond = renameExprUses(result->cond, state);
    if (result->then_expr) result->then_expr = renameExprUses(result->then_expr, state);
    if (result->else_expr) result->else_expr = renameExprUses(result->else_expr, state);
    if (result->base) result->base = renameExprUses(result->base, state);
    if (result->value) result->value = renameExprUses(result->value, state);
    for (auto& arg : result->args) {
        arg = renameExprUses(arg, state);
    }
    for (auto& part : result->parts) {
        part = renameExprUses(part, state);
    }
    state.expr_stack.pop_back();
    return result;
}

static ExprPtr renameExprDef(const ExprPtr& e, RenameState& state) {
    if (!e) return nullptr;
    if (!state.error.empty()) return nullptr;
    if (e->kind == ExprKind::VarRef) {
        auto result = std::make_shared<Expr>(*e);
        int ver = state.newVersion(e->var_name);
        result->var_name = e->var_name + "_" + std::to_string(ver);
        return result;
    }
    // For array/field access targets, rename the base but keep structure
    auto result = std::make_shared<Expr>(*e);
    if (result->array_base) result->array_base = renameExprUses(result->array_base, state);
    if (result->index) result->index = renameExprUses(result->index, state);
    if (result->struct_base) result->struct_base = renameExprUses(result->struct_base, state);
    return result;
}

struct RenameContext {
    RenameState& state;
    const CFG& cfg;
    const DomInfo& dom;
    std::unordered_map<BlockId, std::set<std::string>>& phi_vars;
    std::vector<SSABlock>& ssa_blocks;
    std::vector<bool> visited;

    void rename(BlockId b) {
        if (visited[b]) return;
        visited[b] = true;

        SSABlock& ssab = ssa_blocks[b];
        ssab.id = b;

        // Track versions pushed in this block for cleanup
        std::vector<std::pair<std::string, int>> pushed;

        // Rename phi results. Phi placeholders are created before renaming so
        // predecessors can always append operands even if the successor has not
        // been visited yet.
        for (auto& phi : ssab.phis) {
            phi.result.version = state.newVersion(phi.result.base_name);
            pushed.push_back({phi.result.base_name, 1});
        }

        // Rename statements
        auto* bb = cfg.blocks[b].get();
        for (auto& s : bb->stmts) {
            if (!s) continue;
            if (!state.error.empty()) break;
            auto renamed = std::make_shared<Stmt>(*s);

            if (s->kind == StmtKind::Assign) {
                state.context = "assignment value";
                renamed->assign_value = renameExprUses(s->assign_value, state);
                state.context = "assignment target";
                renamed->assign_target = renameExprDef(s->assign_target, state);
                state.context.clear();
                if (s->assign_target && s->assign_target->kind == ExprKind::VarRef) {
                    pushed.push_back({s->assign_target->var_name, 1});
                }
            } else if (s->kind == StmtKind::Decl) {
                if (s->decl_init.has_value()) {
                    state.context = "declaration initializer for '" + s->decl_name + "'";
                    renamed->decl_init = renameExprUses(s->decl_init.value(), state);
                    state.context.clear();
                    state.newVersion(s->decl_name);
                    pushed.push_back({s->decl_name, 1});
                    renamed->decl_name = s->decl_name + "_" + std::to_string(
                        state.currentVersion(s->decl_name));
                }
            } else if (s->kind == StmtKind::ExprStmt) {
                state.context = "expression statement";
                renamed->expr_stmt = renameExprUses(s->expr_stmt, state);
                state.context.clear();
            }

            ssab.stmts.push_back(renamed);
        }

        // Rename terminator condition
        ssab.term = bb->term;
        if (ssab.term.condition) {
            ssab.term.condition = renameExprUses(ssab.term.condition, state);
        }
        if (!state.error.empty()) return;

        // Fill phi operands in successors
        for (auto succ : bb->successors) {
            for (auto& phi : ssa_blocks[succ].phis) {
                int ver = state.currentVersion(phi.result.base_name);
                if (ver < 0) continue;
                phi.operands.push_back({b, SSAVar{phi.result.base_name, ver}});
            }
        }

        // Recurse into dominated blocks
        for (int i = 0; i < static_cast<int>(cfg.blocks.size()); ++i) {
            if (dom.idom[i] == b && i != b) {
                rename(i);
            }
        }

        // Pop versions
        for (auto& [var, count] : pushed) {
            for (int c = 0; c < count; ++c) {
                state.popVersion(var);
            }
        }
    }
};

SSAProgram buildSSA(const CFG& cfg,
                    const std::unordered_map<std::string, TypeInfo>& seed_symbols) {
    SSAProgram program;
    program.entry = cfg.entry;
    program.exit = cfg.exit;

    int n = static_cast<int>(cfg.blocks.size());
    program.blocks.resize(n);

    // Compute dominators
    DomInfo dom = computeDominators(cfg);

    // Collect variable definitions
    VarDefs defs;
    collectDefs(cfg, defs);
    program.var_types = defs.types;
    for (auto& [name, type] : seed_symbols) {
        program.var_types.emplace(name, type);
    }

    // Place phi nodes
    auto phi_vars = placePhi(defs, dom);

    // Initialize SSA blocks with phi placeholders
    for (auto& [block, vars] : phi_vars) {
        for (auto& var : vars) {
            PhiNode phi;
            phi.result.base_name = var;
            phi.result.version = 0;
            if (defs.types.count(var)) phi.type = defs.types[var];
            phi.expected_predecessors = cfg.blocks[block]->predecessors.size();
            program.blocks[block].phis.push_back(phi);
        }
    }

    // Rename
    RenameState state;
    for (auto& [name, type] : seed_symbols) {
        state.allowed_symbols.insert(name);
        state.newVersion(name);
    }
    RenameContext ctx{state, cfg, dom, phi_vars, program.blocks, {}};
    ctx.visited.assign(n, false);

    ctx.rename(cfg.entry);
    if (!state.error.empty()) {
        program.error = state.error;
        return program;
    }

    // Record max versions
    program.max_versions = state.counter;

    return program;
}

} // namespace pred
