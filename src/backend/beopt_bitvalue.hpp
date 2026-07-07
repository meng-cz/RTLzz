#pragma once

#include "backend/beir.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pred::beir::opt {
namespace bitvalue_detail {

inline int widthOf(const ValueType& type) {
    return type.width > 0 ? type.width : 1;
}

inline std::size_t limbCount(int width) {
    return width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
}

inline std::uint64_t highMask(int width) {
    int rem = width % 64;
    if (rem == 0) return std::numeric_limits<std::uint64_t>::max();
    return (std::uint64_t{1} << rem) - 1;
}

inline void trim(std::vector<std::uint64_t>& limbs, int width) {
    limbs.resize(limbCount(width), 0);
    if (!limbs.empty()) limbs.back() &= highMask(width);
}

inline std::vector<std::uint64_t> zeros(int width) {
    return std::vector<std::uint64_t>(limbCount(width), 0);
}

inline std::vector<std::uint64_t> ones(int width) {
    std::vector<std::uint64_t> out(limbCount(width), std::numeric_limits<std::uint64_t>::max());
    trim(out, width);
    return out;
}

inline bool getBit(const std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return false;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    if (limb >= limbs.size()) return false;
    return ((limbs[limb] >> (bit % 64)) & 1ULL) != 0;
}

inline void setBit(std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    if (limb >= limbs.size()) return;
    limbs[limb] |= (1ULL << (bit % 64));
}

inline bool vectorsEqual(std::vector<std::uint64_t> lhs, std::vector<std::uint64_t> rhs, int width) {
    trim(lhs, width);
    trim(rhs, width);
    return lhs == rhs;
}

inline std::vector<std::uint64_t> bitNot(std::vector<std::uint64_t> value, int width) {
    for (auto& limb : value) limb = ~limb;
    trim(value, width);
    return value;
}

inline std::vector<std::uint64_t> binaryBitOp(const std::vector<std::uint64_t>& lhs,
                                              const std::vector<std::uint64_t>& rhs,
                                              int width,
                                              OpCode op) {
    std::vector<std::uint64_t> out = zeros(width);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint64_t a = i < lhs.size() ? lhs[i] : 0;
        std::uint64_t b = i < rhs.size() ? rhs[i] : 0;
        if (op == OpCode::BitAnd) out[i] = a & b;
        else if (op == OpCode::BitOr) out[i] = a | b;
        else if (op == OpCode::BitXor) out[i] = a ^ b;
    }
    trim(out, width);
    return out;
}

inline std::vector<std::uint64_t> fromU128(unsigned __int128 value, int width) {
    std::vector<std::uint64_t> out = zeros(width);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint64_t>(value);
        value >>= 64;
    }
    trim(out, width);
    return out;
}

inline bool u64(const Operand::Constant& constant, std::uint64_t& out);
inline Operand::Constant makeConstant(std::vector<std::uint64_t> limbs, int width, bool signed_view = false);

inline bool evalSmallBinary(const Operand::Constant& lhs,
                            const Operand::Constant& rhs,
                            OpCode op,
                            int width,
                            Operand::Constant& out) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    if (!u64(lhs, a) || !u64(rhs, b)) return false;

    unsigned __int128 result = 0;
    switch (op) {
    case OpCode::Add: result = static_cast<unsigned __int128>(a) + b; break;
    case OpCode::Sub: result = static_cast<unsigned __int128>(a) - b; break;
    case OpCode::Mul: result = static_cast<unsigned __int128>(a) * b; break;
    case OpCode::Div:
        if (b == 0) return false;
        result = a / b;
        break;
    case OpCode::Mod:
        if (b == 0) return false;
        result = a % b;
        break;
    case OpCode::LogicAnd: result = (a != 0 && b != 0) ? 1 : 0; width = 1; break;
    case OpCode::LogicOr: result = (a != 0 || b != 0) ? 1 : 0; width = 1; break;
    default:
        return false;
    }
    out = makeConstant(fromU128(result, width), width);
    return true;
}

inline std::vector<std::uint64_t> shiftLeft(const std::vector<std::uint64_t>& value, int width, int amount) {
    std::vector<std::uint64_t> out = zeros(width);
    if (amount < 0 || amount >= width) return out;
    for (int bit = 0; bit + amount < width; ++bit) {
        if (getBit(value, bit)) setBit(out, bit + amount);
    }
    return out;
}

