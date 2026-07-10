#pragma once

#include "backend/beir.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
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

inline bool getBit(const std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return false;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    if (limb >= limbs.size()) return false;
    return ((limbs[limb] >> (bit % 64)) & 1ULL) != 0;
}

inline DebugInfo generatedDebug(std::string reason,
                                const std::vector<Operand>& operands = {},
                                const Program* program = nullptr,
                                const std::vector<DebugLoc>& inherited_locs = {}) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Generated;
    debug.reason = std::move(reason);
    addDebugLocs(debug, inherited_locs);
    for (const auto& operand : operands) {
        if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
            debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            debug.derived_names.push_back(operand.text);
        }
    }
    if (program) addOperandDebugLocs(debug, *program, operands);
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

inline bool literalU64(const Operand& operand, std::uint64_t& out) {
    if (operand.kind != OperandKind::Literal || !operand.constant.fitsU64()) return false;
    out = operand.constant.toU64();
    return true;
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

inline std::vector<DebugLoc> inheritedLocs(const Operation& op) {
    std::vector<DebugLoc> locs = op.source_locs;
    locs.insert(locs.end(), op.debug.source_locs.begin(), op.debug.source_locs.end());
    return locs;
}

inline void finishDebug(Operation& op, const std::string& reason, const Program& program, std::vector<DebugLoc> locs) {
    op.debug = generatedDebug(reason, op.operands, &program, locs);
    op.source_locs = op.debug.source_locs;
}

inline void setAssign(Operation& op,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = OperationKind::Assign;
    op.operands.push_back(std::move(operand));
    op.type = type;
    finishDebug(op, reason, program, std::move(locs));
}

inline void setResize(Operation& op,
                      OperationKind kind,
                      Operand operand,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = kind;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.to_width = widthOf(type);
    finishDebug(op, reason, program, std::move(locs));
}

inline void setSlice(Operation& op,
                     Operand operand,
                     int lo,
                     int width,
                     const ValueType& type,
                     const std::string& reason,
                     const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = OperationKind::Slice;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.lo = lo;
    op.hi = lo + width - 1;
    finishDebug(op, reason, program, std::move(locs));
}

inline void setBitSelect(Operation& op,
                         Operand operand,
                         int bit,
                         const ValueType& type,
                         const std::string& reason,
                         const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = OperationKind::BitSelect;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.bit = bit;
    finishDebug(op, reason, program, std::move(locs));
}

inline void setRepeat(Operation& op,
                      Operand operand,
                      int times,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = OperationKind::Repeat;
    op.operands.push_back(std::move(operand));
    op.type = type;
    op.times = times;
    finishDebug(op, reason, program, std::move(locs));
}

inline void setBinary(Operation& op,
                      OpCode opcode,
                      Operand lhs,
                      Operand rhs,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    std::vector<DebugLoc> locs = inheritedLocs(op);
    clearOperationShape(op);
    op.kind = OperationKind::Binary;
    op.op = opcode;
    op.operands.push_back(std::move(lhs));
    op.operands.push_back(std::move(rhs));
    op.type = type;
    finishDebug(op, reason, program, std::move(locs));
}

inline void setSelect(Operation& op,
                      Operand operand,
                      int lo,
                      int width,
                      const ValueType& type,
                      const std::string& reason,
                      const Program& program) {
    int operand_width = widthOf(operand.type);
    if (lo == 0 && width == operand_width) {
        setAssign(op, std::move(operand), type, reason, program);
    } else if (lo == 0) {
        setResize(op, OperationKind::Trunc, std::move(operand), type, reason, program);
    } else {
        setSlice(op, std::move(operand), lo, width, type, reason, program);
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
    if (driver.debug.reason.empty()) driver.debug = generatedDebug(reason, driver.operands, &program);
    driver.source_locs = driver.debug.source_locs;
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
    setBinary(op, opcode, std::move(lhs), std::move(rhs), type, reason, program);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendResizeTemp(Program& program,
                                Operand operand,
                                ValueType type,
                                OperationKind kind,
                                std::string reason) {
    Operation op;
    setResize(op, kind, std::move(operand), type, reason, program);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendBitSelectTemp(Program& program, Operand operand, int bit, ValueType type, std::string reason) {
    Operation op;
    setBitSelect(op, std::move(operand), bit, type, reason, program);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendSliceTemp(Program& program,
                               Operand operand,
                               int lo,
                               int width,
                               ValueType type,
                               std::string reason) {
    Operation op;
    setSlice(op, std::move(operand), lo, width, type, reason, program);
    return appendTemp(program, type, std::move(op), reason);
}

inline Operand appendWidthNormalizedTemp(Program& program, Operand operand, ValueType type, std::string reason) {
    int operand_width = widthOf(operand.type);
    int target_width = widthOf(type);
    if (operand_width == target_width) return operand;
    OperationKind kind = operand_width < target_width ? OperationKind::ZExt : OperationKind::Trunc;
    return appendResizeTemp(program, std::move(operand), type, kind, std::move(reason));
}

inline Operand appendTruncTemp(Program& program, Operand operand, int width) {
    bool signed_view = operand.signed_view;
    ValueType type = operand.type;
    type.width = width;
    Operation op;
    setResize(op, OperationKind::Trunc, std::move(operand), type,
              "inserted truncation for comprehensive width optimization", program);
    Operand out = appendTemp(program, type, std::move(op), "inserted truncation for comprehensive width optimization");
    out.signed_view = signed_view;
    return out;
}

inline Operand appendZExtTemp(Program& program, Operand operand, int width) {
    bool signed_view = operand.signed_view;
    ValueType type = operand.type;
    type.width = width;
    Operation op;
    setResize(op, OperationKind::ZExt, std::move(operand), type,
              "inserted zero extension for comprehensive width optimization", program);
    Operand out = appendTemp(program, type, std::move(op), "inserted zero extension for comprehensive width optimization");
    out.signed_view = signed_view;
    return out;
}

inline bool rewriteIdentity(Operation& op, Program& program) {
    if (op.operands.size() != 1) return false;
    const int out_width = widthOf(op.type);
    const int in_width = widthOf(op.operands[0].type);
    if ((op.kind == OperationKind::Cast ||
         op.kind == OperationKind::ZExt ||
         op.kind == OperationKind::SExt ||
         op.kind == OperationKind::Trunc) &&
        out_width == in_width) {
        setAssign(op, op.operands[0], op.type, "removed identity width operation", program);
        return true;
    }
    if (op.kind == OperationKind::Slice && op.lo == 0 && out_width == in_width) {
        setAssign(op, op.operands[0], op.type, "removed full-width slice", program);
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
                  "folded select of widened value into original operand select", program);
        return true;
    }

    if (lo == 0) {
        OperationKind replacement = widen_op.kind == OperationKind::SExt ? OperationKind::SExt : OperationKind::ZExt;
        setResize(op, replacement, std::move(inner), op.type,
                  "folded truncate of widened value into narrower extension", program);
        return true;
    }

    if (lo >= inner_width) {
        if (isZeroExtLike(widen_op.kind)) {
            setAssign(op, zeroLiteral(op.type), op.type,
                      "folded high zero slice of zero-extended value", program);
            return true;
        }
        if (widen_op.kind == OperationKind::SExt) {
            ValueType bit_type{1, {}};
            Operand sign_bit = appendBitSelectTemp(program, std::move(inner), inner_width - 1, bit_type,
                                                   "extracted sign bit for high slice of sign-extended value");
            if (out_width == 1) {
                setAssign(op, std::move(sign_bit), op.type,
                          "folded one-bit high slice of sign-extended value", program);
            } else {
                setRepeat(op, std::move(sign_bit), out_width, op.type,
                          "folded high slice of sign-extended value into sign-bit repeat", program);
            }
            return true;
        }
    }

    if (lo > 0) {
        if (widen_op.kind == OperationKind::SExt) inner.signed_view = true;
        setBinary(op, OpCode::Shr, std::move(inner), literal(static_cast<std::uint64_t>(lo), 32), op.type,
                  "folded slice of widened value into shift", program);
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
            int inner_width = widthOf(inner.type);
            if (!inner.signed_view && !inner.constant.signed_view &&
                out_width > 0 && inner_width > 0) {
                if (lo >= inner_width) {
                    shifted = zeroLiteral(op.type);
                } else {
                    int slice_width = std::min(out_width, inner_width - lo);
                    ValueType slice_type = op.type;
                    slice_type.width = slice_width;
                    shifted = appendSliceTemp(program, std::move(inner), lo, slice_width, slice_type,
                                              "sliced source before zero-ext select simplification");
                    if (slice_width < out_width) {
                        shifted = appendZExtTemp(program, std::move(shifted), out_width);
                    }
                }
            } else {
                shifted = appendBinaryTemp(program, OpCode::Shr, std::move(inner),
                                           literal(static_cast<std::uint64_t>(lo), 32),
                                           op.type,
                                           "shifted source before zero-ext select simplification");
            }
        } else {
            shifted = appendWidthNormalizedTemp(program, std::move(shifted), op.type,
                                                "normalized source width before zero-ext select simplification");
        }
        if (selected_width >= out_width) {
            setAssign(op, std::move(shifted), op.type,
                      "folded zero extension after full-width select into assignment", program);
            return true;
        }
        setBinary(op, OpCode::BitAnd, std::move(shifted), maskLiteral(selected_width, out_width), op.type,
                  "folded zero extension after select into mask", program);
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
                  "folded sign extension after select into shifts", program);
        return true;
    }

    return false;
}

inline bool simplifyWidthOperation(Operation& op, Program& program) {
    return rewriteIdentity(op, program) ||
           rewriteSelectAfterWiden(op, program) ||
           rewriteWidenAfterSelect(op, program);
}

inline bool lowBitClosedBinary(OpCode op) {
    return op == OpCode::Add ||
           op == OpCode::Sub ||
           op == OpCode::Mul ||
           op == OpCode::BitAnd ||
           op == OpCode::BitOr ||
           op == OpCode::BitXor;
}

inline bool narrowableOperation(const Operation& op) {
    switch (op.kind) {
    case OperationKind::Assign:
    case OperationKind::Cast:
    case OperationKind::ZExt:
    case OperationKind::SExt:
    case OperationKind::Trunc:
    case OperationKind::Slice:
    case OperationKind::Ite:
        return true;
    case OperationKind::Unary:
        return op.op == OpCode::BitNot || op.op == OpCode::Neg;
    case OperationKind::Binary:
        return lowBitClosedBinary(op.op) || op.op == OpCode::Shl || op.op == OpCode::Shr;
    default:
        return false;
    }
}

inline std::vector<NodeId> topologicalOrder(const Program& program) {
    std::vector<std::vector<NodeId>> users(program.signals.size());
    std::vector<std::size_t> indegree(program.signals.size(), 0);
    for (const auto& signal : program.signals) {
        if (signal.id >= program.signals.size()) {
            throw std::runtime_error("BEIR width optimization requires dense NodeId indices");
        }
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind != OperandKind::Symbol) continue;
            if (operand.node >= program.signals.size() || !program.findSignal(operand.node)) {
                throw std::runtime_error("BEIR width optimization found dependency on unknown node");
            }
            users[operand.node].push_back(signal.id);
            ++indegree[signal.id];
        }
    }

    std::queue<NodeId> ready;
    for (const auto& signal : program.signals) {
        if (indegree[signal.id] == 0) ready.push(signal.id);
    }

    std::vector<NodeId> order;
    order.reserve(program.signals.size());
    while (!ready.empty()) {
        NodeId id = ready.front();
        ready.pop();
        order.push_back(id);
        for (NodeId user : users[id]) {
            if (--indegree[user] == 0) ready.push(user);
        }
    }
    if (order.size() != program.signals.size()) {
        throw std::runtime_error("BEIR width optimization requires an acyclic signal dependency graph");
    }
    return order;
}

