#pragma once

#include "ast/AST.h"
#include <clang-c/Index.h>
#include <functional>
#include <optional>

namespace pred {

std::optional<TypeInfo> recognizeHwIntType(CXType type);
std::optional<TypeInfo> recognizeBuiltinFixedWidth(CXType type);
std::optional<TypeInfo> recognizeRecordType(CXType type);

std::optional<TypeInfo> recognizeStdArrayType(
    CXType type,
    const std::function<TypeInfo(CXType)>& convert_elem);

} // namespace pred