inline std::vector<std::uint64_t> shiftRight(const std::vector<std::uint64_t>& value, int width, int amount) {
    std::vector<std::uint64_t> out = zeros(width);
    if (amount < 0 || amount >= width) return out;
    for (int bit = amount; bit < width; ++bit) {
        if (getBit(value, bit)) setBit(out, bit - amount);
    }
    return out;
}

inline std::vector<std::uint64_t> sliceBits(const std::vector<std::uint64_t>& value, int lo, int width) {
    std::vector<std::uint64_t> out = zeros(width);
    for (int bit = 0; bit < width; ++bit) {
        if (getBit(value, lo + bit)) setBit(out, bit);
    }
    return out;
}

inline std::vector<std::uint64_t> concatBits(const std::vector<Operand::Constant>& constants, int width) {
    std::vector<std::uint64_t> out = zeros(width);
    int dst = width;
    for (const auto& constant : constants) {
        int w = widthOf(ValueType{constant.width, {}});
        dst -= w;
        for (int bit = 0; bit < w; ++bit) {
            if (getBit(constant.limbs, bit)) setBit(out, dst + bit);
        }
    }
    return out;
}

inline std::vector<std::uint64_t> repeatBits(const Operand::Constant& constant, int times, int width) {
    std::vector<std::uint64_t> out = zeros(width);
    int src_width = widthOf(ValueType{constant.width, {}});
    int dst = 0;
    for (int i = 0; i < times; ++i) {
        for (int bit = 0; bit < src_width && dst + bit < width; ++bit) {
            if (getBit(constant.limbs, bit)) setBit(out, dst + bit);
        }
        dst += src_width;
    }
    return out;
}

inline Operand::Constant makeConstant(std::vector<std::uint64_t> limbs, int width, bool signed_view) {
    Operand::Constant constant;
    constant.width = width;
    constant.signed_view = signed_view;
    constant.limbs = std::move(limbs);
    trim(constant.limbs, width);
    return constant;
}

inline Operand literalOperand(const Operand::Constant& constant, const ValueType& type) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.constant = constant;
    operand.type = type;
    operand.signed_view = constant.signed_view;
    operand.text = "const";
    return operand;
}

inline bool u64(const Operand::Constant& constant, std::uint64_t& out) {
    if (!constant.fitsU64()) return false;
    out = constant.toU64();
    return true;
}

struct Facts {
    int width = 0;
    bool constant = false;
    Operand::Constant value;
    std::vector<std::uint64_t> known_zero;
    std::vector<std::uint64_t> known_one;

    bool bitKnownZero(int bit) const { return getBit(known_zero, bit); }
    bool bitKnownOne(int bit) const { return getBit(known_one, bit); }
};

inline Facts unknown(int width) {
    Facts facts;
    facts.width = width;
    facts.known_zero = zeros(width);
    facts.known_one = zeros(width);
    return facts;
}

inline Facts fromConstant(const Operand::Constant& constant, int width) {
    Facts facts;
    facts.width = width;
    facts.constant = true;
    facts.value = makeConstant(constant.limbs, width, constant.signed_view);
    facts.known_one = facts.value.limbs;
    facts.known_zero = bitNot(facts.known_one, width);
    return facts;
}

inline const Facts& operandFacts(const Operand& operand,
                                 const std::vector<Facts>& facts,
                                 const Facts& unknown_one) {
    if (operand.kind == OperandKind::Symbol && operand.node < facts.size()) return facts[operand.node];
    return unknown_one;
}

inline Facts literalFacts(const Operand& operand) {
    return fromConstant(operand.constant, widthOf(operand.type));
}

inline Facts zextFacts(const Facts& src, int out_width) {
    Facts out = unknown(out_width);
    int copy_width = std::min(src.width, out_width);
    out.known_zero = sliceBits(src.known_zero, 0, copy_width);
    out.known_one = sliceBits(src.known_one, 0, copy_width);
    out.known_zero.resize(limbCount(out_width), 0);
    out.known_one.resize(limbCount(out_width), 0);
    for (int bit = src.width; bit < out_width; ++bit) setBit(out.known_zero, bit);
    trim(out.known_zero, out_width);
    trim(out.known_one, out_width);
    if (src.constant) out = fromConstant(makeConstant(src.value.limbs, copy_width, src.value.signed_view), out_width);
    return out;
}

