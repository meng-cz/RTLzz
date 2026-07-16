#include "s10predicate/S10Predicate.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace pred::s10predicate {
namespace {

using namespace pred::s9ssa;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s10predicate";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message,
                       DebugLoc loc = {},
                       std::string note = {}) {
    throwRTLZZ(makeContext(std::move(loc), std::move(note)), message);
}

bool typeEq(const S10Type& lhs, const S10Type& rhs) {
    return lhs.kind == rhs.kind && lhs.width == rhs.width;
}

bool isBoolType(const S10Type& type) {
    return type.kind == s8opnorm::S8TypeKind::Bool && type.width == 1;
}

std::string typeText(const S10Type& type) {
    return type.kind == s8opnorm::S8TypeKind::Bool ? "bool" : ("u" + std::to_string(type.width));
}

int wordCount(int width) {
    return width <= 0 ? 0 : ((width + 63) / 64);
}

std::uint64_t highMask(int width) {
    int rem = width % 64;
    if (rem == 0) return ~std::uint64_t{0};
    return (std::uint64_t{1} << rem) - 1;
}

void trimToWidth(std::vector<std::uint64_t>& words, int width) {
    words.resize(static_cast<std::size_t>(wordCount(width)), 0);
    if (!words.empty()) words.back() &= highMask(width);
}

S10Literal makeLiteralValue(std::uint64_t value, S10Type type, bool signed_view = false) {
    S10Literal literal;
    literal.valid_width = type.width;
    literal.is_signed = signed_view;
    literal.source_text = std::to_string(value);
    literal.words.push_back(value);
    trimToWidth(literal.words, type.width);
    return literal;
}

S10Operand literalOperand(std::uint64_t value,
                          S10Type type,
                          bool signed_view = false,
                          DebugLoc loc = {}) {
    S10Operand out;
    out.kind = S10OperandKind::Literal;
    out.type = type;
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.literal = makeLiteralValue(value, type, signed_view);
    return out;
}

S10Operand trueGuard(DebugLoc loc = {}) {
    return literalOperand(1, S10Type{s8opnorm::S8TypeKind::Bool, 1}, false, std::move(loc));
}

S10Operand falseGuard(DebugLoc loc = {}) {
    return literalOperand(0, S10Type{s8opnorm::S8TypeKind::Bool, 1}, false, std::move(loc));
}

bool literalBool(const S10Operand& operand, bool& out) {
    if (operand.kind != S10OperandKind::Literal || !isBoolType(operand.type)) return false;
    std::uint64_t value = operand.literal.words.empty() ? 0 : operand.literal.words.front();
    out = (value & 1U) != 0;
    return true;
}

bool isTrue(const S10Operand& operand) {
    bool value = false;
    return literalBool(operand, value) && value;
}

bool isFalse(const S10Operand& operand) {
    bool value = false;
    return literalBool(operand, value) && !value;
}

bool operandEq(const S10Operand& lhs, const S10Operand& rhs) {
    if (lhs.kind != rhs.kind || lhs.signed_view != rhs.signed_view ||
        !typeEq(lhs.type, rhs.type)) {
        return false;
    }
    if (lhs.kind == S10OperandKind::Value) return lhs.value == rhs.value;
    return lhs.literal.valid_width == rhs.literal.valid_width &&
           lhs.literal.words == rhs.literal.words;
}

std::string wordsHex(const std::vector<std::uint64_t>& words, int width) {
    if (width <= 0) return "0";
    std::ostringstream os;
    os << "0x";
    bool started = false;
    for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
        std::ostringstream part;
        part << std::hex << words[static_cast<std::size_t>(i)];
        std::string text = part.str();
        if (!started) {
            os << text;
            started = true;
        } else {
            os << std::string(16 - std::min<int>(16, text.size()), '0') << text;
        }
    }
    if (!started) os << "0";
    return os.str();
}

const S9Value& s9ValueAt(const S9SSACFG& fn, S9ValueId id) {
    if (id < 0 || id >= static_cast<S9ValueId>(fn.values.size())) {
        fail("Invalid S9 value reference");
    }
    const auto& value = fn.values[static_cast<std::size_t>(id)];
    if (value.id != id) fail("Broken S9 value table invariant");
    return value;
}

const S10Value& valueAt(const S10PredicateProgram& program, S10ValueId id) {
    if (id < 0 || id >= static_cast<S10ValueId>(program.values.size())) {
        fail("Invalid S10 value reference");
    }
    const auto& value = program.values[static_cast<std::size_t>(id)];
    if (value.id != id) fail("Broken S10 value table invariant");
    return value;
}

std::string symbolName(const S10PredicateProgram& program, SymbolId symbol) {
    if (symbol < 0 || symbol >= static_cast<SymbolId>(program.base_symbols.size())) {
        return "<generated>";
    }
    return program.base_symbols[static_cast<std::size_t>(symbol)].debug_name;
}

std::string valueName(const S10PredicateProgram& program, S10ValueId value_id) {
    const auto& value = valueAt(program, value_id);
    if (value.base_symbol >= 0) {
        return symbolName(program, value.base_symbol) + "_v" + std::to_string(value.version);
    }
    return value.debug_name + "#" + std::to_string(value.id);
}

std::string valueKindName(S10ValueKind kind) {
    switch (kind) {
    case S10ValueKind::Initial: return "initial";
    case S10ValueKind::Statement: return "stmt";
    case S10ValueKind::Phi: return "phi";
    case S10ValueKind::Generated: return "generated";
    }
    return "value";
}

std::string opName(S10OpKind kind) {
    switch (kind) {
    case S10OpKind::AssignCast: return "AssignCast";
    case S10OpKind::Add: return "Add";
    case S10OpKind::Sub: return "Sub";
    case S10OpKind::Mul: return "Mul";
    case S10OpKind::Neg: return "Neg";
    case S10OpKind::BitNot: return "BitNot";
    case S10OpKind::LogicalNot: return "LogicalNot";
    case S10OpKind::BitAnd: return "BitAnd";
    case S10OpKind::BitOr: return "BitOr";
    case S10OpKind::BitXor: return "BitXor";
    case S10OpKind::BoolAnd: return "BoolAnd";
    case S10OpKind::BoolOr: return "BoolOr";
    case S10OpKind::Shl: return "Shl";
    case S10OpKind::LShr: return "LShr";
    case S10OpKind::AShr: return "AShr";
    case S10OpKind::Eq: return "Eq";
    case S10OpKind::Ne: return "Ne";
    case S10OpKind::Lt: return "Lt";
    case S10OpKind::Le: return "Le";
    case S10OpKind::Gt: return "Gt";
    case S10OpKind::Ge: return "Ge";
    case S10OpKind::Mux: return "Mux";
    case S10OpKind::ZExt: return "ZExt";
    case S10OpKind::SExt: return "SExt";
    case S10OpKind::Trunc: return "Trunc";
    case S10OpKind::Slice: return "Slice";
    case S10OpKind::BitSelect: return "BitSelect";
    case S10OpKind::DynamicSlice: return "DynamicSlice";
    case S10OpKind::DynamicBitSelect: return "DynamicBitSelect";
    case S10OpKind::WriteSlice: return "WriteSlice";
    case S10OpKind::WriteBit: return "WriteBit";
    case S10OpKind::DynamicWriteSlice: return "DynamicWriteSlice";
    case S10OpKind::DynamicWriteBit: return "DynamicWriteBit";
    case S10OpKind::Concat: return "Concat";
    case S10OpKind::Repeat: return "Repeat";
    case S10OpKind::ReduceOr: return "ReduceOr";
    case S10OpKind::ReduceAnd: return "ReduceAnd";
    case S10OpKind::ReduceXor: return "ReduceXor";
    }
    return "Op";
}

