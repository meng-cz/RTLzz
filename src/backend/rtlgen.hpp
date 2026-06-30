#pragma once

#include "backend/beir.hpp"
#include "predicate/PredicateIR.h"

#include <string>

namespace pred::rtlgen {

std::string emitSystemVerilog(const beir::Program& program);
std::string emitSystemVerilog(const PredicateProgram& source);

} // namespace pred::rtlgen
