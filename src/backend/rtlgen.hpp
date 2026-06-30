#pragma once

#include "backend/beir.hpp"

#include <string>

namespace pred {
struct PredicateProgram;
}

namespace pred::rtlgen {

std::string emitSystemVerilog(const beir::Program& program);
std::string emitSystemVerilog(const PredicateProgram& source);

} // namespace pred::rtlgen
