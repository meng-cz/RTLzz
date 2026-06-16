#include "ir/CanonicalIR.h"

#include <algorithm>

namespace pred {
namespace {

bool isRelational(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

bool isEquality(const std::string& op) {
    return op == "==" || op == "!=";
}

bool isCompare(const std::string& op) {
    return isEquality(op) || isRelational(op) || op == "&&" || op == "||";
}

bool isBitwise(const std::string& op) {
    return op == "&" || op == "|" || op == "^";
}

CanonicalType bitsResult(int width) {
    return CanonicalType::Bits(std::max(0, width), CanonicalSignedMode::Unsigned);
}

CanonicalOp unsignedRelOp(const std::string& op) {
    if (op == "<") return CanonicalOp::Ult;
    if (op == "<=") return CanonicalOp::Ule;
    if (op == ">") return CanonicalOp::Ugt;
    if (op == ">=") return CanonicalOp::Uge;
    return CanonicalOp::Invalid;
}

CanonicalOp signedRelOp(const std::string& op) {
    if (op == "<") return CanonicalOp::Slt;
    if (op == "<=") return CanonicalOp::Sle;
    if (op == ">") return CanonicalOp::Sgt;
    if (op == ">=") return CanonicalOp::Sge;
    return CanonicalOp::Invalid;
}

} // namespace

CanonicalType CanonicalType::Bool() {
    CanonicalType t;
    t.kind = CanonicalTypeKind::Bool;
    t.width = 1;
    t.signed_mode = CanonicalSignedMode::Unsigned;
    return t;
}

CanonicalType CanonicalType::Bits(int width, CanonicalSignedMode mode) {
    CanonicalType t;
    t.kind = CanonicalTypeKind::Bits;
    t.width = width;
    t.signed_mode = mode;
    return t;
}

CanonicalType CanonicalType::SignedView(int width) {
    CanonicalType t;
    t.kind = CanonicalTypeKind::SignedView;
    t.width = width;
    t.signed_mode = CanonicalSignedMode::Signed;
    return t;
}

bool CanonicalType::hasSignedSemantics() const {
    return kind == CanonicalTypeKind::SignedView ||
           signed_mode == CanonicalSignedMode::Signed;
}

std::string CanonicalType::key() const {
    return str();
}

std::string CanonicalType::str() const {
    if (kind == CanonicalTypeKind::Bool) return "Bool";
    if (kind == CanonicalTypeKind::SignedView) {
        return "SignedView(Bits<" + std::to_string(width) + ">)";
    }
    return std::string("Bits<") + std::to_string(width) + "," +
           (signed_mode == CanonicalSignedMode::Signed ? "signed" : "unsigned") + ">";
}

CanonicalType canonicalTypeFromTypeInfo(const TypeInfo& type) {
    if (type.hw_kind == "bool" || type.name == "bool" || type.width == 1 && type.name == "bool") {
        return CanonicalType::Bool();
    }
    if (type.hw_kind == "signed_view") {
        return CanonicalType::SignedView(type.width);
    }
    return CanonicalType::Bits(type.width, type.is_signed ? CanonicalSignedMode::Signed
                                                          : CanonicalSignedMode::Unsigned);
}

TypeInfo typeInfoFromCanonical(const CanonicalType& type, const TypeInfo& preferred) {
    if (type.kind == CanonicalTypeKind::Bool) return make_hw_type("bool", 1, false);
    if (type.kind == CanonicalTypeKind::SignedView) {
        auto out = make_hw_type("Int", type.width, true);
        out.name = "IntSignedView<" + std::to_string(type.width) + ">";
        out.hw_kind = "signed_view";
        return out;
    }
    std::string kind = preferred.hw_kind == "Int" || preferred.name.rfind("Int<", 0) == 0
        ? "Int"
        : "UInt";
    if (preferred.hw_kind == "signed_view") kind = "Int";
    bool is_signed = type.signed_mode == CanonicalSignedMode::Signed;
    return make_hw_type(kind, type.width, is_signed);
}

const char* canonicalOpName(CanonicalOp op) {
    switch (op) {
    case CanonicalOp::Literal: return "literal";
    case CanonicalOp::Var: return "var";
    case CanonicalOp::Trunc: return "trunc";
    case CanonicalOp::ZExt: return "zext";
    case CanonicalOp::SExt: return "sext";
    case CanonicalOp::SignedView: return "signed_view";
    case CanonicalOp::Add: return "add";
    case CanonicalOp::Sub: return "sub";
    case CanonicalOp::Mul: return "mul";
    case CanonicalOp::BitAnd: return "and";
    case CanonicalOp::BitOr: return "or";
    case CanonicalOp::BitXor: return "xor";
    case CanonicalOp::BitNot: return "not";
    case CanonicalOp::Neg: return "neg";
    case CanonicalOp::LShl: return "shl";
    case CanonicalOp::LShr: return "lshr";
    case CanonicalOp::AShr: return "ashr";
    case CanonicalOp::Eq: return "eq";
    case CanonicalOp::Ne: return "ne";
    case CanonicalOp::Ult: return "ult";
    case CanonicalOp::Ule: return "ule";
    case CanonicalOp::Ugt: return "ugt";
    case CanonicalOp::Uge: return "uge";
    case CanonicalOp::Slt: return "slt";
    case CanonicalOp::Sle: return "sle";
    case CanonicalOp::Sgt: return "sgt";
    case CanonicalOp::Sge: return "sge";
    case CanonicalOp::Slice: return "slice";
    case CanonicalOp::BitSelect: return "bit";
    case CanonicalOp::WriteSlice: return "write_slice";
    case CanonicalOp::WriteBit: return "write_bit";
    case CanonicalOp::Concat: return "concat";
    case CanonicalOp::Repeat: return "repeat";
    case CanonicalOp::ReduceOr: return "reduce_or";
    case CanonicalOp::ReduceAnd: return "reduce_and";
    case CanonicalOp::ReduceXor: return "reduce_xor";
    case CanonicalOp::DynamicRangeAt: return "dynamic_range_at";
    case CanonicalOp::DynamicBitAt: return "dynamic_bit_at";
    case CanonicalOp::Lookup: return "lookup";
    case CanonicalOp::Ite: return "ite";
    case CanonicalOp::Invalid: return "invalid";
    }
    return "invalid";
}

CanonicalOpSpec canonicalCast(const CanonicalType& from, const CanonicalType& to) {
    CanonicalOpSpec out;
    out.type = to.kind == CanonicalTypeKind::SignedView
        ? CanonicalType::Bits(to.width, CanonicalSignedMode::Unsigned)
        : to;
    if (from.width == to.width) {
        out.op = to.kind == CanonicalTypeKind::SignedView ? CanonicalOp::SignedView : CanonicalOp::Var;
        return out;
    }
    if (to.width < from.width) {
        out.op = CanonicalOp::Trunc;
        return out;
    }
    out.op = from.hasSignedSemantics() ? CanonicalOp::SExt : CanonicalOp::ZExt;
    return out;
}

CanonicalOpSpec canonicalBinary(const std::string& op,
                                const CanonicalType& lhs,
                                const CanonicalType& rhs) {
    CanonicalOpSpec out;
    if (op == "/" || op == "%") {
        out.error = "Unsupported division/modulo for combinational Int lowering";
        return out;
    }
    if (op == "&&" || op == "||") {
        out.op = op == "&&" ? CanonicalOp::BitAnd : CanonicalOp::BitOr;
        out.type = CanonicalType::Bool();
        out.attrs["logical"] = "true";
        return out;
    }
    if (isEquality(op)) {
        out.op = op == "==" ? CanonicalOp::Eq : CanonicalOp::Ne;
        out.type = CanonicalType::Bool();
        return out;
    }
    if (isRelational(op)) {
        out.op = (lhs.hasSignedSemantics() || rhs.hasSignedSemantics())
            ? signedRelOp(op)
            : unsignedRelOp(op);
        out.type = CanonicalType::Bool();
        return out;
    }
    if (isBitwise(op)) {
        if (lhs.width > 0 && rhs.width > 0 && lhs.width != rhs.width) {
            out.error = "Unsupported bitwise operation with mismatched widths";
            return out;
        }
        out.op = op == "&" ? CanonicalOp::BitAnd : (op == "|" ? CanonicalOp::BitOr : CanonicalOp::BitXor);
        out.type = bitsResult(std::max(lhs.width, rhs.width));
        return out;
    }
    if (op == "+") {
        out.op = CanonicalOp::Add;
        out.type = bitsResult(std::max(lhs.width, rhs.width) + 1);
        return out;
    }
    if (op == "-") {
        out.op = CanonicalOp::Sub;
        out.type = bitsResult(std::max(lhs.width, rhs.width));
        return out;
    }
    if (op == "*") {
        out.op = CanonicalOp::Mul;
        out.type = bitsResult(lhs.width + rhs.width);
        if (lhs.hasSignedSemantics() || rhs.hasSignedSemantics()) out.attrs["signed"] = "true";
        return out;
    }
    if (op == "<<") {
        out.op = CanonicalOp::LShl;
        out.type = bitsResult(lhs.width);
        return out;
    }
    if (op == ">>") {
        out.op = lhs.hasSignedSemantics() ? CanonicalOp::AShr : CanonicalOp::LShr;
        out.type = bitsResult(lhs.width);
        return out;
    }
    out.error = "Unsupported binary operator for canonical Int lowering: " + op;
    return out;
}

CanonicalOpSpec canonicalUnary(const std::string& op, const CanonicalType& operand) {
    CanonicalOpSpec out;
    if (op == "!") {
        out.op = CanonicalOp::Eq;
        out.type = CanonicalType::Bool();
        out.attrs["logical_not"] = "true";
        return out;
    }
    if (op == "~") {
        out.op = CanonicalOp::BitNot;
        out.type = bitsResult(operand.width);
        return out;
    }
    if (op == "-") {
        out.op = CanonicalOp::Neg;
        out.type = bitsResult(operand.width);
        return out;
    }
    out.error = "Unsupported unary operator for canonical Int lowering: " + op;
    return out;
}

CanonicalOpSpec canonicalSlice(const CanonicalType& base, int hi, int lo) {
    CanonicalOpSpec out;
    if (hi < lo || lo < 0 || hi >= base.width) {
        out.error = "Canonical slice out of bounds";
        return out;
    }
    out.op = CanonicalOp::Slice;
    out.type = bitsResult(hi - lo + 1);
    out.attrs["hi"] = std::to_string(hi);
    out.attrs["lo"] = std::to_string(lo);
    return out;
}

CanonicalOpSpec canonicalBitSelect(const CanonicalType& base, int bit) {
    CanonicalOpSpec out;
    if (bit < 0 || bit >= base.width) {
        out.error = "Canonical bit select out of bounds";
        return out;
    }
    out.op = CanonicalOp::BitSelect;
    out.type = CanonicalType::Bool();
    out.attrs["bit"] = std::to_string(bit);
    return out;
}

CanonicalOpSpec canonicalWriteSlice(const CanonicalType& base, int hi, int lo,
                                    const CanonicalType& value) {
    auto slice = canonicalSlice(base, hi, lo);
    if (!slice.error.empty()) return slice;
    CanonicalOpSpec out;
    if (value.width > 0 && value.width != slice.type.width) {
        out.error = "Canonical write_slice value width mismatch";
        return out;
    }
    out.op = CanonicalOp::WriteSlice;
    out.type = bitsResult(base.width);
    out.attrs = slice.attrs;
    return out;
}

CanonicalOpSpec canonicalWriteBit(const CanonicalType& base, int bit,
                                  const CanonicalType&) {
    auto selected = canonicalBitSelect(base, bit);
    if (!selected.error.empty()) return selected;
    CanonicalOpSpec out;
    out.op = CanonicalOp::WriteBit;
    out.type = bitsResult(base.width);
    out.attrs = selected.attrs;
    return out;
}

CanonicalOpSpec canonicalConcat(const CanonicalType* parts, int count) {
    CanonicalOpSpec out;
    if (!parts || count <= 0) {
        out.error = "Canonical concat requires at least one operand";
        return out;
    }
    int width = 0;
    for (int i = 0; i < count; ++i) width += parts[i].width;
    out.op = CanonicalOp::Concat;
    out.type = bitsResult(width);
    return out;
}

CanonicalOpSpec canonicalRepeat(const CanonicalType& value, int times) {
    CanonicalOpSpec out;
    if (times <= 0) {
        out.error = "Canonical repeat count must be positive";
        return out;
    }
    out.op = CanonicalOp::Repeat;
    out.type = bitsResult(value.width * times);
    out.attrs["times"] = std::to_string(times);
    return out;
}

CanonicalOpSpec canonicalReduce(CanonicalOp op, const CanonicalType&) {
    CanonicalOpSpec out;
    if (op != CanonicalOp::ReduceOr && op != CanonicalOp::ReduceAnd &&
        op != CanonicalOp::ReduceXor) {
        out.error = "Unsupported canonical reduce operator";
        return out;
    }
    out.op = op;
    out.type = CanonicalType::Bool();
    return out;
}

} // namespace pred