inline Facts truncFacts(const Facts& src, int out_width) {
    Facts out = unknown(out_width);
    out.known_zero = sliceBits(src.known_zero, 0, out_width);
    out.known_one = sliceBits(src.known_one, 0, out_width);
    if (src.constant) out = fromConstant(makeConstant(src.value.limbs, out_width, src.value.signed_view), out_width);
    return out;
}

inline Facts sliceFacts(const Facts& src, int lo, int out_width) {
    Facts out = unknown(out_width);
    out.known_zero = sliceBits(src.known_zero, lo, out_width);
    out.known_one = sliceBits(src.known_one, lo, out_width);
    if (src.constant) out = fromConstant(makeConstant(sliceBits(src.value.limbs, lo, out_width), out_width, src.value.signed_view), out_width);
    return out;
}

inline Facts bitSelectFacts(const Facts& src, int bit) {
    Facts out = unknown(1);
    if (src.bitKnownZero(bit)) setBit(out.known_zero, 0);
    if (src.bitKnownOne(bit)) setBit(out.known_one, 0);
    if (src.constant) {
        out = fromConstant(makeConstant(getBit(src.value.limbs, bit) ? std::vector<std::uint64_t>{1} :
                                        std::vector<std::uint64_t>{0}, 1, src.value.signed_view), 1);
    }
    return out;
}

inline Facts bitNotFacts(const Facts& src) {
    Facts out = unknown(src.width);
    out.known_zero = src.known_one;
    out.known_one = src.known_zero;
    if (src.constant) out = fromConstant(makeConstant(bitNot(src.value.limbs, src.width), src.width, src.value.signed_view), src.width);
    return out;
}

inline Facts bitAndFacts(const Facts& lhs, const Facts& rhs, int width) {
    Facts out = unknown(width);
    for (std::size_t i = 0; i < out.known_zero.size(); ++i) {
        std::uint64_t lz = i < lhs.known_zero.size() ? lhs.known_zero[i] : 0;
        std::uint64_t rz = i < rhs.known_zero.size() ? rhs.known_zero[i] : 0;
        std::uint64_t lo = i < lhs.known_one.size() ? lhs.known_one[i] : 0;
        std::uint64_t ro = i < rhs.known_one.size() ? rhs.known_one[i] : 0;
        out.known_zero[i] = lz | rz;
        out.known_one[i] = lo & ro;
    }
    trim(out.known_zero, width);
    trim(out.known_one, width);
    if (lhs.constant && rhs.constant) {
        out = fromConstant(makeConstant(binaryBitOp(lhs.value.limbs, rhs.value.limbs, width, OpCode::BitAnd), width), width);
    }
    return out;
}

inline Facts bitOrFacts(const Facts& lhs, const Facts& rhs, int width) {
    Facts out = unknown(width);
    for (std::size_t i = 0; i < out.known_zero.size(); ++i) {
        std::uint64_t lz = i < lhs.known_zero.size() ? lhs.known_zero[i] : 0;
        std::uint64_t rz = i < rhs.known_zero.size() ? rhs.known_zero[i] : 0;
        std::uint64_t lo = i < lhs.known_one.size() ? lhs.known_one[i] : 0;
        std::uint64_t ro = i < rhs.known_one.size() ? rhs.known_one[i] : 0;
        out.known_zero[i] = lz & rz;
        out.known_one[i] = lo | ro;
    }
    trim(out.known_zero, width);
    trim(out.known_one, width);
    if (lhs.constant && rhs.constant) {
        out = fromConstant(makeConstant(binaryBitOp(lhs.value.limbs, rhs.value.limbs, width, OpCode::BitOr), width), width);
    }
    return out;
}

inline Facts bitXorFacts(const Facts& lhs, const Facts& rhs, int width) {
    Facts out = unknown(width);
    for (std::size_t i = 0; i < out.known_zero.size(); ++i) {
        std::uint64_t lz = i < lhs.known_zero.size() ? lhs.known_zero[i] : 0;
        std::uint64_t rz = i < rhs.known_zero.size() ? rhs.known_zero[i] : 0;
        std::uint64_t lo = i < lhs.known_one.size() ? lhs.known_one[i] : 0;
        std::uint64_t ro = i < rhs.known_one.size() ? rhs.known_one[i] : 0;
        out.known_zero[i] = (lz & rz) | (lo & ro);
        out.known_one[i] = (lz & ro) | (lo & rz);
    }
    trim(out.known_zero, width);
    trim(out.known_one, width);
    if (lhs.constant && rhs.constant) {
        out = fromConstant(makeConstant(binaryBitOp(lhs.value.limbs, rhs.value.limbs, width, OpCode::BitXor), width), width);
    }
    return out;
}