inline int factsEffectiveWidth(const Signal& signal) {
    int declared = widthOf(signal.type);
    if (!signal.value.valid || signal.value.width <= 0) return declared;
    int width = std::min(declared, signal.value.width);
    for (int bit = width - 1; bit >= 0; --bit) {
        if (!getBit(signal.value.known_zero, bit)) return bit + 1;
    }
    return 1;
}

inline int literalEffectiveWidth(const Operand& operand) {
    int width = widthOf(operand.type);
    if (operand.kind != OperandKind::Literal) return width;
    for (int bit = width - 1; bit >= 0; --bit) {
        if (getBit(operand.constant.limbs, bit)) return bit + 1;
    }
    return 1;
}

inline int operandAssignedWidth(const Program& program, const Operand& operand, const std::vector<int>& assigned) {
    int declared = widthOf(operand.type);
    if (operand.kind == OperandKind::Literal) return std::min(declared, literalEffectiveWidth(operand));
    if (operand.kind != OperandKind::Symbol || operand.node >= assigned.size()) return declared;
    if (const Signal* signal = program.findSignal(operand.node)) {
        declared = widthOf(signal->type);
    }
    int known = assigned[operand.node] > 0 ? assigned[operand.node] : declared;
    return std::min(declared, std::max(1, known));
}

