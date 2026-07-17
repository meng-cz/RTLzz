#pragma once

#include "backend/beir.hpp"

#include <string>

namespace pred::rtlgen {

std::string emitSystemVerilog(const beir::Program& program);

} // namespace pred::rtlgen
