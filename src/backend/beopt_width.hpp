#pragma once

#include "backend/beir.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pred::beir::opt {
namespace width_detail {

inline int widthOf(const ValueType& type) {
    return type.width > 0 ? type.width : 1;
}

inline std::size_t limbCount(int width) {
    return width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
}

inline void trim(std::vector<std::uint64_t>& limbs, int width) {
    limbs.resize(limbCount(width), 0);
    if (limbs.empty() || width % 64 == 0) return;
    limbs.back() &= ((std::uint64_t{1} << (width % 64)) - 1);
}

inline DebugInfo generatedDebug(std::string reason, const std::vector<Operand>& operands = {}) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Generated;
    debug.reason = std::move(reason);
    for (const auto& operand : operands) {
        if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
            debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            debug.derived_names.push_back(operand.text);
        }
    }
    return debug;
}

inline Operand literal(std::uint64_t value, int width) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type.width = width;
    operand.constant.width = width;
    operand.constant.limbs.resize(limbCount(width), 0);
    if (!operand.constant.limbs.empty()) operand.constant.limbs[0] = value;
    trim(operand.constant.limbs, width);
    operand.text = std::to_string(value);
    return operand;
}

inline Operand maskLiteral(int ones, int width) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type.width = width;
    operand.constant.width = width;
    operand.constant.limbs.assign(limbCount(width), 0);
    for (int bit = 0; bit < ones && bit < width; ++bit) {
        operand.constant.limbs[static_cast<std::size_t>(bit / 64)] |= (1ULL << (bit % 64));
    }
    trim(operand.constant.limbs, width);
    operand.text = "mask_" + std::to_string(ones);
    return operand;
}

inline Operand zeroLiteral(const ValueType& type) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type = type;
    operand.constant.width = widthOf(type);
    operand.constant.limbs.assign(limbCount(operand.constant.width), 0);
    operand.text = "0";
    return operand;
}

inline void clearOperationShape(Operation& op) {
    op.op = OpCode::None;
    op.operands.clear();
    op.to_width = 0;
    op.hi = -1;
    op.lo = -1;
    op.bit = -1;
    op.times = 0;
}

inline void setAssign(Operation& op, Operand operand, const ValueType& type, const std::string& reason) {
    clearOperationShape(op);
    op.kind = OperationKind::Assign;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.debug = generatedDebug(reason, op.operands);
}

inline void setResize(Operation& op,
                      OperationKind kind,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason) {
    clearOperationShape(op);
    op.kind = kind;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.to_width = widthOf(type);
    op.debug = generatedDebug(reason, op.operands);
}

inline void setSlice(Operation& op, Operand operand, int lo, int width, const ValueType& type, const std::string& reason) {
    clearOperationShape(op);
    op.kind = OperationKind::Slice;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.lo = lo;
    op.hi = lo + width - 1;
    op.debug = generatedDebug(reason, op.operands);
}

inline void setBitSelect(Operation& op, Operand operand, int bit, const ValueType& type, const std::string& reason) {
    clearOperationShape(op);
    op.kind = OperationKind::BitSelect;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.bit = bit;
    op.debug = generatedDebug(reason, op.operands);
}

inline void setRepeat(Operation& op, Operand operand, int times, const ValueType& type, const std::string& reason) {
    clearOperationShape(op);
    op.kind = OperationKind::Repeat;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.times = times;
    op.debug = generatedDebug(reason, op.operands);
}

inline void setBinary(Operation& op,
                      OpCode opcode,
                      Operand lhs,
                      Operand rhs,
                      const ValueType& type,
                      const std::string& reason) {
    clearOperationShape(op);
    op.kind = OperationKind::Binary;
    op.op = opcode;
    op.operands.push_back(std::move(lhs));
    op.operands.push_back(std::move(rhs));
    op.type = type;
    op.debug = generatedDebug(reason, op.operands);
}

inline void setSelect(Operation& op, Operand operand, int lo, int width, const ValueType& type, const std::string& reason) {
    int operand_width = widthOf(operand.type);
    if (lo == 0 && width == operand_width) {
        setAssign(op, std::move(operand), type, reason);
    } else if (lo == 0) {
        setResize(op, OperationKind::Trunc, std::move(operand), type, reason);
    } else {
        setSlice(op, std::move(operand), lo, width, type, reason);
    }
}

inline bool isResizeLike(OperationKind kind) {
    return kind == OperationKind::Cast ||
           kind == OperationKind::ZExt ||
           kind == OperationKind::SExt ||
           kind == OperationKind::Trunc;
}

