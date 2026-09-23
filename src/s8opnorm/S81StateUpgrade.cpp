#include "s8opnorm/S81StateUpgrade.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_set>

namespace pred::s81stateupgrade {
namespace {

using BlockId = s8opnorm::BlockId;
using SymbolId = s8opnorm::SymbolId;

std::vector<BlockId> successors(const s8opnorm::S8Terminator& term) {
    std::vector<BlockId> result;
    switch (term.kind) {
    case s8opnorm::S8TermKind::Jump: result.push_back(term.jump_target); break;
    case s8opnorm::S8TermKind::Branch:
        result.push_back(term.true_target); result.push_back(term.false_target); break;
    case s8opnorm::S8TermKind::Switch:
        for (const auto& target : term.switch_targets) result.push_back(target.target);
        result.push_back(term.default_target);
        break;
    case s8opnorm::S8TermKind::Exit:
    case s8opnorm::S8TermKind::Unreachable: break;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool operandIs(const s8opnorm::S8Operand& operand, SymbolId symbol) {
    return operand.kind == s8opnorm::S8OperandKind::Var && operand.symbol == symbol;
}

void renameOperand(s8opnorm::S8Operand& operand, SymbolId from, SymbolId to) {
    if (operandIs(operand, from)) operand.symbol = to;
}

void renameUses(s8opnorm::S8Stmt& stmt, SymbolId from, SymbolId to) {
    switch (stmt.kind) {
    case s8opnorm::S8StmtKind::Assign: renameOperand(stmt.value, from, to); break;
    case s8opnorm::S8StmtKind::Op:
        for (auto& operand : stmt.op.operands) renameOperand(operand, from, to);
        break;
    case s8opnorm::S8StmtKind::Lookup:
        renameOperand(stmt.lookup_index, from, to);
        for (auto& operand : stmt.lookup_elements) renameOperand(operand, from, to);
        break;
    case s8opnorm::S8StmtKind::LookupWrite:
        renameOperand(stmt.lookup_index, from, to);
        renameOperand(stmt.lookup_value, from, to);
        for (auto& operand : stmt.lookup_elements) renameOperand(operand, from, to);
        break;
    }
}

void renameUses(s8opnorm::S8Terminator& term, SymbolId from, SymbolId to) {
    if (term.kind == s8opnorm::S8TermKind::Branch) renameOperand(term.condition, from, to);
    if (term.kind == s8opnorm::S8TermKind::Switch) {
        renameOperand(term.switch_value, from, to);
        for (auto& target : term.switch_targets)
            if (target.value) renameOperand(*target.value, from, to);
    }
}

bool stmtDefines(const s8opnorm::S8Stmt& stmt, SymbolId symbol) {
    if (stmt.kind == s8opnorm::S8StmtKind::LookupWrite)
        return std::find(stmt.lookup_write_targets.begin(), stmt.lookup_write_targets.end(), symbol) !=
               stmt.lookup_write_targets.end();
    return stmt.target == symbol;
}

bool stmtUses(const s8opnorm::S8Stmt& stmt, SymbolId symbol) {
    auto uses = [symbol](const s8opnorm::S8Operand& operand) { return operandIs(operand, symbol); };
    switch (stmt.kind) {
    case s8opnorm::S8StmtKind::Assign: return uses(stmt.value);
    case s8opnorm::S8StmtKind::Op:
        return std::any_of(stmt.op.operands.begin(), stmt.op.operands.end(), uses);
    case s8opnorm::S8StmtKind::Lookup:
        return uses(stmt.lookup_index) || std::any_of(stmt.lookup_elements.begin(), stmt.lookup_elements.end(), uses);
    case s8opnorm::S8StmtKind::LookupWrite:
        return uses(stmt.lookup_index) || uses(stmt.lookup_value) ||
               std::any_of(stmt.lookup_elements.begin(), stmt.lookup_elements.end(), uses);
    }
    return false;
}

bool termUses(const s8opnorm::S8Terminator& term, SymbolId symbol) {
    if (term.kind == s8opnorm::S8TermKind::Branch) return operandIs(term.condition, symbol);
    if (term.kind != s8opnorm::S8TermKind::Switch) return false;
    if (operandIs(term.switch_value, symbol)) return true;
    for (const auto& target : term.switch_targets)
        if (target.value && operandIs(*target.value, symbol)) return true;
    return false;
}

bool totalCombinational(const s8opnorm::S8Stmt& stmt) {
    if (stmt.kind == s8opnorm::S8StmtKind::Assign) return true;
    if (stmt.kind != s8opnorm::S8StmtKind::Op) return false;
    switch (stmt.op.kind) {
    case s8opnorm::S8OpKind::DynamicSlice:
    case s8opnorm::S8OpKind::DynamicBitSelect:
    case s8opnorm::S8OpKind::WriteSlice:
    case s8opnorm::S8OpKind::WriteBit:
    case s8opnorm::S8OpKind::DynamicWriteSlice:
    case s8opnorm::S8OpKind::DynamicWriteBit:
        return false;
    default:
        return true;
    }
}

bool operandsDefinedIn(const s8opnorm::S8Stmt& stmt, const std::unordered_set<SymbolId>& defs) {
    auto defined = [&defs](const s8opnorm::S8Operand& operand) {
        return operand.kind == s8opnorm::S8OperandKind::Var && defs.count(operand.symbol) != 0;
    };
    if (stmt.kind == s8opnorm::S8StmtKind::Assign) return defined(stmt.value);
    if (stmt.kind != s8opnorm::S8StmtKind::Op) return true;
    return std::any_of(stmt.op.operands.begin(), stmt.op.operands.end(), defined);
}

} // namespace

Summary hoistBranchLocalCombinational(s8opnorm::S8NormProgram& program) {
    auto& fn = program.top;
    const int count = static_cast<int>(fn.blocks.size());
    if (count == 0) return {};
    for (int index = 0; index < count; ++index)
        if (fn.blocks[static_cast<std::size_t>(index)].id != index)
            throw std::runtime_error("S81 state upgrade requires dense S8 block ids");

    std::vector<std::vector<BlockId>> succs(static_cast<std::size_t>(count));
    std::vector<std::vector<BlockId>> preds(static_cast<std::size_t>(count));
    for (int block = 0; block < count; ++block) {
        succs[static_cast<std::size_t>(block)] = successors(fn.blocks[static_cast<std::size_t>(block)].terminator);
        for (BlockId successor : succs[static_cast<std::size_t>(block)]) {
            if (successor < 0 || successor >= count) throw std::runtime_error("S81 state upgrade found invalid successor");
            preds[static_cast<std::size_t>(successor)].push_back(block);
        }
    }

    std::vector<int> depth(static_cast<std::size_t>(count), -1);
    std::deque<BlockId> queue;
    if (fn.entry >= 0 && fn.entry < count) { depth[static_cast<std::size_t>(fn.entry)] = 0; queue.push_back(fn.entry); }
    while (!queue.empty()) {
        const BlockId block = queue.front(); queue.pop_front();
        for (BlockId successor : succs[static_cast<std::size_t>(block)]) {
            const int candidate = depth[static_cast<std::size_t>(block)] + 1;
            if (candidate > depth[static_cast<std::size_t>(successor)]) {
                depth[static_cast<std::size_t>(successor)] = candidate;
                queue.push_back(successor);
            }
        }
    }
    std::vector<BlockId> order;
    for (BlockId block = 0; block < count; ++block) if (depth[static_cast<std::size_t>(block)] >= 0) order.push_back(block);
    std::sort(order.begin(), order.end(), [&](BlockId a, BlockId b) { return depth[a] > depth[b]; });

    Summary summary;
    unsigned unique = 0;
    for (BlockId parent_id : order) {
        auto& parent = fn.blocks[static_cast<std::size_t>(parent_id)];
        if (parent.terminator.kind != s8opnorm::S8TermKind::Branch) continue;
        for (BlockId child_id : succs[static_cast<std::size_t>(parent_id)]) {
            if (preds[static_cast<std::size_t>(child_id)].size() != 1) continue;
            auto& child = fn.blocks[static_cast<std::size_t>(child_id)];

            // The rename region is deliberately limited to single-predecessor
            // descendants. A merge could observe the old value from another
            // branch, so it is never rewritten by this local transformation.
            std::vector<BlockId> region;
            std::deque<BlockId> region_queue;
            std::unordered_set<BlockId> region_set;
            region_queue.push_back(child_id);
            while (!region_queue.empty()) {
                const BlockId block = region_queue.front(); region_queue.pop_front();
                if (!region_set.insert(block).second) continue;
                region.push_back(block);
                for (BlockId successor : succs[static_cast<std::size_t>(block)])
                    if (preds[static_cast<std::size_t>(successor)].size() == 1) region_queue.push_back(successor);
            }

            std::unordered_set<SymbolId> local_defs;
            for (auto& stmt : child.stmts) {
                if (!totalCombinational(stmt) || stmt.target < 0 ||
                    fn.symbols[static_cast<std::size_t>(stmt.target)].role == s8opnorm::S8SymbolRole::Port ||
                    operandsDefinedIn(stmt, local_defs)) {
                    if (stmt.target >= 0) local_defs.insert(stmt.target);
                    continue;
                }
                const SymbolId old = stmt.target;
                bool escapes = false;
                std::deque<BlockId> outside_work;
                std::unordered_set<BlockId> outside_seen;
                for (BlockId block : region) {
                    const auto& candidate = fn.blocks[static_cast<std::size_t>(block)];
                    for (const auto& inner : candidate.stmts) {
                        if (&candidate == &child && &inner == &stmt) continue;
                        if (stmtDefines(inner, old)) { escapes = true; break; }
                    }
                    if (escapes) break;
                    for (BlockId successor : succs[static_cast<std::size_t>(block)]) {
                        if (!region_set.count(successor)) outside_work.push_back(successor);
                    }
                }
                while (!escapes && !outside_work.empty()) {
                    const BlockId block = outside_work.front(); outside_work.pop_front();
                    if (!outside_seen.insert(block).second) continue;
                    const auto& candidate = fn.blocks[static_cast<std::size_t>(block)];
                    for (const auto& inner : candidate.stmts) {
                        if (stmtUses(inner, old) || termUses(candidate.terminator, old)) {
                            escapes = true;
                            break;
                        }
                    }
                    if (escapes || termUses(candidate.terminator, old)) { escapes = true; break; }
                    for (BlockId successor : succs[static_cast<std::size_t>(block)]) outside_work.push_back(successor);
                }
                if (escapes) { local_defs.insert(old); continue; }

                auto clone = fn.symbols[static_cast<std::size_t>(old)];
                clone.id = static_cast<SymbolId>(fn.symbols.size());
                clone.role = s8opnorm::S8SymbolRole::Temp;
                clone.debug_name = "__s81_state_up_" + clone.debug_name + "_" + std::to_string(unique++);
                fn.symbols.push_back(std::move(clone));
                stmt.target = static_cast<SymbolId>(fn.symbols.size() - 1);
                for (BlockId block : region) {
                    auto& candidate = fn.blocks[static_cast<std::size_t>(block)];
                    for (auto& inner : candidate.stmts) renameUses(inner, old, stmt.target);
                    renameUses(candidate.terminator, old, stmt.target);
                }
                parent.stmts.push_back(stmt);
                stmt.kind = s8opnorm::S8StmtKind::Assign;
                stmt.target = -1; // Erased after this traversal.
                ++summary.lifted_statements;
                ++summary.renamed_symbols;
            }
            child.stmts.erase(std::remove_if(child.stmts.begin(), child.stmts.end(),
                [](const s8opnorm::S8Stmt& stmt) { return stmt.target < 0; }), child.stmts.end());
        }
    }
    return summary;
}

} // namespace pred::s81stateupgrade