inline Facts compareFacts(const Facts& lhs, const Facts& rhs, OpCode op) {
    Facts out = unknown(1);
    if (!lhs.constant || !rhs.constant) return out;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    if (!u64(lhs.value, a) || !u64(rhs.value, b)) return out;
    bool result = false;
    if (op == OpCode::Eq) result = vectorsEqual(lhs.value.limbs, rhs.value.limbs, std::max(lhs.width, rhs.width));
    else if (op == OpCode::Ne) result = !vectorsEqual(lhs.value.limbs, rhs.value.limbs, std::max(lhs.width, rhs.width));
    else if (op == OpCode::Lt) result = a < b;
    else if (op == OpCode::Le) result = a <= b;
    else if (op == OpCode::Gt) result = a > b;
    else if (op == OpCode::Ge) result = a >= b;
    else return out;
    return fromConstant(makeConstant({result ? 1ULL : 0ULL}, 1), 1);
}

inline Facts binaryFacts(const Facts& lhs, const Facts& rhs, OpCode op, int width) {
    if (op == OpCode::BitAnd) return bitAndFacts(lhs, rhs, width);
    if (op == OpCode::BitOr) return bitOrFacts(lhs, rhs, width);
    if (op == OpCode::BitXor) return bitXorFacts(lhs, rhs, width);
    if (lhs.constant && rhs.constant) {
        Operand::Constant folded;
        if (evalSmallBinary(lhs.value, rhs.value, op, width, folded)) {
            return fromConstant(folded, folded.width);
        }
    }
    if (op == OpCode::Eq || op == OpCode::Ne || op == OpCode::Lt ||
        op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge) {
        return compareFacts(lhs, rhs, op);
    }
    if ((op == OpCode::Shl || op == OpCode::Shr) && lhs.constant && rhs.constant) {
        std::uint64_t amount = 0;
        if (u64(rhs.value, amount) && amount <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            auto value = op == OpCode::Shl ? shiftLeft(lhs.value.limbs, width, static_cast<int>(amount))
                                           : shiftRight(lhs.value.limbs, width, static_cast<int>(amount));
            return fromConstant(makeConstant(value, width, lhs.value.signed_view), width);
        }
    }
    return unknown(width);
}

inline Facts reduceFacts(const Facts& src, OperationKind kind) {
    Facts out = unknown(1);
    if (kind == OperationKind::ReduceOr) {
        bool any_one = false;
        bool all_zero = true;
        for (int bit = 0; bit < src.width; ++bit) {
            any_one = any_one || src.bitKnownOne(bit);
            all_zero = all_zero && src.bitKnownZero(bit);
        }
        if (any_one) return fromConstant(makeConstant({1}, 1), 1);
        if (all_zero) return fromConstant(makeConstant({0}, 1), 1);
    } else if (kind == OperationKind::ReduceAnd) {
        bool any_zero = false;
        bool all_one = true;
        for (int bit = 0; bit < src.width; ++bit) {
            any_zero = any_zero || src.bitKnownZero(bit);
            all_one = all_one && src.bitKnownOne(bit);
        }
        if (any_zero) return fromConstant(makeConstant({0}, 1), 1);
        if (all_one) return fromConstant(makeConstant({1}, 1), 1);
    } else if (kind == OperationKind::ReduceXor && src.constant) {
        bool parity = false;
        for (int bit = 0; bit < src.width; ++bit) parity = parity != getBit(src.value.limbs, bit);
        return fromConstant(makeConstant({parity ? 1ULL : 0ULL}, 1), 1);
    }
    return out;
}

