#pragma once

#include "ast/AST.h"
#include <string>

namespace pred::IntSemantics {

struct BinaryResult {
    TypeInfo type;
    std::string error;
};

BinaryResult binaryResultType(const std::string& op,
                              const TypeInfo& lhs,
                              const TypeInfo& rhs);

TypeInfo unaryResultType(const std::string& op, const TypeInfo& operand);

} // namespace pred::IntSemantics
