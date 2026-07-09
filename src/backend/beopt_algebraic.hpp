#pragma once

#include "backend/beir.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pred::beir::opt {
namespace algebraic_detail {

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

inline const Operand::Constant* constantOf(const Operand& operand, const Program& program) {
    if (operand.kind == OperandKind::Literal) return &operand.constant;
    if (operand.kind != OperandKind::Symbol) return nullptr;
    const Signal* signal = program.findSignal(operand.node);
    if (!signal || !signal->value.valid || !signal->value.constant) return nullptr;
    return &signal->value.value;
}

inline bool isZero(const Operand& operand, const Program& program) {
    const Operand::Constant* constant = constantOf(operand, program);
    return constant && constant->isZero();
}

inline bool isOne(const Operand& operand, const Program& program) {
    const Operand::Constant* constant = constantOf(operand, program);
    return constant && constant->isOne();
}

inline bool isAllOnes(const Operand& operand, const Program& program) {
    const Operand::Constant* constant = constantOf(operand, program);
    return constant && constant->isAllOnes();
}

inline Operand constantOperand(std::vector<std::uint64_t> limbs, const ValueType& type, bool signed_view = false) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type = type;
    operand.signed_view = signed_view;
    operand.constant.width = widthOf(type);
    operand.constant.signed_view = signed_view;
    operand.constant.limbs = std::move(limbs);
    trim(operand.constant.limbs, operand.constant.width);
    operand.text = "const";
    return operand;
}

inline Operand zeroOperand(const ValueType& type) {
    return constantOperand(std::vector<std::uint64_t>(limbCount(widthOf(type)), 0), type);
}

inline Operand allOnesOperand(const ValueType& type) {
    std::vector<std::uint64_t> limbs(limbCount(widthOf(type)), std::numeric_limits<std::uint64_t>::max());
    return constantOperand(std::move(limbs), type);
}

inline void setAssign(Operation& op,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> source_locs = op.source_locs;
    source_locs.insert(source_locs.end(), op.debug.source_locs.begin(), op.debug.source_locs.end());
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
    op.debug.source_locs = std::move(source_locs);
    op.debug.derived_nodes.clear();
    op.debug.derived_names.clear();
    for (const auto& operand_ref : op.operands) {
        if (operand_ref.kind == OperandKind::Symbol && operand_ref.node != kInvalidNodeId) {
            op.debug.derived_nodes.push_back(operand_ref.node);
        } else if (!operand_ref.text.empty()) {
            op.debug.derived_names.push_back(operand_ref.text);
        }
    }
    addOperandDebugLocs(op.debug, program, op.operands);
    op.source_locs = op.debug.source_locs;
}

inline bool rewriteBinary(Operation& op, const Program& program) {
    if (op.kind != OperationKind::Binary || op.operands.size() != 2) return false;

    Operand lhs = op.operands[0];
    Operand rhs = op.operands[1];
    const ValueType type = op.type;

    switch (op.op) {
    case OpCode::Add:
        if (isZero(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed addition by zero", program);
            return true;
        }
        if (isZero(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed addition of zero", program);
            return true;
        }
        return false;
    case OpCode::Sub:
        if (isZero(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed subtraction by zero", program);
            return true;
        }
        return false;
    case OpCode::Mul:
        if (isZero(lhs, program) || isZero(rhs, program)) {
            setAssign(op, zeroOperand(type), type, "folded multiplication by zero", program);
            return true;
        }
        if (isOne(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed multiplication by one", program);
            return true;
        }
        if (isOne(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed multiplication of one", program);
            return true;
        }
        return false;
    case OpCode::BitAnd:
        if (isZero(lhs, program) || isZero(rhs, program)) {
            setAssign(op, zeroOperand(type), type, "folded bitwise and with zero", program);
            return true;
        }
        if (isAllOnes(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed bitwise and with all ones", program);
            return true;
        }
        if (isAllOnes(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed bitwise and of all ones", program);
            return true;
        }
        return false;
    case OpCode::BitOr:
        if (isZero(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed bitwise or with zero", program);
            return true;
        }
        if (isZero(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed bitwise or of zero", program);
            return true;
        }
        if (isAllOnes(lhs, program) || isAllOnes(rhs, program)) {
            setAssign(op, allOnesOperand(type), type, "folded bitwise or with all ones", program);
            return true;
        }
        return false;
    case OpCode::BitXor:
        if (isZero(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed bitwise xor with zero", program);
            return true;
        }
        if (isZero(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed bitwise xor of zero", program);
            return true;
        }
        return false;
    case OpCode::LogicAnd:
        if (isZero(lhs, program) || isZero(rhs, program)) {
            setAssign(op, zeroOperand(type), type, "folded logical and with false", program);
            return true;
        }
        if (isOne(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed logical and with true", program);
            return true;
        }
        if (isOne(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed logical and of true", program);
            return true;
        }
        return false;
    case OpCode::LogicOr:
        if (isOne(lhs, program) || isOne(rhs, program)) {
            Operand one = constantOperand({1}, type);
            setAssign(op, std::move(one), type, "folded logical or with true", program);
            return true;
        }
        if (isZero(rhs, program)) {
            setAssign(op, std::move(lhs), type, "removed logical or with false", program);
            return true;
        }
        if (isZero(lhs, program)) {
            setAssign(op, std::move(rhs), type, "removed logical or of false", program);
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace algebraic_detail

inline bool simplifyAlgebraicIdentities(MutableProgram& graph) {
    graph.ensureValueFacts();
    bool changed = false;
    for (auto& signal : graph.program().signals) {
        if (!signal.driver) continue;
        bool signal_changed = algebraic_detail::rewriteBinary(*signal.driver, graph.program());
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = signal_changed || changed;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
