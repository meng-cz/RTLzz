#pragma once

#include "backend/beopt_width.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>
#include <vector>

namespace pred::beir::opt {
namespace bit_update_detail {

struct Update {
    int lo = 0;
    int hi = -1;
    Operand value;
};

inline std::vector<unsigned> useCounts(
    const MutableProgram& graph,
    const std::unordered_set<NodeId>& live) {
    std::vector<unsigned> counts(graph.program().signals.size(), 0);
    for (const auto& signal : graph.program().signals) {
        if (!live.count(signal.id)) continue;
        if (graph.isObservable(signal)) ++counts[signal.id];
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind == OperandKind::Symbol && operand.node < counts.size()) {
                ++counts[operand.node];
            }
        }
    }
    return counts;
}

inline std::unordered_set<NodeId> reachable(const MutableProgram& graph) {
    std::unordered_set<NodeId> live;
    std::vector<NodeId> pending;
    for (const auto& signal : graph.program().signals) {
        if (graph.isObservable(signal)) pending.push_back(signal.id);
    }
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        if (!live.insert(id).second) continue;
        const auto& signal = graph.program().signal(id);
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind == OperandKind::Symbol) pending.push_back(operand.node);
        }
    }
    return live;
}

inline bool compatibleWrite(const Operation& op, const ValueType& type) {
    return op.kind == OperationKind::WriteSlice &&
           op.operands.size() == 2 &&
           !type.isArray() &&
           op.type.width == type.width &&
           op.type.array_dims == type.array_dims &&
           op.operands[0].type.width == type.width &&
           op.operands[0].type.array_dims == type.array_dims &&
           !op.operands[1].type.isArray() &&
           op.lo >= 0 && op.hi >= op.lo && op.hi < type.width;
}

inline bool transparentAssign(const Operation& op, const ValueType& type) {
    return op.kind == OperationKind::Assign &&
           op.operands.size() == 1 &&
           op.type.width == type.width &&
           op.type.array_dims == type.array_dims &&
           op.operands[0].type.width == type.width &&
           op.operands[0].type.array_dims == type.array_dims;
}

inline Operand resizeValue(Program& program,
                           Operand value,
                           int width,
                           const DebugInfo& debug) {
    if (value.type.width == width && !value.type.isArray()) return value;
    Operation cast;
    cast.kind = OperationKind::Cast;
    cast.type = ValueType{width, {}};
    cast.operands = {std::move(value)};
    cast.debug = debug;
    return width_detail::appendTemp(program, cast.type, std::move(cast),
                                    "resize coalesced bit-range update value");
}

inline Operand slice(Program& program,
                     const Operand& source,
                     int lo,
                     int hi,
                     const DebugInfo& debug) {
    const int width = hi - lo + 1;
    if (lo == 0 && width == source.type.width) return source;
    Operation op;
    op.kind = OperationKind::Slice;
    op.type = ValueType{width, {}};
    op.lo = lo;
    op.hi = hi;
    op.operands = {source};
    op.debug = debug;
    return width_detail::appendTemp(program, op.type, std::move(op),
                                    "coalesced bit-range update piece");
}

struct Piece {
    int lo = 0;
    int hi = -1;
    Operand source;
    int source_lo = 0;
};

inline bool sameSource(const Operand& lhs, const Operand& rhs) {
    if (lhs.kind != rhs.kind || lhs.node != rhs.node || lhs.text != rhs.text ||
        lhs.signed_view != rhs.signed_view || lhs.type.width != rhs.type.width ||
        lhs.type.array_dims != rhs.type.array_dims) return false;
    if (lhs.kind != OperandKind::Literal) return true;
    return lhs.constant.width == rhs.constant.width &&
           lhs.constant.signed_view == rhs.constant.signed_view &&
           lhs.constant.limbs == rhs.constant.limbs;
}

} // namespace bit_update_detail

