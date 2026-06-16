#pragma once

#include "ir/CFG.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace pred {

// SSA variable: original name + version number
struct SSAVar {
    std::string base_name;
    int version;

    std::string str() const {
        return base_name + "_" + std::to_string(version);
    }
};

// Phi function at a join point
struct PhiNode {
    SSAVar result;                          // x_3 = phi(...)
    std::vector<std::pair<BlockId, SSAVar>> operands; // [(pred_block, x_version), ...]
    TypeInfo type;
};

// SSA-form basic block: original stmts with variables renamed + phi nodes
struct SSABlock {
    BlockId id;
    std::vector<PhiNode> phis;
    std::vector<StmtPtr> stmts;  // stmts with VarRef names replaced by SSA names
    Terminator term;             // condition expr also renamed
};

struct SSAProgram {
    std::vector<SSABlock> blocks;
    BlockId entry;
    BlockId exit;
    // All variables and their final versions
    std::unordered_map<std::string, int> max_versions;
    // Type info for each base variable
    std::unordered_map<std::string, TypeInfo> var_types;
    std::string error;
};

// Build SSA form from a CFG.
// Inserts phi nodes at dominance frontiers and renames variables.
SSAProgram buildSSA(const CFG& cfg,
                    const std::unordered_map<std::string, TypeInfo>& seed_symbols = {});

} // namespace pred
