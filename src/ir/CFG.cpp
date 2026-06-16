#include "ir/CFG.h"

namespace pred {

BasicBlock* CFG::newBlock() {
    auto bb = std::make_unique<BasicBlock>();
    bb->id = static_cast<BlockId>(blocks.size());
    auto* ptr = bb.get();
    blocks.push_back(std::move(bb));
    return ptr;
}

BasicBlock* CFG::getBlock(BlockId id) {
    if (id < 0 || id >= static_cast<int>(blocks.size())) return nullptr;
    return blocks[id].get();
}

void CFG::addEdge(BlockId from, BlockId to) {
    auto* f = getBlock(from);
    auto* t = getBlock(to);
    if (!f || !t) return;
    f->successors.push_back(to);
    t->predecessors.push_back(from);
}

// --- CFG Builder ---

struct CFGBuilder {
    CFG cfg;
    BasicBlock* current = nullptr;

    // Break targets stack (for loops and switch)
    std::vector<BlockId> break_targets;
    std::vector<BlockId> continue_targets;

    BasicBlock* newBlock() {
        return cfg.newBlock();
    }

    void setCurrent(BasicBlock* bb) {
        current = bb;
    }

    void emit(StmtPtr s) {
        if (current) {
            current->stmts.push_back(std::move(s));
        }
    }

    void jump(BlockId target) {
        if (!current) return;
        current->term.kind = TermKind::Jump;
        current->term.jump_target = target;
        cfg.addEdge(current->id, target);
        current = nullptr;
    }

    void branch(ExprPtr cond, BlockId true_bb, BlockId false_bb) {
        if (!current) return;
        current->term.kind = TermKind::Branch;
        current->term.condition = std::move(cond);
        current->term.true_target = true_bb;
        current->term.false_target = false_bb;
        cfg.addEdge(current->id, true_bb);
        cfg.addEdge(current->id, false_bb);
        current = nullptr;
    }

