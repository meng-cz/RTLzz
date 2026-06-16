#pragma once

#include "ir/SSA.h"
#include "predicate/PredicateIR.h"

namespace pred {

// Convert an SSA program (with branches and phi nodes) into a flat
// list of predicate-guarded assignments (PredicateProgram).
// This eliminates all control flow.
PredicateProgram predicate(const SSAProgram& ssa);

} // namespace pred