// Collapse a single-user chain of WriteSlice operations into one flat bit
// composition. Updates are collected newest-first, so overlapping writes keep
// the source-language "last write wins" semantics.
inline bool coalesceBitRangeUpdates(MutableProgram& graph,
                                    unsigned max_updates = 32,
                                    unsigned max_pieces = 64) {
    using namespace bit_update_detail;
    if (max_updates == 0 || max_pieces < 2) return false;
    max_updates = std::min(max_updates, 64u);
    max_pieces = std::min(max_pieces, 128u);

    Program& program = graph.program();
    const std::size_t original_count = program.signals.size();
    const auto live = reachable(graph);
    const auto use_count = useCounts(graph, live);
    const auto order = width_detail::topologicalOrder(program);
    std::unordered_set<NodeId> consumed;
    bool changed = false;

    for (auto reverse = order.rbegin(); reverse != order.rend(); ++reverse) {
        const NodeId root = *reverse;
        if (root >= original_count || !live.count(root) || consumed.count(root)) continue;
        const Signal& root_signal = program.signal(root);
        if (!root_signal.driver ||
            !compatibleWrite(*root_signal.driver, root_signal.type)) continue;

        const ValueType type = root_signal.type;
        DebugInfo debug = root_signal.driver->debug;
        std::vector<Update> updates;
        std::vector<NodeId> chain;
        Operand base;
        NodeId current = root;

        while (true) {
            const Operation op = *program.signal(current).driver;
            if (!compatibleWrite(op, type)) break;
            updates.push_back(Update{op.lo, op.hi, op.operands[1]});
            chain.push_back(current);
            base = op.operands[0];
            if (updates.size() >= max_updates) break;

            // Frontend lowering commonly materializes each assignment through
            // one or more same-width Assign nodes. They are bit-preserving, so
            // a private alias may be bypassed while looking for the preceding
            // WriteSlice in the update chain.
            bool found_previous_write = false;
            while (base.kind == OperandKind::Symbol &&
                   base.node < original_count &&
                   use_count[base.node] == 1 &&
                   !consumed.count(base.node)) {
                const Signal* previous = program.findSignal(base.node);
                if (!previous || !previous->driver) break;
                if (compatibleWrite(*previous->driver, type)) {
                    current = base.node;
                    found_previous_write = true;
                    break;
                }
                if (!transparentAssign(*previous->driver, type)) break;
                chain.push_back(base.node);
                base = previous->driver->operands[0];
            }
            if (!found_previous_write) break;
        }

        std::set<int> boundaries{0, type.width};
        for (const auto& update : updates) {
            boundaries.insert(update.lo);
            boundaries.insert(update.hi + 1);
        }
        if (boundaries.size() < 2 || boundaries.size() - 1 > max_pieces) continue;

        // Do not append casts until the size guards have accepted this chain:
        // a rejected candidate must leave the graph completely unchanged.
        for (auto& update : updates) {
            update.value = resizeValue(program, std::move(update.value),
                                       update.hi - update.lo + 1, debug);
        }

        std::vector<int> points(boundaries.begin(), boundaries.end());
        std::vector<Piece> pieces;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            const int lo = points[i];
            const int hi = points[i + 1] - 1;
            const Update* owner = nullptr;
            for (const auto& update : updates) {
                if (update.lo <= lo && update.hi >= hi) {
                    owner = &update;
                    break;
                }
            }
            Piece piece;
            piece.lo = lo;
            piece.hi = hi;
            if (owner) {
                piece.source = owner->value;
                piece.source_lo = lo - owner->lo;
            } else {
                piece.source = base;
                piece.source_lo = lo;
            }
            if (!pieces.empty()) {
                Piece& previous = pieces.back();
                const int previous_width = previous.hi - previous.lo + 1;
                if (previous.hi + 1 == piece.lo &&
                    sameSource(previous.source, piece.source) &&
                    previous.source_lo + previous_width == piece.source_lo) {
                    previous.hi = piece.hi;
                    continue;
                }
            }
            pieces.push_back(std::move(piece));
        }
        if (pieces.size() > max_pieces) continue;

        std::vector<Operand> operands;
        operands.reserve(pieces.size());
        for (auto piece = pieces.rbegin(); piece != pieces.rend(); ++piece) {
            const int width = piece->hi - piece->lo + 1;
            operands.push_back(slice(program, piece->source, piece->source_lo,
                                     piece->source_lo + width - 1, debug));
        }

        Operation composed;
        composed.kind = operands.size() == 1 ? OperationKind::Assign
                                             : OperationKind::Concat;
        composed.type = type;
        composed.operands = std::move(operands);
        composed.debug = debug;
        addDebugMessage(composed.debug,
                        updates.size() > 1
                            ? "coalesced bit-range update chain"
                            : "lowered static bit-range update");
        program.signal(root).driver = std::move(composed);
        consumed.insert(chain.begin(), chain.end());
        changed = true;
    }

    if (changed) graph.markValueFactsDirty();
    return changed;
}

} // namespace pred::beir::opt