inline int assignedWidthFromOperation(const Program& program, const Operation& op, const std::vector<int>& assigned) {
    int out_width = widthOf(op.type);
    auto operand_width = [&](std::size_t index) {
        if (index >= op.operands.size()) return out_width;
        return operandAssignedWidth(program, op.operands[index], assigned);
    };

    int computed = out_width;
    switch (op.kind) {
    case OperationKind::Assign:
    case OperationKind::Cast:
    case OperationKind::ZExt:
    case OperationKind::Trunc:
        computed = operand_width(0);
        break;
    case OperationKind::SExt:
        computed = out_width;
        break;
    case OperationKind::Slice:
        computed = std::max(1, operand_width(0) - std::max(0, op.lo));
        break;
    case OperationKind::BitSelect:
        computed = 1;
        break;
    case OperationKind::Ite:
        computed = std::max(operand_width(1), operand_width(2));
        break;
    case OperationKind::Unary:
        if (op.op == OpCode::LogicNot) computed = 1;
        else computed = out_width;
        break;
    case OperationKind::Binary: {
        int lhs = operand_width(0);
        int rhs = operand_width(1);
        if (op.op == OpCode::Add) computed = std::max(lhs, rhs) + 1;
        else if (op.op == OpCode::Sub) computed = out_width;
        else if (op.op == OpCode::Mul) computed = lhs + rhs;
        else if (op.op == OpCode::BitAnd) computed = std::min(lhs, rhs);
        else if (op.op == OpCode::BitOr || op.op == OpCode::BitXor) computed = std::max(lhs, rhs);
        else if (op.op == OpCode::LogicAnd || op.op == OpCode::LogicOr ||
                 op.op == OpCode::Eq || op.op == OpCode::Ne ||
                 op.op == OpCode::Lt || op.op == OpCode::Le ||
                 op.op == OpCode::Gt || op.op == OpCode::Ge) {
            computed = 1;
        } else if (op.op == OpCode::Shl && op.operands.size() >= 2) {
            std::uint64_t amount = 0;
            computed = literalU64(op.operands[1], amount) &&
                       amount <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                ? lhs + static_cast<int>(amount)
                : out_width;
        } else if (op.op == OpCode::Shr && op.operands.size() >= 2) {
            std::uint64_t amount = 0;
            computed = literalU64(op.operands[1], amount) &&
                       amount <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                ? std::max(1, lhs - static_cast<int>(amount))
                : out_width;
        }
        break;
    }
    case OperationKind::Concat: {
        int total = 0;
        for (const auto& operand : op.operands) total += widthOf(operand.type);
        computed = total;
        break;
    }
    case OperationKind::Repeat:
        computed = op.operands.empty() ? out_width : operand_width(0) * std::max(0, op.times);
        break;
    case OperationKind::ReduceOr:
    case OperationKind::ReduceAnd:
    case OperationKind::ReduceXor:
        computed = 1;
        break;
    default:
        computed = out_width;
        break;
    }
    return std::max(1, std::min(out_width, computed));
}

