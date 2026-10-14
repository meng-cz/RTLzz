#pragma once

#include "backend/pipeline.hpp"
#include "v2/V2AST.h"

namespace pred::pipeline {
// Reserve the backend-owned bool output before SSA checks output completeness.
// This is a constant placeholder, not a valid bypass or user output definition.
void prepareValidOutput(v2::FunctionAST& function, const Options& options);
}