inline Facts inferOperation(const Operation& op, const std::vector<Facts>& facts) {
    const int width = widthOf(op.type);
    const Facts unknown_operand = unknown(width);
    std::vector<Facts> operands;
    operands.reserve(op.operands.size());
    for (const auto& operand : op.operands) {
        if (operand.kind == OperandKind::Literal) operands.push_back(literalFacts(operand));
        else operands.push_back(operandFacts(operand, facts, unknown_operand));
    }

    if (op.kind == OperationKind::Assign || op.kind == OperationKind::Cast) {
        if (operands.empty()) return unknown(width);
        if (width >= operands[0].width) return zextFacts(operands[0], width);
        return truncFacts(operands[0], width);
    }
    if (op.kind == OperationKind::ZExt) return operands.empty() ? unknown(width) : zextFacts(operands[0], width);
    if (op.kind == OperationKind::Trunc) return operands.empty() ? unknown(width) : truncFacts(operands[0], width);
    if (op.kind == OperationKind::SExt) return operands.empty() ? unknown(width) : unknown(width);
    if (op.kind == OperationKind::Slice) return operands.empty() ? unknown(width) : sliceFacts(operands[0], op.lo, width);
    if (op.kind == OperationKind::BitSelect) return operands.empty() ? unknown(1) : bitSelectFacts(operands[0], op.bit);
    if (op.kind == OperationKind::Unary && !operands.empty()) {
        if (op.op == OpCode::BitNot) return bitNotFacts(operands[0]);
        if (op.op == OpCode::LogicNot && operands[0].constant) {
            return fromConstant(makeConstant({operands[0].value.isZero() ? 1ULL : 0ULL}, 1), 1);
        }
        if (op.op == OpCode::Neg && operands[0].constant && operands[0].value.fitsU64()) {
            unsigned __int128 value = -static_cast<unsigned __int128>(operands[0].value.toU64());
            return fromConstant(makeConstant(fromU128(value, width), width, operands[0].value.signed_view), width);
        }
    }
    if (op.kind == OperationKind::Binary && operands.size() >= 2) {
        return binaryFacts(operands[0], operands[1], op.op, width);
    }
    if (op.kind == OperationKind::Concat && !operands.empty()) {
        Facts out = unknown(width);
        bool all_constant = true;
        std::vector<Operand::Constant> constants;
        int dst = width;
        for (const Facts& operand : operands) {
            all_constant = all_constant && operand.constant;
            constants.push_back(operand.value);
            dst -= operand.width;
            for (int bit = 0; bit < operand.width; ++bit) {
                if (operand.bitKnownZero(bit)) setBit(out.known_zero, dst + bit);
                if (operand.bitKnownOne(bit)) setBit(out.known_one, dst + bit);
            }
        }
        if (all_constant) return fromConstant(makeConstant(concatBits(constants, width), width), width);
        return out;
    }
    if (op.kind == OperationKind::Repeat && !operands.empty()) {
        Facts out = unknown(width);
        for (int i = 0; i < op.times; ++i) {
            int base = i * operands[0].width;
            for (int bit = 0; bit < operands[0].width && base + bit < width; ++bit) {
                if (operands[0].bitKnownZero(bit)) setBit(out.known_zero, base + bit);
                if (operands[0].bitKnownOne(bit)) setBit(out.known_one, base + bit);
            }
        }
        if (operands[0].constant) return fromConstant(makeConstant(repeatBits(operands[0].value, op.times, width), width), width);
        return out;
    }
    if (op.kind == OperationKind::ReduceOr || op.kind == OperationKind::ReduceAnd || op.kind == OperationKind::ReduceXor) {
        return operands.empty() ? unknown(1) : reduceFacts(operands[0], op.kind);
    }
    if (op.kind == OperationKind::Ite && operands.size() >= 3) {
        if (operands[0].constant) return operands[0].value.isZero() ? operands[2] : operands[1];
        Facts out = unknown(width);
        for (std::size_t i = 0; i < out.known_zero.size(); ++i) {
            std::uint64_t a0 = i < operands[1].known_zero.size() ? operands[1].known_zero[i] : 0;
            std::uint64_t b0 = i < operands[2].known_zero.size() ? operands[2].known_zero[i] : 0;
            std::uint64_t a1 = i < operands[1].known_one.size() ? operands[1].known_one[i] : 0;
            std::uint64_t b1 = i < operands[2].known_one.size() ? operands[2].known_one[i] : 0;
            out.known_zero[i] = a0 & b0;
            out.known_one[i] = a1 & b1;
        }
        trim(out.known_zero, width);
        trim(out.known_one, width);
        return out;
    }
    return unknown(width);
}