inline std::vector<int> analyzeAssignedWidths(const Program& program, const std::vector<NodeId>& order) {
    std::vector<int> assigned(program.signals.size(), 0);
    for (NodeId id : order) {
        const Signal& signal = program.signal(id);
        int width = widthOf(signal.type);
        if (signal.driver && narrowableOperation(*signal.driver)) {
            width = assignedWidthFromOperation(program, *signal.driver, assigned);
        }
        width = std::min(width, factsEffectiveWidth(signal));
        assigned[id] = std::max(1, width);
    }
    return assigned;
}

inline void demandOperand(const Program& program,
                          const Operand& operand,
                          int needed,
                          const std::vector<int>& assigned,
                          std::vector<int>& demand) {
    if (needed <= 0 || operand.kind != OperandKind::Symbol || operand.node >= demand.size()) return;
    int operand_width = operandAssignedWidth(program, operand, assigned);
    demand[operand.node] = std::max(demand[operand.node], std::min(operand_width, needed));
}

inline void propagateDemand(const Program& program,
                            const Operation& op,
                            int out_demand,
                            const std::vector<int>& assigned,
                            std::vector<int>& demand) {
    if (out_demand <= 0) return;
    int need = std::min(out_demand, widthOf(op.type));
    auto full = [&](std::size_t index) {
        if (index < op.operands.size()) {
            demandOperand(program, op.operands[index], operandAssignedWidth(program, op.operands[index], assigned),
                          assigned, demand);
        }
    };
    auto low = [&](std::size_t index, int bits) {
        if (index < op.operands.size()) demandOperand(program, op.operands[index], bits, assigned, demand);
    };

    switch (op.kind) {
    case OperationKind::Assign:
    case OperationKind::Cast:
    case OperationKind::ZExt:
    case OperationKind::Trunc:
        low(0, need);
        return;
    case OperationKind::SExt:
        if (!op.operands.empty()) low(0, std::min(need, operandAssignedWidth(program, op.operands[0], assigned)));
        return;
    case OperationKind::Slice:
        low(0, op.lo + need);
        return;
    case OperationKind::BitSelect:
        low(0, op.bit + 1);
        return;
    case OperationKind::Ite:
        full(0);
        low(1, need);
        low(2, need);
        return;
    case OperationKind::Unary:
        if (op.op == OpCode::BitNot || op.op == OpCode::Neg) low(0, need);
        else full(0);
        return;
    case OperationKind::Binary:
        if (op.op == OpCode::Add || op.op == OpCode::Sub || op.op == OpCode::Mul ||
            op.op == OpCode::BitAnd || op.op == OpCode::BitOr || op.op == OpCode::BitXor) {
            low(0, need);
            low(1, need);
            return;
        }
        if (op.op == OpCode::Shl && op.operands.size() >= 2) {
            std::uint64_t amount = 0;
            if (literalU64(op.operands[1], amount) &&
                amount <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                int shift = static_cast<int>(amount);
                low(0, shift >= need ? 1 : need - shift);
                full(1);
                return;
            }
        }
        if (op.op == OpCode::Shr && op.operands.size() >= 2) {
            std::uint64_t amount = 0;
            if (literalU64(op.operands[1], amount) &&
                amount <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                low(0, need + static_cast<int>(amount));
                full(1);
                return;
            }
        }
        full(0);
        full(1);
        return;
    case OperationKind::Concat: {
        int remaining = need;
        for (std::size_t reverse = op.operands.size(); reverse-- > 0 && remaining > 0;) {
            int part_width = widthOf(op.operands[reverse].type);
            demandOperand(program, op.operands[reverse], std::min(part_width, remaining), assigned, demand);
            remaining -= part_width;
        }
        return;
    }
    case OperationKind::Repeat:
        low(0, need);
        return;
    default:
        for (std::size_t i = 0; i < op.operands.size(); ++i) full(i);
        return;
    }
}