inline bool isZeroExtLike(OperationKind kind) {
    return kind == OperationKind::Cast || kind == OperationKind::ZExt;
}

inline bool selectedRange(const Operation& op, int& lo, int& width) {
    if (op.operands.size() != 1) return false;
    if (op.kind == OperationKind::Trunc) {
        lo = 0;
        width = widthOf(op.type);
        return true;
    }
    if (op.kind == OperationKind::Slice) {
        lo = op.lo;
        width = widthOf(op.type);
        return lo >= 0 && width > 0;
    }
    return false;
}

inline std::string makeTempName(const Program& program) {
    std::unordered_set<std::string> names;
    for (const auto& signal : program.signals) names.insert(signal.name);
    for (std::size_t i = program.signals.size();; ++i) {
        std::string name = "__beopt_width_" + std::to_string(i);
        if (!names.count(name)) return name;
    }
}

inline Operand appendTemp(Program& program, ValueType type, Operation driver, std::string reason) {
    Signal signal;
    signal.id = static_cast<NodeId>(program.signals.size());
    signal.name = makeTempName(program);
    signal.type = type;
    driver.type = type;
    if (driver.debug.reason.empty()) driver.debug = generatedDebug(reason, driver.operands);
    signal.debug = driver.debug;
    signal.driver = std::move(driver);
    program.signals.push_back(std::move(signal));

    Operand operand;
    operand.kind = OperandKind::Symbol;
    operand.node = program.signals.back().id;
    operand.text = program.signals.back().name;
    operand.type = type;
    return operand;
}

