#pragma once

#include "BackendIR.h"
#include "JsonParser.h"

namespace rtlzz {

Program loadPredicateJson(const json::Value& root);

} // namespace rtlzz
