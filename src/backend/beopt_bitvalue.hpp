#pragma once

#include "backend/beir.hpp"

#include <cstdint>
#include <limits>
#include <string>
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

inline bool getBit(const std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return false;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    if (limb >= limbs.size()) return false;
    return ((limbs[limb] >> (bit % 64)) & 1ULL) != 0;
}

inline bool vectorsEqual(std::vector<std::uint64_t> lhs, std::vector<std::uint64_t> rhs, int width) {
    trim(lhs, width);
    trim(rhs, width);
    return lhs == rhs;
}

inline Operand::Constant makeConstant(std::vector<std::uint64_t> limbs, int width, bool signed_view = false) {
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

using Facts = ValueFacts;

inline bool bitKnownZero(const Facts& facts, int bit) {
    return getBit(facts.known_zero, bit);
}

inline bool bitKnownOne(const Facts& facts, int bit) {
    return getBit(facts.known_one, bit);
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
        if (!bitKnownZero(facts, bit)) return false;
    }
    return true;
}

inline bool rewriteOperation(Operation& op, const Program& program, const Facts& result) {
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
        if (op.kind == OperationKind::Trunc && src.kind == OperandKind::Symbol) {
            if (const Signal* src_signal = program.findSignal(src.node)) {
                const Facts& src_facts = src_signal->value;
                if (out_width < src_width && src_facts.valid && sameLowBits(src_facts, out_width)) {
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
                    setAssign(op, src, op.type, "removed truncation of known-zero high bits");
                    return true;
                }
            }
        }
    }

    if (op.kind == OperationKind::BitSelect && op.operands.size() == 1) {
        const Facts* src = &result;
        if (op.operands[0].kind == OperandKind::Symbol) {
            if (const Signal* signal = program.findSignal(op.operands[0].node)) src = &signal->value;
        }
        if (src->valid && (bitKnownZero(*src, op.bit) || bitKnownOne(*src, op.bit))) {
            Operand::Constant constant = makeConstant({bitKnownOne(*src, op.bit) ? 1ULL : 0ULL}, 1);
            setAssign(op, literalOperand(constant, op.type), op.type, "folded bit select from known bit");
            return true;
        }
    }

    return false;
}

} // namespace bitvalue_detail

inline bool propagateBitValues(MutableProgram& graph) {
    using namespace bitvalue_detail;

    graph.ensureValueFacts();
    bool changed = false;
    for (auto& signal : graph.program().signals) {
        if (!signal.driver || !signal.value.valid) continue;
        bool signal_changed = rewriteOperation(*signal.driver, graph.program(), signal.value);
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