inline Operand makeLiteralFromFacts(const Facts& facts, const ValueType& type) {
    Operand operand = literalOperand(facts.value, type);
    operand.constant.width = widthOf(type);
    trim(operand.constant.limbs, operand.constant.width);
    return operand;
}

inline void setAssign(Operation& op, Operand operand, const ValueType& type, const std::string& reason) {
    op.kind = OperationKind::Assign;
    op.op = OpCode::None;
    op.operands.clear();
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.to_width = 0;
    op.hi = -1;
    op.lo = -1;
    op.bit = -1;
    op.times = 0;
    op.debug.origin = DebugOrigin::Generated;
    op.debug.reason = reason;
}

inline bool sameLowBits(const Facts& facts, int width) {
    for (int bit = width; bit < facts.width; ++bit) {
        if (!facts.bitKnownZero(bit)) return false;
    }
    return true;
}

inline bool rewriteOperation(Operation& op, const Program& program, const Facts& result, const std::vector<Facts>& facts) {
    if (result.constant && op.kind != OperationKind::PortRead && op.kind != OperationKind::Lookup &&
        op.kind != OperationKind::ArrayAccess) {
        if (op.kind == OperationKind::Assign && op.operands.size() == 1 &&
            op.operands[0].kind == OperandKind::Literal &&
            vectorsEqual(op.operands[0].constant.limbs, result.value.limbs, widthOf(op.type))) {
            return false;
        }
        setAssign(op, makeLiteralFromFacts(result, op.type), op.type, "constant folded by BEIR bit-value optimization");
        return true;
    }

    if ((op.kind == OperationKind::ZExt || op.kind == OperationKind::Trunc || op.kind == OperationKind::Cast) &&
        op.operands.size() == 1) {
        Operand& src = op.operands[0];
        int out_width = widthOf(op.type);
        int src_width = widthOf(src.type);
        if (out_width == src_width) {
            setAssign(op, src, op.type, "removed identity width conversion");
            return true;
        }
        if (op.kind == OperationKind::Trunc && src.kind == OperandKind::Symbol && src.node < facts.size()) {
            const Facts& src_facts = facts[src.node];
            if (out_width < src_width && sameLowBits(src_facts, out_width)) {
                if (const Signal* src_signal = program.findSignal(src.node)) {
                    if (src_signal->driver &&
                        (src_signal->driver->kind == OperationKind::ZExt ||
                         src_signal->driver->kind == OperationKind::Cast ||
                         src_signal->driver->kind == OperationKind::Assign) &&
                        src_signal->driver->operands.size() == 1 &&
                        widthOf(src_signal->driver->operands[0].type) == out_width) {
                        setAssign(op, src_signal->driver->operands[0], op.type,
                                  "removed redundant widen-then-truncate conversion chain");
                        return true;
                    }
                }
                setAssign(op, src, op.type, "removed truncation of known-zero high bits");
                return true;
            }
        }
    }

    if (op.kind == OperationKind::BitSelect && op.operands.size() == 1) {
        const Facts& src = op.operands[0].kind == OperandKind::Symbol && op.operands[0].node < facts.size()
            ? facts[op.operands[0].node]
            : result;
        if (src.bitKnownZero(op.bit) || src.bitKnownOne(op.bit)) {
            Operand::Constant constant = makeConstant({src.bitKnownOne(op.bit) ? 1ULL : 0ULL}, 1);
            setAssign(op, literalOperand(constant, op.type), op.type, "folded bit select from known bit");
            return true;
        }
    }

    return false;
}

inline std::vector<Facts> analyze(const Program& program) {
    std::vector<Facts> facts(program.signals.size());
    for (const auto& signal : program.signals) {
        Facts out = unknown(widthOf(signal.type));
        if (signal.driver) out = inferOperation(*signal.driver, facts);
        out.width = widthOf(signal.type);
        trim(out.known_zero, out.width);
        trim(out.known_one, out.width);
        facts[signal.id] = std::move(out);
    }
    return facts;
}

} // namespace bitvalue_detail

inline bool propagateBitValues(MutableProgram& graph) {
    using namespace bitvalue_detail;

    const std::vector<Facts> facts = analyze(graph.program());
    bool changed = false;
    for (auto& signal : graph.program().signals) {
        if (!signal.driver || signal.id >= facts.size()) continue;
        bool signal_changed = rewriteOperation(*signal.driver, graph.program(), facts[signal.id], facts);
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }
    return changed;
}

} // namespace pred::beir::opt
