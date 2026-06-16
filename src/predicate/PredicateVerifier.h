#pragma once

#include "predicate/PredicateIR.h"
#include <string>

namespace pred {

struct PredicateVerifyResult {
    bool ok = true;
    std::string error;
};

PredicateVerifyResult verifyPredicateProgram(const PredicateProgram& program);

} // namespace pred
