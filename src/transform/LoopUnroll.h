#pragma once

#include "ast/AST.h"
#include <string>

namespace pred {

struct UnrollConfig {
    int max_iterations = 1024;  // refuse to unroll beyond this
};

struct UnrollResult {
    std::vector<StmtPtr> body;  // unrolled body (loops replaced by sequential stmts)
    std::string error;          // non-empty if a loop couldn't be unrolled
};

// Unroll all static loops in the function body.
// Returns a new body with loops replaced by their unrolled expansions.
// Non-static loops produce an error.
UnrollResult unrollLoops(const std::vector<StmtPtr>& body, const UnrollConfig& config = {});

} // namespace pred
