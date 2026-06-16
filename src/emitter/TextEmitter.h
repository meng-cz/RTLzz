#pragma once

#include "predicate/PredicateIR.h"
#include <string>

namespace pred {

// Emit a PredicateProgram as human-readable text.
// Format:
//   function: <name>
//   assignments: <count>
//
//   when (<guard>): <target> = <value>
//   when (<guard>): <target> = <value>
//   ...
std::string emitText(const PredicateProgram& prog);

// Helper: convert an expression to a readable string
std::string exprToString(const ExprPtr& e);

} // namespace pred