S10ValueKind convertKind(S9ValueKind kind) {
    switch (kind) {
    case S9ValueKind::Initial: return S10ValueKind::Initial;
    case S9ValueKind::Statement: return S10ValueKind::Statement;
    case S9ValueKind::Phi: return S10ValueKind::Phi;
    case S9ValueKind::Generated: return S10ValueKind::Generated;
    }
    return S10ValueKind::Statement;
}

S10Operand mapOperand(const S10PredicateProgram& out, const S9Operand& operand) {
    S10Operand mapped;
    mapped.type = operand.type;
    mapped.signed_view = operand.signed_view;
    mapped.debug_loc = operand.debug_loc;
    if (operand.kind == S9OperandKind::Literal) {
        mapped.kind = S10OperandKind::Literal;
        mapped.literal = operand.literal;
        return mapped;
    }
    mapped.kind = S10OperandKind::Value;
    mapped.value = operand.value;
    (void)valueAt(out, mapped.value);
    return mapped;
}

S10Operand valueOperand(const S10PredicateProgram& program,
                        S10ValueId value,
                        bool signed_view = false,
                        DebugLoc loc = {}) {
    S10Operand out;
    out.kind = S10OperandKind::Value;
    out.type = valueAt(program, value).type;
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.value = value;
    return out;
}

S10Operation copyOperation(const S9Operation& op, const S10PredicateProgram& out) {
    S10Operation copied;
    copied.kind = op.kind;
    copied.debug_loc = op.debug_loc;
    copied.hi = op.hi;
    copied.lo = op.lo;
    copied.bit = op.bit;
    copied.times = op.times;
    copied.result_width = op.result_width;
    for (const auto& operand : op.operands) copied.operands.push_back(mapOperand(out, operand));
    return copied;
}