inline std::vector<int> analyzeDemandWidths(MutableProgram& graph,
                                            const std::vector<NodeId>& order,
                                            const std::vector<int>& assigned) {
    const Program& program = graph.program();
    std::vector<int> demand(program.signals.size(), 0);
    for (const auto& signal : program.signals) {
        if (graph.isObservable(signal)) demand[signal.id] = widthOf(signal.type);
    }
    for (std::size_t index = order.size(); index-- > 0;) {
        NodeId id = order[index];
        const Signal& signal = program.signal(id);
        if (!signal.driver || demand[id] <= 0) continue;
        propagateDemand(program, *signal.driver, demand[id], assigned, demand);
    }
    return demand;
}

inline Operand resizeOperandForUse(Program& program, Operand operand, int width) {
    if (operand.kind == OperandKind::Symbol) {
        if (const Signal* signal = program.findSignal(operand.node)) operand.type = signal->type;
    }
    int current = widthOf(operand.type);
    width = std::max(1, width);
    if (current == width) return operand;
    if (operand.kind == OperandKind::Literal) {
        operand.type.width = width;
        operand.constant.width = width;
        trim(operand.constant.limbs, width);
        return operand;
    }
    if (operand.kind != OperandKind::Symbol) return operand;
    return current > width
        ? appendTruncTemp(program, std::move(operand), width)
        : appendZExtTemp(program, std::move(operand), width);
}

