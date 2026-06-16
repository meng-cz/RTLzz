#include "semantics/IntSemantics.h"
#include "ir/CanonicalIR.h"

#include <algorithm>

namespace pred::IntSemantics {
namespace {

bool isHwLike(const TypeInfo& type) {
    return type.is_hw_int ||
           type.hw_kind == "builtin" ||
           type.name.rfind("UInt<", 0) == 0 ||
           type.name.rfind("Int<", 0) == 0;
}

bool isCompareOp(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" ||
           op == ">" || op == ">=" || op == "&&" || op == "||";
}

bool isBitwiseOp(const std::string& op) {
    return op == "&" || op == "|" || op == "^";
}

bool isSignedView(const TypeInfo& type) {
    return type.hw_kind == "signed_view";
}

bool isIntBitVector(const TypeInfo& type) {
    return type.hw_kind == "Int" ||
           type.name.rfind("Int<", 0) == 0 ||
           type.name.rfind("Int <", 0) == 0;
}

TypeInfo bitvectorTypeLike(const TypeInfo& lhs, const TypeInfo& rhs, int width) {
    bool is_signed = isSignedView(lhs) || isSignedView(rhs);
    std::string kind = (is_signed || isIntBitVector(lhs) || isIntBitVector(rhs)) ? "Int" : "UInt";
    if (isHwLike(lhs) || isHwLike(rhs)) {
        return make_hw_type(kind, width, is_signed);
    }
    TypeInfo out = lhs.width >= rhs.width ? lhs : rhs;
    out.width = width;
    out.is_signed = is_signed;
    return out;
}

} // namespace

BinaryResult binaryResultType(const std::string& op,
                              const TypeInfo& lhs,
                              const TypeInfo& rhs) {
    auto canonical = canonicalBinary(op,
        canonicalTypeFromTypeInfo(lhs),
        canonicalTypeFromTypeInfo(rhs));
    if (!canonical.error.empty()) {
        return {{}, canonical.error};
    }

    if (isCompareOp(op) || canonical.type.isBool()) {
        return {make_hw_type("bool", 1, false), ""};
    }
    if (op == "<<" || op == ">>") {
        return {lhs, ""};
    }

    int width = canonical.type.width;
    if (width < 0) width = 0;
    return {bitvectorTypeLike(lhs, rhs, width), ""};
}

TypeInfo unaryResultType(const std::string& op, const TypeInfo& operand) {
    if (op == "!") return make_hw_type("bool", 1, false);
    return operand;
}

} // namespace pred::IntSemantics