std::vector<BlockId> successorsOf(const S9Terminator& term) {
    std::vector<BlockId> out;
    switch (term.kind) {
    case S9TermKind::Jump:
        out.push_back(term.jump_target);
        break;
    case S9TermKind::Branch:
        out.push_back(term.true_target);
        out.push_back(term.false_target);
        break;
    case S9TermKind::Switch:
        for (const auto& target : term.switch_targets) out.push_back(target.target);
        out.push_back(term.default_target);
        break;
    case S9TermKind::Exit:
    case S9TermKind::Unreachable:
        break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct CFGInfo {
    std::vector<std::vector<BlockId>> succs;
    std::vector<std::vector<BlockId>> preds;
    std::vector<BlockId> topo;
};

CFGInfo analyzeCFG(const S9SSACFG& fn) {
    const int n = static_cast<int>(fn.blocks.size());
    if (fn.entry < 0 || fn.entry >= n) fail("S9 CFG has invalid entry block");
    CFGInfo info;
    info.succs.resize(static_cast<std::size_t>(n));
    info.preds.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& block = fn.blocks[static_cast<std::size_t>(i)];
        if (block.id != i) fail("S10 currently requires dense S9 block ids");
        if (!block.reachable) continue;
        info.succs[static_cast<std::size_t>(i)] = successorsOf(block.terminator);
        for (BlockId succ : info.succs[static_cast<std::size_t>(i)]) {
            if (succ < 0 || succ >= n) fail("S9 CFG has invalid successor block");
            info.preds[static_cast<std::size_t>(succ)].push_back(i);
        }
    }

    std::vector<int> color(static_cast<std::size_t>(n), 0);
    std::vector<BlockId> postorder;
    std::function<void(BlockId)> dfs = [&](BlockId block) {
        if (!fn.blocks[static_cast<std::size_t>(block)].reachable) return;
        color[static_cast<std::size_t>(block)] = 1;
        for (BlockId succ : info.succs[static_cast<std::size_t>(block)]) {
            if (!fn.blocks[static_cast<std::size_t>(succ)].reachable) continue;
            if (color[static_cast<std::size_t>(succ)] == 1) {
                fail("S10 predicate lowering does not support cyclic CFG");
            }
            if (color[static_cast<std::size_t>(succ)] == 0) dfs(succ);
        }
        color[static_cast<std::size_t>(block)] = 2;
        postorder.push_back(block);
    };
    dfs(fn.entry);
    info.topo.assign(postorder.rbegin(), postorder.rend());
    return info;
}

struct Context {
    const S9SSACFG* input = nullptr;
    S10PredicateProgram output;
    PredicateOptions options;
    PredicateSummary summary;
    int generated_counter = 0;
};

S10ValueId addValue(Context& ctx,
                    S10Type type,
                    std::string debug_name,
                    BlockId source_block,
                    S10ValueKind kind = S10ValueKind::Generated) {
    if (static_cast<int>(ctx.output.values.size() + 1) > ctx.options.max_values) {
        fail("S10 value limit exceeded");
    }
    S10Value value;
    value.id = static_cast<S10ValueId>(ctx.output.values.size());
    value.base_symbol = -1;
    value.version = -1;
    value.type = type;
    value.kind = kind;
    value.debug_name = std::move(debug_name);
    value.source_block = source_block;
    ctx.output.values.push_back(std::move(value));
    return static_cast<S10ValueId>(ctx.output.values.size() - 1);
}

S10Definition makeAssign(S10ValueId target,
                         S10Operand value,
                         S10Operand guard,
                         DebugLoc loc,
                         BlockId block,
                         std::string note = {}) {
    S10Definition def;
    def.kind = S10DefKind::Assign;
    def.debug_loc = std::move(loc);
    def.guard = std::move(guard);
    def.target = target;
    def.value = std::move(value);
    def.source_block = block;
    def.debug_note = std::move(note);
    return def;
}

S10Definition makeOp(S10ValueId target,
                     S10OpKind kind,
                     S10Type result_type,
                     std::vector<S10Operand> operands,
                     S10Operand guard,
                     DebugLoc loc,
                     BlockId block,
                     std::string note = {}) {
    S10Definition def;
    def.kind = S10DefKind::Op;
    def.debug_loc = loc;
    def.guard = std::move(guard);
    def.target = target;
    def.op.kind = kind;
    def.op.debug_loc = loc;
    def.op.result_width = result_type.width;
    def.op.operands = std::move(operands);
    def.source_block = block;
    def.debug_note = std::move(note);
    return def;
}

S10Operand emitOp(Context& ctx,
                  S10OpKind kind,
                  S10Type type,
                  std::vector<S10Operand> operands,
                  S10Operand guard,
                  DebugLoc loc,
                  BlockId block,
                  const std::string& hint,
                  const std::string& note = {}) {
    S10ValueId value = addValue(ctx, type,
                                "__s10_" + hint + "_" + std::to_string(ctx.generated_counter++),
                                block);
    ctx.output.definitions.push_back(makeOp(value, kind, type, std::move(operands),
                                            std::move(guard), loc, block, note));
    return valueOperand(ctx.output, value, false, loc);
}

S10Operand makeNot(Context& ctx, S10Operand value, DebugLoc loc, BlockId block);

S10Operand makeAnd(Context& ctx,
                   S10Operand lhs,
                   S10Operand rhs,
                   DebugLoc loc,
                   BlockId block) {
    if (isFalse(lhs) || isFalse(rhs)) return falseGuard(loc);
    if (isTrue(lhs)) return rhs;
    if (isTrue(rhs)) return lhs;
    if (operandEq(lhs, rhs)) return lhs;
    ++ctx.summary.generated_guards;
    return emitOp(ctx, S10OpKind::BoolAnd, S10Type{s8opnorm::S8TypeKind::Bool, 1},
                  {std::move(lhs), std::move(rhs)}, trueGuard(loc), loc, block,
                  "guard_and", "guard");
}

S10Operand makeOr(Context& ctx,
                  S10Operand lhs,
                  S10Operand rhs,
                  DebugLoc loc,
                  BlockId block) {
    if (isTrue(lhs) || isTrue(rhs)) return trueGuard(loc);
    if (isFalse(lhs)) return rhs;
    if (isFalse(rhs)) return lhs;
    if (operandEq(lhs, rhs)) return lhs;
    ++ctx.summary.generated_guards;
    return emitOp(ctx, S10OpKind::BoolOr, S10Type{s8opnorm::S8TypeKind::Bool, 1},
                  {std::move(lhs), std::move(rhs)}, trueGuard(loc), loc, block,
                  "guard_or", "guard");
}

S10Operand makeNot(Context& ctx, S10Operand value, DebugLoc loc, BlockId block) {
    if (isTrue(value)) return falseGuard(loc);
    if (isFalse(value)) return trueGuard(loc);
    ++ctx.summary.generated_guards;
    return emitOp(ctx, S10OpKind::LogicalNot, S10Type{s8opnorm::S8TypeKind::Bool, 1},
                  {std::move(value)}, trueGuard(loc), loc, block, "guard_not", "guard");
}

S10Operand makeEq(Context& ctx,
                  S10Operand lhs,
                  S10Operand rhs,
                  DebugLoc loc,
                  BlockId block) {
    ++ctx.summary.generated_guards;
    return emitOp(ctx, S10OpKind::Eq, S10Type{s8opnorm::S8TypeKind::Bool, 1},
                  {std::move(lhs), std::move(rhs)}, trueGuard(loc), loc, block,
                  "guard_eq", "switch_guard");
}

using EdgeKey = std::pair<BlockId, BlockId>;

struct GuardInfo {
    std::vector<std::optional<S10Operand>> block_guards;
    std::map<EdgeKey, S10Operand> edge_guards;
};

void addEdgeGuard(Context& ctx,
                  GuardInfo& guards,
                  BlockId pred,
                  BlockId succ,
                  S10Operand guard,
                  DebugLoc loc) {
    EdgeKey key{pred, succ};
    auto it = guards.edge_guards.find(key);
    if (it == guards.edge_guards.end()) {
        guards.edge_guards.emplace(key, std::move(guard));
    } else {
        it->second = makeOr(ctx, std::move(it->second), std::move(guard), loc, pred);
    }
}

GuardInfo computeGuards(Context& ctx, const CFGInfo& cfg) {
    const auto& fn = *ctx.input;
    GuardInfo guards;
    guards.block_guards.resize(fn.blocks.size());
    guards.block_guards[static_cast<std::size_t>(fn.entry)] = trueGuard();

    for (BlockId block_id : cfg.topo) {
        const auto& block = fn.blocks[static_cast<std::size_t>(block_id)];
        if (!block.reachable) continue;
        S10Operand block_guard = guards.block_guards[static_cast<std::size_t>(block_id)]
            .value_or(falseGuard());

        auto propagate = [&](BlockId succ, S10Operand edge_guard, DebugLoc loc) {
            if (succ < 0 || succ >= static_cast<BlockId>(fn.blocks.size())) {
                fail("S9 terminator has invalid successor", loc);
            }
            if (!fn.blocks[static_cast<std::size_t>(succ)].reachable) return;
            S10Operand edge_for_block = edge_guard;
            addEdgeGuard(ctx, guards, block_id, succ, std::move(edge_guard), loc);
            auto& succ_guard = guards.block_guards[static_cast<std::size_t>(succ)];
            if (succ_guard) {
                succ_guard = makeOr(ctx, std::move(succ_guard.value()), std::move(edge_for_block),
                                    loc, block_id);
            } else {
                succ_guard = std::move(edge_for_block);
            }
        };

        switch (block.terminator.kind) {
        case S9TermKind::Jump:
            propagate(block.terminator.jump_target, block_guard, {});
            break;
        case S9TermKind::Branch: {
            S10Operand cond = mapOperand(ctx.output, block.terminator.condition);
            if (!isBoolType(cond.type)) fail("S10 branch condition must be bool");
            propagate(block.terminator.true_target,
                      makeAnd(ctx, block_guard, cond, block.terminator.condition.debug_loc,
                              block_id),
                      block.terminator.condition.debug_loc);
            propagate(block.terminator.false_target,
                      makeAnd(ctx, block_guard,
                              makeNot(ctx, cond, block.terminator.condition.debug_loc, block_id),
                              block.terminator.condition.debug_loc, block_id),
                      block.terminator.condition.debug_loc);
            break;
        }
        case S9TermKind::Switch: {
            if (block.terminator.default_target < 0) {
                fail("S10 switch requires a default target");
            }
            S10Operand switch_value = mapOperand(ctx.output, block.terminator.switch_value);
            std::set<std::string> seen_cases;
            std::optional<S10Operand> any_case;
            for (const auto& target : block.terminator.switch_targets) {
                if (!target.value) fail("S10 switch case is missing a value");
                S10Operand case_value = mapOperand(ctx.output, target.value.value());
                if (!typeEq(switch_value.type, case_value.type)) {
                    fail("S10 switch case type mismatch", case_value.debug_loc);
                }
                if (case_value.kind == S10OperandKind::Literal) {
                    std::string case_key = wordsHex(case_value.literal.words,
                                                    case_value.literal.valid_width);
                    if (!seen_cases.insert(case_key).second) {
                        fail("S10 switch has duplicate case literal", case_value.debug_loc);
                    }
                }
                S10Operand eq = makeEq(ctx, switch_value, case_value,
                                       block.terminator.switch_value.debug_loc, block_id);
                any_case = any_case
                    ? makeOr(ctx, any_case.value(), eq, block.terminator.switch_value.debug_loc,
                             block_id)
                    : eq;
                propagate(target.target,
                          makeAnd(ctx, block_guard, eq,
                                  block.terminator.switch_value.debug_loc, block_id),
                          block.terminator.switch_value.debug_loc);
            }
            S10Operand default_local = any_case
                ? makeNot(ctx, any_case.value(), block.terminator.switch_value.debug_loc, block_id)
                : trueGuard(block.terminator.switch_value.debug_loc);
            propagate(block.terminator.default_target,
                      makeAnd(ctx, block_guard, std::move(default_local),
                              block.terminator.switch_value.debug_loc, block_id),
                      block.terminator.switch_value.debug_loc);
            break;
        }
        case S9TermKind::Exit:
        case S9TermKind::Unreachable:
            break;
        }
    }
    return guards;
}

S10Operand guardForBlock(const GuardInfo& guards, BlockId block) {
    if (block < 0 || block >= static_cast<BlockId>(guards.block_guards.size())) {
        return falseGuard();
    }
    return guards.block_guards[static_cast<std::size_t>(block)].value_or(falseGuard());
}

S10Operand guardForEdge(Context& ctx,
                        const GuardInfo& guards,
                        BlockId pred,
                        BlockId succ,
                        DebugLoc loc) {
    auto it = guards.edge_guards.find(EdgeKey{pred, succ});
    if (it == guards.edge_guards.end()) {
        fail("Missing edge guard for phi incoming", loc,
             "bb" + std::to_string(pred) + "->bb" + std::to_string(succ));
    }
    (void)ctx;
    return it->second;
}

void lowerPhi(Context& ctx,
              const S9Phi& phi,
              BlockId block_id,
              const GuardInfo& guards) {
    if (phi.incoming.empty()) return;
    const S10Value& result = valueAt(ctx.output, phi.result);
    if (phi.incoming.size() == 1) {
        S10Operand incoming_guard = guardForEdge(ctx, guards, phi.incoming.front().pred,
                                                 block_id, phi.debug_loc);
        S10Operand incoming = valueOperand(ctx.output, phi.incoming.front().value,
                                           false, phi.debug_loc);
        ctx.output.definitions.push_back(makeAssign(phi.result, std::move(incoming),
                                                    std::move(incoming_guard), phi.debug_loc,
                                                    block_id, "lowered_phi"));
        ++ctx.summary.lowered_phis;
        return;
    }

    S10Operand fallback = valueOperand(ctx.output, phi.incoming.back().value,
                                       false, phi.debug_loc);
    S10Operand suffix_guard = guardForEdge(ctx, guards, phi.incoming.back().pred,
                                           block_id, phi.debug_loc);
    for (int i = static_cast<int>(phi.incoming.size()) - 2; i >= 0; --i) {
        const auto& incoming = phi.incoming[static_cast<std::size_t>(i)];
        S10Operand edge_guard = guardForEdge(ctx, guards, incoming.pred, block_id,
                                             phi.debug_loc);
        S10Operand mux_guard = makeOr(ctx, edge_guard, suffix_guard, phi.debug_loc,
                                      block_id);
        S10Operand incoming_value = valueOperand(ctx.output, incoming.value,
                                                 false, phi.debug_loc);
        S10ValueId target = (i == 0)
            ? phi.result
            : addValue(ctx, result.type,
                       "__s10_phi_mux_" + std::to_string(ctx.generated_counter++),
                       block_id);
        S10Operand next_suffix_guard = mux_guard;
        ctx.output.definitions.push_back(makeOp(target, S10OpKind::Mux, result.type,
                                                {std::move(edge_guard),
                                                 std::move(incoming_value),
                                                 std::move(fallback)},
                                                mux_guard, phi.debug_loc,
                                                block_id, "lowered_phi"));
        fallback = valueOperand(ctx.output, target, false, phi.debug_loc);
        suffix_guard = std::move(next_suffix_guard);
    }
    ++ctx.summary.lowered_phis;
}

void lowerStmt(Context& ctx,
               const S9Stmt& stmt,
               BlockId block_id,
               const S10Operand& block_guard) {
    switch (stmt.kind) {
    case S9StmtKind::Assign:
        ctx.output.definitions.push_back(makeAssign(stmt.target,
                                                    mapOperand(ctx.output, stmt.value),
                                                    block_guard, stmt.debug_loc,
                                                    block_id, stmt.debug_note));
        break;
    case S9StmtKind::Op: {
        S10Definition def;
        def.kind = S10DefKind::Op;
        def.debug_loc = stmt.debug_loc;
        def.guard = block_guard;
        def.target = stmt.target;
        def.op = copyOperation(stmt.op, ctx.output);
        def.source_block = block_id;
        def.debug_note = stmt.debug_note;
        ctx.output.definitions.push_back(std::move(def));
        break;
    }
    case S9StmtKind::Lookup: {
        S10Definition def;
        def.kind = S10DefKind::Lookup;
        def.debug_loc = stmt.debug_loc;
        def.guard = block_guard;
        def.target = stmt.target;
        def.lookup_index = mapOperand(ctx.output, stmt.lookup_index);
        for (const auto& elem : stmt.lookup_elements) {
            def.lookup_elements.push_back(mapOperand(ctx.output, elem));
        }
        def.source_block = block_id;
        def.debug_note = stmt.debug_note;
        ctx.output.definitions.push_back(std::move(def));
        break;
    }
    }
}

bool isOutputPort(const S10Port& port) {
    return port.direction == ParamDirection::Output ||
           port.passing == ParamPassingKind::MutableRef;
}

S10PredicateProgram lowerFunction(const S9SSACFG& fn,
                                  const PredicateOptions& options,
                                  PredicateSummary& summary) {
    verifySSAProgram(S9SSAProgram{fn});
    CFGInfo cfg = analyzeCFG(fn);

    Context ctx;
    ctx.input = &fn;
    ctx.options = options;
    ctx.output.name = fn.name;
    ctx.summary.function_name = fn.name;

    for (const auto& symbol : fn.base_symbols) {
        S10Symbol out;
        out.id = symbol.id;
        out.type = symbol.type;
        out.debug_name = symbol.debug_name;
        out.role = symbol.role;
        ctx.output.base_symbols.push_back(std::move(out));
    }
    for (const auto& value : fn.values) {
        S10Value out;
        out.id = value.id;
        out.base_symbol = value.base_symbol;
        out.version = value.version;
        out.type = value.type;
        out.kind = convertKind(value.kind);
        out.debug_name = value.debug_name;
        out.source_block = value.def_block;
        if (out.id != static_cast<S10ValueId>(ctx.output.values.size())) {
            fail("S9 values must be dense before S10 predicate lowering");
        }
        ctx.output.values.push_back(std::move(out));
    }
    for (const auto& port : fn.ports) {
        S10Port out;
        out.symbol = port.symbol;
        out.direction = port.direction;
        out.passing = port.passing;
        out.initial_value = port.initial_value;
        out.final_value = port.final_value;
        ctx.output.ports.push_back(std::move(out));
    }
    for (const auto& group : fn.port_groups) {
        S10PortGroup out;
        out.source_name = group.source_name;
        out.direction = group.direction;
        out.passing = group.passing;
        out.source_type = group.source_type;
        out.scalar_source_type = group.scalar_source_type;
        out.scalar_type = group.scalar_type;
        out.array_dims = group.array_dims;
        for (const auto& element : group.elements) {
            S10PortElement out_element;
            out_element.symbol = element.symbol;
            out_element.indices = element.indices;
            out.elements.push_back(std::move(out_element));
        }
        ctx.output.port_groups.push_back(std::move(out));
    }

    GuardInfo guards = computeGuards(ctx, cfg);
    ctx.output.block_guards.resize(fn.blocks.size());
    for (const auto& block : fn.blocks) {
        S10BlockGuard guard;
        guard.block = block.id;
        guard.reachable = block.reachable;
        if (block.reachable) guard.guard = guardForBlock(guards, block.id);
        ctx.output.block_guards[static_cast<std::size_t>(block.id)] = std::move(guard);
        if (!block.reachable) ++ctx.summary.ignored_unreachable_blocks;
    }

    for (BlockId block_id : cfg.topo) {
        const auto& block = fn.blocks[static_cast<std::size_t>(block_id)];
        if (!block.reachable) continue;
        S10Operand block_guard = guardForBlock(guards, block_id);
        for (const auto& phi : block.phis) lowerPhi(ctx, phi, block_id, guards);
        for (const auto& stmt : block.stmts) lowerStmt(ctx, stmt, block_id, block_guard);
    }

    for (auto& port : ctx.output.ports) {
        if (isOutputPort(port) && !port.final_value) {
            fail("Output port has no final predicate value", {},
                 "symbol=" + symbolName(ctx.output, port.symbol));
        }
        if (port.final_value) (void)valueAt(ctx.output, port.final_value.value());
        if (port.initial_value) (void)valueAt(ctx.output, port.initial_value.value());
        port.final_guard = trueGuard();
    }

    ctx.summary.values = static_cast<int>(ctx.output.values.size());
    ctx.summary.definitions = static_cast<int>(ctx.output.definitions.size());
    ctx.summary.block_guards = static_cast<int>(ctx.output.block_guards.size());
    summary = ctx.summary;
    return std::move(ctx.output);
}

std::string operandText(const S10PredicateProgram& program, const S10Operand& operand) {
    std::ostringstream os;
    if (operand.signed_view) os << "s:";
    if (operand.kind == S10OperandKind::Value) {
        os << valueName(program, operand.value);
    } else {
        os << wordsHex(operand.literal.words, operand.literal.valid_width);
    }
    os << "<" << typeText(operand.type) << ">";
    return os.str();
}

std::string defText(const S10PredicateProgram& program, const S10Definition& def) {
    std::ostringstream os;
    os << "guard=" << operandText(program, def.guard) << " ";
    switch (def.kind) {
    case S10DefKind::Assign:
        os << "assign " << valueName(program, def.target) << " = "
           << operandText(program, def.value);
        break;
    case S10DefKind::Op:
        os << "op " << valueName(program, def.target) << " = "
           << opName(def.op.kind) << "<" << def.op.result_width << ">(";
        for (std::size_t i = 0; i < def.op.operands.size(); ++i) {
            if (i) os << ", ";
            os << operandText(program, def.op.operands[i]);
        }
        os << ")";
        if (def.op.hi >= 0 || def.op.lo >= 0 || def.op.bit >= 0 || def.op.times > 0) {
            os << " meta{hi=" << def.op.hi << ",lo=" << def.op.lo
               << ",bit=" << def.op.bit << ",times=" << def.op.times << "}";
        }
        break;
    case S10DefKind::Lookup:
        os << "lookup " << valueName(program, def.target) << " = lookup("
           << operandText(program, def.lookup_index);
        for (const auto& elem : def.lookup_elements) os << ", " << operandText(program, elem);
        os << ")";
        break;
    }
    if (def.source_block >= 0) os << " source=bb" << def.source_block;
    if (!def.debug_note.empty()) os << " ; " << def.debug_note;
    return os.str();
}

void verifyOperand(const S10PredicateProgram& program, const S10Operand& operand) {
    if (operand.type.width <= 0) fail("S10 operand has invalid width");
    if (operand.kind == S10OperandKind::Value) {
        const auto& value = valueAt(program, operand.value);
        if (!typeEq(value.type, operand.type)) fail("S10 operand type mismatch");
    } else {
        if (operand.literal.valid_width != operand.type.width) {
            fail("S10 literal width does not match operand type");
        }
        if (static_cast<int>(operand.literal.words.size()) != wordCount(operand.literal.valid_width)) {
            fail("S10 literal word count mismatch");
        }
    }
}

struct BoolExpr;
using BoolExprPtr = std::shared_ptr<BoolExpr>;

struct BoolExpr {
    enum class Kind {
        Const,
        Atom,
        Not,
        And,
        Or,
    };

    Kind kind = Kind::Const;
    bool const_value = false;
    S10ValueId atom = -1;
    BoolExprPtr lhs;
    BoolExprPtr rhs;
};

BoolExprPtr boolConst(bool value) {
    auto out = std::make_shared<BoolExpr>();
    out->kind = BoolExpr::Kind::Const;
    out->const_value = value;
    return out;
}

BoolExprPtr boolAtom(S10ValueId value) {
    auto out = std::make_shared<BoolExpr>();
    out->kind = BoolExpr::Kind::Atom;
    out->atom = value;
    return out;
}

bool boolExprEqual(const BoolExprPtr& lhs, const BoolExprPtr& rhs) {
    if (lhs == rhs) return true;
    if (!lhs || !rhs || lhs->kind != rhs->kind) return false;
    switch (lhs->kind) {
    case BoolExpr::Kind::Const:
        return lhs->const_value == rhs->const_value;
    case BoolExpr::Kind::Atom:
        return lhs->atom == rhs->atom;
    case BoolExpr::Kind::Not:
        return boolExprEqual(lhs->lhs, rhs->lhs);
    case BoolExpr::Kind::And:
    case BoolExpr::Kind::Or:
        return (boolExprEqual(lhs->lhs, rhs->lhs) && boolExprEqual(lhs->rhs, rhs->rhs)) ||
               (boolExprEqual(lhs->lhs, rhs->rhs) && boolExprEqual(lhs->rhs, rhs->lhs));
    }
    return false;
}

BoolExprPtr boolNot(BoolExprPtr value) {
    if (!value) return boolConst(false);
    if (value->kind == BoolExpr::Kind::Const) return boolConst(!value->const_value);
    if (value->kind == BoolExpr::Kind::Not) return value->lhs;
    auto out = std::make_shared<BoolExpr>();
    out->kind = BoolExpr::Kind::Not;
    out->lhs = std::move(value);
    return out;
}

BoolExprPtr boolAnd(BoolExprPtr lhs, BoolExprPtr rhs) {
    if (!lhs || !rhs) return boolConst(false);
    if (lhs->kind == BoolExpr::Kind::Const) return lhs->const_value ? rhs : lhs;
    if (rhs->kind == BoolExpr::Kind::Const) return rhs->const_value ? lhs : rhs;
    if (boolExprEqual(lhs, rhs)) return lhs;
    auto out = std::make_shared<BoolExpr>();
    out->kind = BoolExpr::Kind::And;
    out->lhs = std::move(lhs);
    out->rhs = std::move(rhs);
    return out;
}

BoolExprPtr boolOr(BoolExprPtr lhs, BoolExprPtr rhs) {
    if (!lhs) return rhs;
    if (!rhs) return lhs;
    if (lhs->kind == BoolExpr::Kind::Const) return lhs->const_value ? lhs : rhs;
    if (rhs->kind == BoolExpr::Kind::Const) return rhs->const_value ? rhs : lhs;
    if (boolExprEqual(lhs, rhs)) return lhs;
    auto out = std::make_shared<BoolExpr>();
    out->kind = BoolExpr::Kind::Or;
    out->lhs = std::move(lhs);
    out->rhs = std::move(rhs);
    return out;
}

using DefinitionByValue = std::vector<const S10Definition*>;

BoolExprPtr boolExprForOperand(const S10PredicateProgram& program,
                               const DefinitionByValue& defs,
                               const S10Operand& operand,
                               std::set<S10ValueId>& expanding);

BoolExprPtr boolExprForValue(const S10PredicateProgram& program,
                             const DefinitionByValue& defs,
                             S10ValueId value,
                             std::set<S10ValueId>& expanding) {
    const auto& info = valueAt(program, value);
    if (!isBoolType(info.type)) {
        fail("S10 readonly check expected a bool value", {},
             "value=" + valueName(program, value));
    }
    if (!expanding.insert(value).second) return boolAtom(value);
    const S10Definition* def = value >= 0 &&
        value < static_cast<S10ValueId>(defs.size()) ? defs[static_cast<std::size_t>(value)] : nullptr;
    if (!def) {
        expanding.erase(value);
        return boolAtom(value);
    }

    BoolExprPtr out = boolAtom(value);
    if (def->kind == S10DefKind::Assign) {
        out = boolExprForOperand(program, defs, def->value, expanding);
    } else if (def->kind == S10DefKind::Op) {
        if (def->op.kind == S10OpKind::LogicalNot && def->op.operands.size() == 1) {
            out = boolNot(boolExprForOperand(program, defs, def->op.operands[0], expanding));
        } else if (def->op.kind == S10OpKind::BoolAnd && def->op.operands.size() == 2) {
            out = boolAnd(boolExprForOperand(program, defs, def->op.operands[0], expanding),
                          boolExprForOperand(program, defs, def->op.operands[1], expanding));
        } else if (def->op.kind == S10OpKind::BoolOr && def->op.operands.size() == 2) {
            out = boolOr(boolExprForOperand(program, defs, def->op.operands[0], expanding),
                         boolExprForOperand(program, defs, def->op.operands[1], expanding));
        }
    }
    expanding.erase(value);
    return out;
}

BoolExprPtr boolExprForOperand(const S10PredicateProgram& program,
                               const DefinitionByValue& defs,
                               const S10Operand& operand,
                               std::set<S10ValueId>& expanding) {
    if (!isBoolType(operand.type)) {
        fail("S10 readonly check expected a bool operand", operand.debug_loc);
    }
    if (operand.kind == S10OperandKind::Literal) {
        bool value = false;
        if (!literalBool(operand, value)) {
            fail("S10 readonly check found malformed bool literal", operand.debug_loc);
        }
        return boolConst(value);
    }
    return boolExprForValue(program, defs, operand.value, expanding);
}

void collectAtoms(const BoolExprPtr& expr, std::set<S10ValueId>& atoms) {
    if (!expr) return;
    switch (expr->kind) {
    case BoolExpr::Kind::Const:
        return;
    case BoolExpr::Kind::Atom:
        atoms.insert(expr->atom);
        return;
    case BoolExpr::Kind::Not:
        collectAtoms(expr->lhs, atoms);
        return;
    case BoolExpr::Kind::And:
    case BoolExpr::Kind::Or:
        collectAtoms(expr->lhs, atoms);
        collectAtoms(expr->rhs, atoms);
        return;
    }
}

bool evalBoolExpr(const BoolExprPtr& expr,
                  const std::unordered_map<S10ValueId, int>& atom_index,
                  std::uint64_t mask) {
    if (!expr) return false;
    switch (expr->kind) {
    case BoolExpr::Kind::Const:
        return expr->const_value;
    case BoolExpr::Kind::Atom: {
        auto it = atom_index.find(expr->atom);
        if (it == atom_index.end()) return false;
        return ((mask >> it->second) & 1U) != 0;
    }
    case BoolExpr::Kind::Not:
        return !evalBoolExpr(expr->lhs, atom_index, mask);
    case BoolExpr::Kind::And:
        return evalBoolExpr(expr->lhs, atom_index, mask) &&
               evalBoolExpr(expr->rhs, atom_index, mask);
    case BoolExpr::Kind::Or:
        return evalBoolExpr(expr->lhs, atom_index, mask) ||
               evalBoolExpr(expr->rhs, atom_index, mask);
    }
    return false;
}

bool implies(const BoolExprPtr& premise, const BoolExprPtr& conclusion) {
    if (boolExprEqual(premise, conclusion)) return true;
    if (conclusion && conclusion->kind == BoolExpr::Kind::Const && conclusion->const_value) return true;
    if (premise && premise->kind == BoolExpr::Kind::Const && !premise->const_value) return true;
    std::set<S10ValueId> atoms;
    collectAtoms(premise, atoms);
    collectAtoms(conclusion, atoms);
    if (atoms.size() > 12) {
        // Too large for this local checker. Avoid rejecting valid designs;
        // later optimization/verification stages can use stronger reasoning.
        return true;
    }
    std::unordered_map<S10ValueId, int> atom_index;
    int index = 0;
    for (S10ValueId atom : atoms) atom_index[atom] = index++;
    std::uint64_t total = std::uint64_t{1} << atoms.size();
    for (std::uint64_t mask = 0; mask < total; ++mask) {
        if (evalBoolExpr(premise, atom_index, mask) &&
            !evalBoolExpr(conclusion, atom_index, mask)) {
            return false;
        }
    }
    return true;
}

struct ReadonlyContext {
    const S10PredicateProgram& program;
    const DefinitionByValue& defs;
};

BoolExprPtr guardExpr(const ReadonlyContext& ctx, const S10Operand& guard) {
    std::set<S10ValueId> expanding;
    return boolExprForOperand(ctx.program, ctx.defs, guard, expanding);
}

BoolExprPtr availabilityExpr(const ReadonlyContext& ctx, S10ValueId value) {
    const auto& info = valueAt(ctx.program, value);
    if (info.kind == S10ValueKind::Initial) return boolConst(true);
    if (value < 0 || value >= static_cast<S10ValueId>(ctx.defs.size()) ||
        !ctx.defs[static_cast<std::size_t>(value)]) {
        fail("S10 readonly check found value without definition", {},
             "value=" + valueName(ctx.program, value));
    }
    return guardExpr(ctx, ctx.defs[static_cast<std::size_t>(value)]->guard);
}

void checkValueUseAvailable(const ReadonlyContext& ctx,
                            S10ValueId value,
                            const BoolExprPtr& use_guard,
                            DebugLoc loc,
                            const std::string& note) {
    BoolExprPtr available = availabilityExpr(ctx, value);
    if (!implies(use_guard, available)) {
        fail("S10 readonly check found guarded read outside value definition coverage",
             loc, note + " value=" + valueName(ctx.program, value));
    }
}

void checkOperandAvailable(const ReadonlyContext& ctx,
                           const S10Operand& operand,
                           const BoolExprPtr& use_guard,
                           const std::string& note) {
    if (operand.kind != S10OperandKind::Value) return;
    checkValueUseAvailable(ctx, operand.value, use_guard, operand.debug_loc, note);
}

void readonlyCheckDefinition(const ReadonlyContext& ctx, const S10Definition& def) {
    BoolExprPtr def_guard = guardExpr(ctx, def.guard);
    checkOperandAvailable(ctx, def.guard, boolConst(true), "definition guard");
    switch (def.kind) {
    case S10DefKind::Assign:
        checkOperandAvailable(ctx, def.value, def_guard, "assign rhs");
        break;
    case S10DefKind::Lookup:
        checkOperandAvailable(ctx, def.lookup_index, def_guard, "lookup index");
        for (const auto& elem : def.lookup_elements) {
            checkOperandAvailable(ctx, elem, def_guard, "lookup element");
        }
        break;
    case S10DefKind::Op:
        if (def.op.kind == S10OpKind::Mux && def.op.operands.size() == 3) {
            const auto& cond = def.op.operands[0];
            const auto& then_value = def.op.operands[1];
            const auto& else_value = def.op.operands[2];
            checkOperandAvailable(ctx, cond, def_guard, "mux condition");
            BoolExprPtr cond_expr = guardExpr(ctx, cond);
            checkOperandAvailable(ctx, then_value, boolAnd(def_guard, cond_expr), "mux then arm");
            checkOperandAvailable(ctx, else_value, boolAnd(def_guard, boolNot(cond_expr)), "mux else arm");
            break;
        }
        for (const auto& operand : def.op.operands) {
            checkOperandAvailable(ctx, operand, def_guard, "op operand");
        }
        break;
    }
}

void readonlyPredicateCheck(const S10PredicateProgram& program,
                            const DefinitionByValue& defs) {
    ReadonlyContext ctx{program, defs};
    for (const auto& def : program.definitions) readonlyCheckDefinition(ctx, def);
    for (const auto& block_guard : program.block_guards) {
        if (block_guard.guard) {
            checkOperandAvailable(ctx, block_guard.guard.value(), boolConst(true), "block guard");
        }
    }
    for (const auto& port : program.ports) {
        if (port.final_guard) {
            checkOperandAvailable(ctx, port.final_guard.value(), boolConst(true), "port final guard");
        }
        if (port.final_value) {
            BoolExprPtr final_guard = port.final_guard
                ? guardExpr(ctx, port.final_guard.value())
                : boolConst(true);
            checkValueUseAvailable(ctx, port.final_value.value(), final_guard, {},
                                   "port final binding");
        } else if (isOutputPort(port)) {
            fail("S10 readonly check found output without final value");
        }
    }
}

} // namespace

