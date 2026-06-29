#pragma once

#include "predicate/PredicateIR.h"
#include <string>

namespace pred {

// Emit a backend-oriented JSON shape: a list of scalar signals where each signal
// has at most one driver operation. Expression trees are flattened into
// temporary signals.
std::string emitListJson(const PredicateProgram& prog);

} // namespace pred
