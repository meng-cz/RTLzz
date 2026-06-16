#pragma once

#include "predicate/PredicateIR.h"
#include <string>

namespace pred {

// Emit a PredicateProgram in SMT-LIB-like syntax.
// Format:
//   ; function: <name>
//   (declare-const <var> (_ BitVec <width>))
//   ...
//   (define <target> (ite <guard> <value> <prev>))
//   ...
std::string emitSmt(const PredicateProgram& prog);

} // namespace pred
