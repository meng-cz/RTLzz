#pragma once

#include "predicate/PredicateIR.h"
#include <string>

namespace pred {

// Emit a PredicateProgram as structured JSON.
// Format:
// {
//   "function": "name",
//   "symbols": { "var": { "type": "...", "width": N, "signed": bool } },
//   "assignments": [
//     { "guard": "...", "target": "...", "value": "...",
//       "guard_expr": {...}, "target_expr": {...}, "value_expr": {...} }
//   ]
// }
std::string emitJson(const PredicateProgram& prog);

} // namespace pred
