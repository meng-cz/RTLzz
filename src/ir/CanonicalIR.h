#pragma once

#include "ast/AST.h"

#include <map>
#include <string>

namespace pred {

enum class CanonicalTypeKind {
    Bool,
    Bits,
    SignedView,
};

enum class CanonicalSignedMode {
    Unsigned,
    Signed,
};

struct CanonicalType {
    CanonicalTypeKind kind = CanonicalTypeKind::Bits;
    int width = 0;
    CanonicalSignedMode signed_mode = CanonicalSignedMode::Unsigned;

    static CanonicalType Bool();
    static CanonicalType Bits(int width, CanonicalSignedMode mode = CanonicalSignedMode::Unsigned);
    static CanonicalType SignedView(int width);

    bool isBool() const { return kind == CanonicalTypeKind::Bool; }
    bool isBits() const { return kind == CanonicalTypeKind::Bits; }
    bool isSignedView() const { return kind == CanonicalTypeKind::SignedView; }
    bool hasSignedSemantics() const;
    std::string key() const;
    std::string str() const;
};

enum class CanonicalOp {
    Invalid,
    Literal,
    Var,
    Trunc,
    ZExt,
    SExt,
    SignedView,
    Add,
    Sub,
    Mul,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Neg,
    LShl,
    LShr,
    AShr,
    Eq,
    Ne,
    Ult,
    Ule,
    Ugt,
    Uge,
    Slt,
    Sle,
    Sgt,
    Sge,
    Slice,
    BitSelect,
    WriteSlice,
    WriteBit,
    Concat,
    Repeat,
    ReduceOr,
    ReduceAnd,
    ReduceXor,
    DynamicRangeAt,
    DynamicBitAt,
    Lookup,
    Ite,
};

struct CanonicalOpSpec {
    CanonicalOp op = CanonicalOp::Invalid;
    CanonicalType type;
    std::map<std::string, std::string> attrs;
    std::string error;

    explicit operator bool() const { return error.empty() && op != CanonicalOp::Invalid; }
};

CanonicalType canonicalTypeFromTypeInfo(const TypeInfo& type);
TypeInfo typeInfoFromCanonical(const CanonicalType& type, const TypeInfo& preferred = {});
const char* canonicalOpName(CanonicalOp op);

CanonicalOpSpec canonicalCast(const CanonicalType& from, const CanonicalType& to);
CanonicalOpSpec canonicalBinary(const std::string& op,
                                const CanonicalType& lhs,
                                const CanonicalType& rhs);
CanonicalOpSpec canonicalUnary(const std::string& op, const CanonicalType& operand);
CanonicalOpSpec canonicalSlice(const CanonicalType& base, int hi, int lo);
CanonicalOpSpec canonicalBitSelect(const CanonicalType& base, int bit);
CanonicalOpSpec canonicalWriteSlice(const CanonicalType& base, int hi, int lo,
                                    const CanonicalType& value);
CanonicalOpSpec canonicalWriteBit(const CanonicalType& base, int bit,
                                  const CanonicalType& value);
CanonicalOpSpec canonicalConcat(const CanonicalType* parts, int count);
CanonicalOpSpec canonicalRepeat(const CanonicalType& value, int times);
CanonicalOpSpec canonicalReduce(CanonicalOp op, const CanonicalType& value);

} // namespace pred