inline Operand appendBinaryTemp(Program& program,
                                OpCode opcode,
                                Operand lhs,
                                Operand rhs,
                                ValueType type,
                                std::string reason) {
    Operation op;
    setBinary(op, opcode, std::move(lhs), std::move(rhs), type, reason);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendResizeTemp(Program& program,
                                Operand operand,
                                ValueType type,
                                OperationKind kind,
                                std::string reason) {
    Operation op;
    setResize(op, kind, std::move(operand), type, reason);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendBitSelectTemp(Program& program, Operand operand, int bit, ValueType type, std::string reason) {
    Operation op;
    setBitSelect(op, std::move(operand), bit, type, reason);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendWidthNormalizedTemp(Program& program, Operand operand, ValueType type, std::string reason) {
    int operand_width = widthOf(operand.type);
    int target_width = widthOf(type);
    if (operand_width == target_width) return operand;
    OperationKind kind = operand_width < target_width ? OperationKind::ZExt : OperationKind::Trunc;
    return appendResizeTemp(program, std::move(operand), type, kind, std::move(reason));
}

inline bool rewriteIdentity(Operation& op) {
    if (op.operands.size() != 1) return false;
    const int out_width = widthOf(op.type);
    const int in_width = widthOf(op.operands[0].type);
    if ((op.kind == OperationKind::Cast ||
         op.kind == OperationKind::ZExt ||
         op.kind == OperationKind::SExt ||
         op.kind == OperationKind::Trunc) &&
        out_width == in_width) {
        setAssign(op, op.operands[0], op.type, "removed identity width operation");
        return true;
    }
    if (op.kind == OperationKind::Slice && op.lo == 0 && out_width == in_width) {
        setAssign(op, op.operands[0], op.type, "removed full-width slice");
        return true;
    }
    return false;
}

inline bool rewriteSelectAfterWiden(Operation& op, Program& program) {
    int lo = 0;
    int out_width = 0;
    if (!selectedRange(op, lo, out_width)) return false;
    Operand widened = op.operands[0];
    if (widened.kind != OperandKind::Symbol) return false;
    const Signal* widened_signal = program.findSignal(widened.node);
    if (!widened_signal || !widened_signal->driver || widened_signal->driver->operands.size() != 1) return false;

    const Operation& widen_op = *widened_signal->driver;
    if (!isResizeLike(widen_op.kind) || widen_op.kind == OperationKind::Trunc) return false;
    Operand inner = widen_op.operands[0];
    int inner_width = widthOf(inner.type);
    int hi = lo + out_width - 1;

    if (hi < inner_width) {
        setSelect(op, std::move(inner), lo, out_width, op.type,
                  "folded select of widened value into original operand select");
        return true;
    }

    if (lo == 0) {
        OperationKind replacement = widen_op.kind == OperationKind::SExt ? OperationKind::SExt : OperationKind::ZExt;
        setResize(op, replacement, std::move(inner), op.type,
                  "folded truncate of widened value into narrower extension");
        return true;
    }

    if (lo >= inner_width) {
        if (isZeroExtLike(widen_op.kind)) {
            setAssign(op, zeroLiteral(op.type), op.type, "folded high zero slice of zero-extended value");
            return true;
        }
        if (widen_op.kind == OperationKind::SExt) {
            ValueType bit_type{1, {}};
            Operand sign_bit = appendBitSelectTemp(program, std::move(inner), inner_width - 1, bit_type,
                                                   "extracted sign bit for high slice of sign-extended value");
            if (out_width == 1) {
                setAssign(op, std::move(sign_bit), op.type,
                          "folded one-bit high slice of sign-extended value");
            } else {
                setRepeat(op, std::move(sign_bit), out_width, op.type,
                          "folded high slice of sign-extended value into sign-bit repeat");
            }
            return true;
        }
    }

    if (lo > 0) {
        if (widen_op.kind == OperationKind::SExt) inner.signed_view = true;
        setBinary(op, OpCode::Shr, std::move(inner), literal(static_cast<std::uint64_t>(lo), 32), op.type,
                  "folded slice of widened value into shift");
        return true;
    }

    return false;
}

inline bool rewriteWidenAfterSelect(Operation& op, Program& program) {
    if (!(op.kind == OperationKind::Cast || op.kind == OperationKind::ZExt || op.kind == OperationKind::SExt) ||
        op.operands.size() != 1) {
        return false;
    }
    Operand selected = op.operands[0];
    if (selected.kind != OperandKind::Symbol) return false;
    const Signal* selected_signal = program.findSignal(selected.node);
    if (!selected_signal || !selected_signal->driver) return false;

    const Operation& select_op = *selected_signal->driver;
    int lo = 0;
    int selected_width = 0;
    if (!selectedRange(select_op, lo, selected_width)) return false;
    Operand inner = select_op.operands[0];
    int out_width = widthOf(op.type);

    if (isZeroExtLike(op.kind)) {
        Operand shifted = inner;
        if (lo > 0) {
            shifted = appendBinaryTemp(program, OpCode::Shr, std::move(inner),
                                       literal(static_cast<std::uint64_t>(lo), 32),
                                       op.type,
                                       "shifted source before zero-ext select simplification");
        } else {
            shifted = appendWidthNormalizedTemp(program, std::move(shifted), op.type,
                                                "normalized source width before zero-ext select simplification");
        }
        if (selected_width >= out_width) {
            setAssign(op, std::move(shifted), op.type,
                      "folded zero extension after full-width select into assignment");
            return true;
        }
        setBinary(op, OpCode::BitAnd, std::move(shifted), maskLiteral(selected_width, out_width), op.type,
                  "folded zero extension after select into mask");
        return true;
    }

    if (op.kind == OperationKind::SExt && out_width > selected_width) {
        Operand shifted = inner;
        if (lo > 0) {
            shifted = appendBinaryTemp(program, OpCode::Shr, std::move(inner),
                                       literal(static_cast<std::uint64_t>(lo), 32),
                                       op.type,
                                       "shifted source before sign-ext select simplification");
        }
        Operand masked = appendBinaryTemp(program, OpCode::BitAnd, std::move(shifted),
                                          maskLiteral(selected_width, out_width),
                                          op.type,
                                          "masked selected bits before sign extension");
        int shift = out_width - selected_width;
        Operand left = appendBinaryTemp(program, OpCode::Shl, std::move(masked),
                                        literal(static_cast<std::uint64_t>(shift), 32),
                                        op.type,
                                        "aligned selected sign bit before sign extension");
        left.signed_view = true;
        setBinary(op, OpCode::Shr, std::move(left), literal(static_cast<std::uint64_t>(shift), 32), op.type,
                  "folded sign extension after select into shifts");
        return true;
    }

    return false;
}

inline bool simplifyWidthOperation(Operation& op, Program& program) {
    return rewriteIdentity(op) ||
           rewriteSelectAfterWiden(op, program) ||
           rewriteWidenAfterSelect(op, program);
}

} // namespace width_detail

inline bool simplifyWidthOperations(MutableProgram& graph) {
    using namespace width_detail;

    graph.ensureValueFacts();
    Program& program = graph.program();
    std::size_t original_count = program.signals.size();
    program.signals.reserve(original_count * 4 + 8);

    bool changed = false;
    for (std::size_t index = 0; index < original_count; ++index) {
        Signal& signal = program.signals[index];
        if (!signal.driver || !signal.value.valid) continue;
        bool signal_changed = simplifyWidthOperation(*signal.driver, program);
        if (signal_changed) signal.debug = signal.driver->debug;
        changed = changed || signal_changed;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