inline void narrowOperationResult(Operation& op, int width) {
    op.type.width = width;
    if (op.kind == OperationKind::Cast ||
        op.kind == OperationKind::ZExt ||
        op.kind == OperationKind::SExt ||
        op.kind == OperationKind::Trunc) {
        op.to_width = width;
    } else if (op.kind == OperationKind::Slice) {
        op.hi = op.lo + width - 1;
    }
}

inline bool rewriteNarrowedLogicalShrToSlice(Operation& op,
                                             int width,
                                             const Program& program) {
    if (op.kind != OperationKind::Binary || op.op != OpCode::Shr ||
        op.operands.size() < 2) {
        return false;
    }
    const Operand lhs = op.operands[0];
    if (lhs.signed_view || lhs.constant.signed_view) return false;
    std::uint64_t amount = 0;
    if (!literalU64(op.operands[1], amount) ||
        amount > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    int lo = static_cast<int>(amount);
    int lhs_width = widthOf(lhs.type);
    if (width <= 0 || lhs_width <= 0 || lo < 0 || lo + width > lhs_width) {
        return false;
    }
    setSlice(op, lhs, lo, width, op.type,
             "narrowed logical right shift into slice", program);
    return true;
}

inline void normalizeOperationOperands(Operation& op, Program& program, const std::vector<int>& assigned) {
    int out_width = widthOf(op.type);
    auto assigned_operand = [&](std::size_t index) {
        return index < op.operands.size() ? operandAssignedWidth(program, op.operands[index], assigned) : out_width;
    };

    if (op.kind == OperationKind::Binary && op.operands.size() >= 2) {
        if (op.op == OpCode::Add || op.op == OpCode::Sub) {
            int common = std::max(assigned_operand(0), assigned_operand(1));
            op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), common);
            op.operands[1] = resizeOperandForUse(program, std::move(op.operands[1]), common);
        } else if (lowBitClosedBinary(op.op)) {
            op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), out_width);
            op.operands[1] = resizeOperandForUse(program, std::move(op.operands[1]), out_width);
        } else if (op.op == OpCode::Eq || op.op == OpCode::Ne ||
                   op.op == OpCode::Lt || op.op == OpCode::Le ||
                   op.op == OpCode::Gt || op.op == OpCode::Ge) {
            int common = std::max(widthOf(op.operands[0].type), widthOf(op.operands[1].type));
            op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), common);
            op.operands[1] = resizeOperandForUse(program, std::move(op.operands[1]), common);
        }
    } else if (op.kind == OperationKind::Unary &&
               (op.op == OpCode::BitNot || op.op == OpCode::Neg) &&
               !op.operands.empty()) {
        op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), out_width);
    } else if (op.kind == OperationKind::WriteBit && op.operands.size() >= 2) {
        op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), out_width);
        op.operands[1] = resizeOperandForUse(program, std::move(op.operands[1]), 1);
    } else if (op.kind == OperationKind::WriteSlice && op.operands.size() >= 2) {
        op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), out_width);
        op.operands[1] = resizeOperandForUse(program, std::move(op.operands[1]), op.hi - op.lo + 1);
    } else if ((op.kind == OperationKind::Assign ||
                op.kind == OperationKind::Cast ||
                op.kind == OperationKind::ZExt) &&
               !op.operands.empty()) {
        op.operands[0] = resizeOperandForUse(program, std::move(op.operands[0]), out_width);
    } else {
        for (auto& operand : op.operands) {
            if (operand.kind == OperandKind::Symbol) {
                if (const Signal* signal = program.findSignal(operand.node)) operand.type = signal->type;
            }
        }
    }
}

