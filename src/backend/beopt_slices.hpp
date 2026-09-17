#pragma once

#include "backend/beopt_width.hpp"

namespace pred::beir::opt {

// Run after constant propagation; leave unsupported/out-of-range accesses alone.
inline bool specializeConstantSlices(MutableProgram& graph) {
    auto& program = graph.program();
    bool changed = false;
    const auto count = program.signals.size();
    for (std::size_t id = 0; id < count; ++id) {
        if (!program.signals[id].driver) continue;
        Operation op = *program.signals[id].driver; // appending slices can reallocate signals
        if (op.type.isArray() || op.operands.empty()) continue;
        const int base_width = op.operands[0].type.width;
        if (op.kind == OperationKind::DynamicSlice || op.kind == OperationKind::DynamicWriteSlice) {
            const bool write = op.kind == OperationKind::DynamicWriteSlice;
            if (op.operands.size() != (write ? 3u : 2u)) continue;
            const auto& index = op.operands[1];
            if (index.kind != OperandKind::Literal || !index.constant.fitsU64()) continue;
            const int width = write ? op.operands[2].type.width : op.type.width;
            if (width <= 0 || base_width < width ||
                index.constant.toU64() > static_cast<std::uint64_t>(base_width - width)) continue;
            op.lo = static_cast<int>(index.constant.toU64());
            op.hi = op.lo + width - 1;
            op.kind = write ? OperationKind::WriteSlice : OperationKind::Slice;
            op.operands.erase(op.operands.begin() + 1);
        } else if (op.kind != OperationKind::WriteSlice) {
            continue;
        }
        if (op.kind == OperationKind::WriteSlice) {
            if (op.operands.size() != 2 || op.type.width != base_width ||
                op.lo < 0 || op.hi < op.lo || op.hi >= base_width) continue;
            const int width = op.hi - op.lo + 1;
            auto value = op.operands[1];
            if (value.type.width != width) {
                Operation cast;
                cast.kind = OperationKind::Cast;
                cast.operands = {value};
                value = width_detail::appendTemp(program, ValueType{width, {}}, cast,
                                                 "resize static slice write value");
            }
            std::vector<Operand> parts;
            auto slice = [&](int lo, int hi) {
                Operation part;
                part.kind = OperationKind::Slice;
                part.lo = lo;
                part.hi = hi;
                part.operands = {op.operands[0]};
                part.debug = op.debug;
                parts.push_back(width_detail::appendTemp(program, ValueType{hi - lo + 1, {}},
                                                          part, "static slice write preserved bits"));
            };
            if (op.hi + 1 < base_width) slice(op.hi + 1, base_width - 1);
            parts.push_back(value);
            if (op.lo > 0) slice(0, op.lo - 1);
            op.kind = parts.size() == 1 ? OperationKind::Assign : OperationKind::Concat;
            op.operands = std::move(parts);
            op.hi = op.lo = -1;
        }
        addDebugMessage(op.debug, "specialized constant-index slice access");
        program.signals[id].driver = std::move(op);
        changed = true;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
