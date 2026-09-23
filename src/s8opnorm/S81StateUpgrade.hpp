#pragma once

#include "s8opnorm/S8Norm.h"

namespace pred::s81stateupgrade {

struct Summary {
    int lifted_statements = 0;
    int renamed_symbols = 0;
};

// Hoist branch-local, total combinational calculations to immediately before
// their controlling branch. The pass creates a fresh temporary for each moved
// definition and rewrites only its single-predecessor descendant region.
Summary hoistBranchLocalCombinational(s8opnorm::S8NormProgram& program);

} // namespace pred::s81stateupgrade