void verifyPredicateProgram(const S10PredicateProgram& program) {
    for (std::size_t i = 0; i < program.base_symbols.size(); ++i) {
        const auto& symbol = program.base_symbols[i];
        if (symbol.id != static_cast<SymbolId>(i)) fail("S10 base symbol ids must be dense");
        if (symbol.type.width <= 0) fail("S10 base symbol has invalid type");
    }
    for (std::size_t i = 0; i < program.values.size(); ++i) {
        const auto& value = program.values[i];
        if (value.id != static_cast<S10ValueId>(i)) fail("S10 value ids must be dense");
        if (value.type.width <= 0) fail("S10 value has invalid type");
        if (value.base_symbol >= 0) {
            if (value.base_symbol >= static_cast<SymbolId>(program.base_symbols.size())) {
                fail("S10 value has invalid base symbol");
            }
            if (!typeEq(value.type, program.base_symbols[static_cast<std::size_t>(value.base_symbol)].type)) {
                fail("S10 value type does not match base symbol type");
            }
        }
    }

    std::vector<int> def_count(program.values.size(), 0);
    DefinitionByValue defs(program.values.size(), nullptr);
    std::vector<std::vector<S10ValueId>> deps(program.values.size());
    auto add_dep = [&](S10ValueId target, const S10Operand& operand) {
        if (operand.kind == S10OperandKind::Value) {
            deps[static_cast<std::size_t>(target)].push_back(operand.value);
        }
    };
    for (const auto& def : program.definitions) {
        const auto& target = valueAt(program, def.target);
        ++def_count[static_cast<std::size_t>(def.target)];
        defs[static_cast<std::size_t>(def.target)] = &def;
        verifyOperand(program, def.guard);
        if (!isBoolType(def.guard.type)) fail("S10 definition guard must be bool");
        add_dep(def.target, def.guard);
        switch (def.kind) {
        case S10DefKind::Assign:
            verifyOperand(program, def.value);
            if (!typeEq(def.value.type, target.type)) fail("S10 assign type mismatch");
            add_dep(def.target, def.value);
            break;
        case S10DefKind::Op:
            if (def.op.result_width != target.type.width) {
                fail("S10 op result width does not match target value");
            }
            for (const auto& operand : def.op.operands) {
                verifyOperand(program, operand);
                add_dep(def.target, operand);
            }
            if (def.op.kind == S10OpKind::Mux) {
                if (def.op.operands.size() != 3) fail("S10 mux must have 3 operands");
                if (!isBoolType(def.op.operands[0].type)) fail("S10 mux condition must be bool");
                if (!typeEq(def.op.operands[1].type, def.op.operands[2].type) ||
                    !typeEq(def.op.operands[1].type, target.type)) {
                    fail("S10 mux arm type mismatch");
                }
            }
            break;
        case S10DefKind::Lookup:
            verifyOperand(program, def.lookup_index);
            add_dep(def.target, def.lookup_index);
            for (const auto& elem : def.lookup_elements) {
                verifyOperand(program, elem);
                if (!typeEq(elem.type, target.type)) fail("S10 lookup element type mismatch");
                add_dep(def.target, elem);
            }
            break;
        }
    }
    for (const auto& value : program.values) {
        int count = def_count[static_cast<std::size_t>(value.id)];
        if (value.kind == S10ValueKind::Initial) {
            if (count != 0) fail("S10 initial value must not have a definition");
        } else if (count != 1) {
            fail("S10 non-initial value must have exactly one definition", {},
                 "value=" + value.debug_name + "#" + std::to_string(value.id));
        }
    }
    std::vector<int> color(program.values.size(), 0);
    std::function<void(S10ValueId)> visit = [&](S10ValueId value) {
        auto index = static_cast<std::size_t>(value);
        if (color[index] == 1) fail("S10 value dependency cycle detected");
        if (color[index] == 2) return;
        color[index] = 1;
        for (S10ValueId dep : deps[index]) visit(dep);
        color[index] = 2;
    };
    for (const auto& value : program.values) visit(value.id);
    for (const auto& block_guard : program.block_guards) {
        if (block_guard.guard) {
            verifyOperand(program, block_guard.guard.value());
            if (!isBoolType(block_guard.guard->type)) fail("S10 block guard must be bool");
        }
    }
    for (const auto& port : program.ports) {
        if (port.initial_value) (void)valueAt(program, port.initial_value.value());
        if (port.final_value) {
            const auto& final_value = valueAt(program, port.final_value.value());
            if (port.symbol >= 0 &&
                port.symbol < static_cast<SymbolId>(program.base_symbols.size()) &&
                !typeEq(final_value.type, program.base_symbols[static_cast<std::size_t>(port.symbol)].type)) {
                fail("S10 port final value type mismatch");
            }
        } else if (isOutputPort(port)) {
            fail("S10 output port is missing final value");
        }
        if (port.final_guard) {
            verifyOperand(program, port.final_guard.value());
            if (!isBoolType(port.final_guard->type)) fail("S10 final guard must be bool");
        }
    }
    readonlyPredicateCheck(program, defs);
}

