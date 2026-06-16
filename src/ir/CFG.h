#pragma once

#include "ast/AST.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace pred {

// A basic block contains a sequence of non-branching statements
// and ends with a terminator (branch, conditional branch, or return).

using BlockId = int;

enum class TermKind {
    Jump,       // unconditional jump to one successor
    Branch,     // conditional: if cond goto true_block else goto false_block
    Return,
    None,       // falls through (only for entry/exit placeholders)
};

struct Terminator {
    TermKind kind = TermKind::None;
    ExprPtr condition;          // for Branch
    BlockId true_target = -1;
    BlockId false_target = -1;
    BlockId jump_target = -1;   // for Jump
};

struct BasicBlock {
    BlockId id;
    std::vector<StmtPtr> stmts;
    Terminator term;

    std::vector<BlockId> successors;
    std::vector<BlockId> predecessors;
};

struct CFG {
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<std::unique_ptr<BasicBlock>> blocks;

    BasicBlock* newBlock();
    BasicBlock* getBlock(BlockId id);
    void addEdge(BlockId from, BlockId to);
};

// Build a CFG from a function's body statements.
// After construction, loops are still present as back-edges.
CFG buildCFG(const std::vector<StmtPtr>& body);

} // namespace pred
