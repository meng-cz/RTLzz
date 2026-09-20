#pragma once

#include "backend/beopt_width.hpp"

namespace pred::beir::opt {

// Run after constant propagation; leave unsupported/out-of-range accesses alone.
// Static writes deliberately remain WriteSlice operations here.  Keeping their
// range metadata intact lets BitRangeUpdateCoalescing see and combine a whole
// update chain before it is lowered to slices and concatenation.
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
        } else {
            continue;
        }
        addDebugMessage(op.debug, "specialized constant-index slice access");
        program.signals[id].driver = std::move(op);
        changed = true;
    }
    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