std::string debugPrint(const S10PredicateProgram& program,
                       const std::vector<PredicateSummary>& summaries) {
    std::ostringstream os;
    os << "s10predicate\n";
    for (const auto& summary : summaries) {
        os << "summary function=" << summary.function_name
           << " values=" << summary.values
           << " definitions=" << summary.definitions
           << " block_guards=" << summary.block_guards
           << " lowered_phis=" << summary.lowered_phis
           << " generated_guards=" << summary.generated_guards
           << " ignored_unreachable_blocks=" << summary.ignored_unreachable_blocks << "\n";
    }
    os << "top " << program.name << "\n";
    os << "symbols\n";
    for (const auto& symbol : program.base_symbols) {
        os << "  %" << symbol.id << " " << symbol.debug_name
           << " " << typeText(symbol.type) << "\n";
    }
    os << "values\n";
    for (const auto& value : program.values) {
        os << "  $" << value.id << " " << valueName(program, value.id)
           << " " << typeText(value.type)
           << " kind=" << valueKindName(value.kind);
        if (value.source_block >= 0) os << " source=bb" << value.source_block;
        os << "\n";
    }
    os << "block_guards\n";
    for (const auto& guard : program.block_guards) {
        os << "  bb" << guard.block;
        if (!guard.reachable) {
            os << " unreachable\n";
        } else {
            os << " guard=" << operandText(program, guard.guard.value()) << "\n";
        }
    }
    os << "definitions\n";
    for (const auto& def : program.definitions) {
        os << "  " << defText(program, def) << "\n";
    }
    os << "ports\n";
    for (const auto& port : program.ports) {
        os << "  " << symbolName(program, port.symbol);
        if (port.initial_value) os << " initial=" << valueName(program, port.initial_value.value());
        if (port.final_value) os << " final=" << valueName(program, port.final_value.value());
        if (port.final_guard) os << " final_guard=" << operandText(program, port.final_guard.value());
        os << "\n";
    }
    return os.str();
}

PredicateResult lowerPredicates(const S9SSAProgram& program,
                                const PredicateOptions& options) {
    try {
        PredicateResult result;
        PredicateSummary summary;
        S10PredicateProgram out = lowerFunction(program.top, options, summary);
        verifyPredicateProgram(out);
        result.summaries.push_back(summary);
        if (options.debug_print) result.debug_text = debugPrint(out, result.summaries);
        result.program = std::move(out);
        return result;
    } catch (const RTLZZException& ex) {
        PredicateResult result;
        PredicateError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        PredicateResult result;
        PredicateError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

S10PredicateProgram lowerPredicatesOrThrow(const S9SSAProgram& program,
                                           const PredicateOptions& options) {
    auto result = lowerPredicates(program, options);
    if (!result.ok()) throw RTLZZException(result.error->context, result.error->message);
    return std::move(result.program.value());
}

} // namespace pred::s10predicate