inline bool applyComprehensiveWidths(MutableProgram& graph,
                                     const std::vector<NodeId>& order,
                                     const std::vector<int>& assigned,
                                     const std::vector<int>& demand) {
    Program& program = graph.program();
    std::size_t original_count = program.signals.size();
    program.signals.reserve(original_count * 3 + 8);
    bool changed = false;

    for (NodeId id : order) {
        if (id >= original_count) continue;
        Signal& signal = program.signal(id);
        if (!signal.driver || signal.type.isArray() || graph.isObservable(signal)) continue;
        int old_width = widthOf(signal.type);
        int assigned_width = id < assigned.size() && assigned[id] > 0 ? assigned[id] : old_width;
        int demand_width = id < demand.size() && demand[id] > 0 ? demand[id] : assigned_width;
        int target = std::max(1, std::min(old_width, std::min(assigned_width, demand_width)));
        if (target >= old_width || !narrowableOperation(*signal.driver)) continue;
        signal.type.width = target;
        if (!rewriteNarrowedLogicalShrToSlice(*signal.driver, target, program)) {
            narrowOperationResult(*signal.driver, target);
        }
        signal.debug = signal.driver->debug = generatedDebug(
            "narrowed signal by comprehensive BEIR width optimization",
            signal.driver->operands,
            &program,
            inheritedLocs(*signal.driver));
        signal.driver->source_locs = signal.driver->debug.source_locs;
        changed = true;
    }

    for (std::size_t index = 0; index < original_count; ++index) {
        Signal& signal = program.signals[index];
        if (!signal.driver) continue;
        std::size_t before = program.signals.size();
        normalizeOperationOperands(*signal.driver, program, assigned);
        if (program.signals.size() != before) changed = true;
    }

    return changed;
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
    if (changed) {
        graph.markValueFactsDirty();
        return true;
    }

    const std::vector<NodeId> order = topologicalOrder(program);
    const std::vector<int> assigned = analyzeAssignedWidths(program, order);
    const std::vector<int> demand = analyzeDemandWidths(graph, order, assigned);
    changed = applyComprehensiveWidths(graph, order, assigned, demand);
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