    void buildStmts(const std::vector<StmtPtr>& stmts);
    void buildStmt(const StmtPtr& s);
};

void CFGBuilder::buildStmt(const StmtPtr& s) {
    if (!s) return;

    switch (s->kind) {
    case StmtKind::Assign:
    case StmtKind::Decl:
    case StmtKind::ExprStmt:
        emit(s);
        break;

    case StmtKind::Block:
        buildStmts(s->block_stmts);
        break;

    case StmtKind::If: {
        auto* then_bb = newBlock();
        auto* else_bb = newBlock();
        auto* merge_bb = newBlock();

        branch(s->if_cond, then_bb->id, else_bb->id);

        setCurrent(then_bb);
        buildStmts(s->if_then);
        if (current) jump(merge_bb->id);

        setCurrent(else_bb);
        if (!s->if_else.empty()) {
            buildStmts(s->if_else);
        }
        if (current) jump(merge_bb->id);

        setCurrent(merge_bb);
        break;
    }

    case StmtKind::For: {
        // for (init; cond; step) body
        if (s->for_init) buildStmt(s->for_init);

        auto* header_bb = newBlock();
        auto* body_bb = newBlock();
        auto* step_bb = newBlock();
        auto* exit_bb = newBlock();

        if (current) jump(header_bb->id);

        setCurrent(header_bb);
        if (s->for_cond) {
            branch(s->for_cond, body_bb->id, exit_bb->id);
        } else {
            jump(body_bb->id);
        }

        break_targets.push_back(exit_bb->id);
        continue_targets.push_back(step_bb->id);

        setCurrent(body_bb);
        buildStmts(s->for_body);
        if (current) jump(step_bb->id);

        setCurrent(step_bb);
        if (s->for_step) {
            // Wrap step expression as ExprStmt
            auto step_stmt = std::make_shared<Stmt>();
            step_stmt->kind = StmtKind::ExprStmt;
            step_stmt->expr_stmt = s->for_step;
            emit(step_stmt);
        }
        jump(header_bb->id); // back-edge

        break_targets.pop_back();
        continue_targets.pop_back();
        setCurrent(exit_bb);
        break;
    }

    case StmtKind::While: {
        auto* header_bb = newBlock();
        auto* body_bb = newBlock();
        auto* exit_bb = newBlock();

        if (current) jump(header_bb->id);

        setCurrent(header_bb);
        branch(s->while_cond, body_bb->id, exit_bb->id);

        break_targets.push_back(exit_bb->id);
        continue_targets.push_back(header_bb->id);

        setCurrent(body_bb);
        buildStmts(s->while_body);
        if (current) jump(header_bb->id);

        break_targets.pop_back();
        continue_targets.pop_back();
        setCurrent(exit_bb);
        break;
    }

    case StmtKind::DoWhile: {
        auto* body_bb = newBlock();
        auto* cond_bb = newBlock();
        auto* exit_bb = newBlock();

        if (current) jump(body_bb->id);

        break_targets.push_back(exit_bb->id);
        continue_targets.push_back(cond_bb->id);

        setCurrent(body_bb);
        buildStmts(s->while_body);
        if (current) jump(cond_bb->id);

        setCurrent(cond_bb);
        branch(s->while_cond, body_bb->id, exit_bb->id);

        break_targets.pop_back();
        continue_targets.pop_back();
        setCurrent(exit_bb);
        break;
    }

    case StmtKind::Switch: {
        auto* exit_bb = newBlock();
        break_targets.push_back(exit_bb->id);

        std::vector<std::pair<ExprPtr, BasicBlock*>> case_blocks;
        BasicBlock* default_bb = nullptr;

        for (auto& clause : s->switch_cases) {
            auto* bb = newBlock();
            if (clause.value.has_value()) {
                case_blocks.push_back({clause.value.value(), bb});
            } else {
                default_bb = bb;
            }
        }
        if (!default_bb) default_bb = exit_bb;

        // Build cascading branches
        for (size_t i = 0; i < case_blocks.size(); ++i) {
            auto cond = make_binary("==",
                                    s->switch_expr,
                                    case_blocks[i].first,
                                    TypeInfo{"bool", 1, false});
            auto* next_check = (i + 1 < case_blocks.size())
                ? newBlock() : default_bb;
            branch(cond, case_blocks[i].second->id, next_check->id);
            if (i + 1 < case_blocks.size()) {
                setCurrent(next_check);
            }
        }
        if (case_blocks.empty() && current) {
            jump(default_bb->id);
        }

        // Fill case bodies
        size_t clause_idx = 0;
        for (auto& clause : s->switch_cases) {
            BasicBlock* bb;
            if (clause.value.has_value()) {
                // Find matching block
                bb = case_blocks[clause_idx].second;
                clause_idx++;
            } else {
                bb = default_bb;
            }
            setCurrent(bb);
            for (auto& stmt : clause.body) {
                buildStmt(stmt);
            }
            if (current) jump(exit_bb->id);
        }

        break_targets.pop_back();
        setCurrent(exit_bb);
        break;
    }

    case StmtKind::Break: {
        if (!break_targets.empty() && current) {
            jump(break_targets.back());
        }
        break;
    }

    case StmtKind::Continue: {
        if (!continue_targets.empty() && current) {
            jump(continue_targets.back());
        }
        break;
    }

    case StmtKind::Return: {
        if (current) {
            emit(s);
            current->term.kind = TermKind::Return;
            cfg.addEdge(current->id, cfg.exit);
            current = nullptr;
        }
        break;
    }
    }
}

void CFGBuilder::buildStmts(const std::vector<StmtPtr>& stmts) {
    for (auto& s : stmts) {
        if (!current) break;
        buildStmt(s);
    }
}

CFG buildCFG(const std::vector<StmtPtr>& body) {
    CFGBuilder builder;

    auto* entry = builder.newBlock();
    auto* exit_block = builder.newBlock();
    builder.cfg.entry = entry->id;
    builder.cfg.exit = exit_block->id;

    builder.setCurrent(entry);
    builder.buildStmts(body);

    if (builder.current) {
        builder.jump(exit_block->id);
    }

    return std::move(builder.cfg);
}

} // namespace pred
