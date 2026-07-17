#include "s5unroll/S5Unroll.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pred::s5unroll {
namespace {

using namespace pred::s3statementize;
using namespace pred::s4cfg;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s5unroll";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

bool isIntegerType(const TypeInfo& type) {
    if (type.name == "bool" || type.hw_kind == "bool") return false;
    if (type.is_array || !type.struct_name.empty() || type.is_pointer ||
        type.is_reference) {
        return false;
    }
    return type.is_hw_int || type.hw_kind == "builtin" ||
           type.name == "int" || type.name == "uint32_t" ||
           type.name == "uint8_t" || type.name == "unsigned" ||
           type.name == "unsigned int" || type.name == "long" ||
           type.name == "unsigned long" || type.width > 0;
}

int typeWidth(const TypeInfo& type) {
    if (type.width > 0) return type.width;
    if (type.name == "uint8_t") return 8;
    if (type.name == "int" || type.name == "uint32_t" ||
        type.name == "unsigned" || type.name == "unsigned int" ||
        type.hw_kind == "builtin") {
        return 32;
    }
    return 64;
}

std::uint64_t maskForWidth(int width) {
    if (width <= 0 || width >= 64) return std::numeric_limits<std::uint64_t>::max();
    return (std::uint64_t{1} << width) - 1;
}

std::uint64_t wrapValue(std::int64_t value, const TypeInfo& type) {
    int width = typeWidth(type);
    return static_cast<std::uint64_t>(value) & maskForWidth(width);
}

std::int64_t signedValue(std::uint64_t value, const TypeInfo& type) {
    int width = typeWidth(type);
    if (!type.is_signed || width <= 0 || width >= 64) {
        return static_cast<std::int64_t>(value);
    }
    std::uint64_t sign = std::uint64_t{1} << (width - 1);
    std::uint64_t mask = maskForWidth(width);
    value &= mask;
    if (!(value & sign)) return static_cast<std::int64_t>(value);
    return -static_cast<std::int64_t>((~value + 1) & mask);
}

std::string literalForValue(std::uint64_t value, const TypeInfo& type) {
    if (type.is_signed) return std::to_string(signedValue(value, type));
    return std::to_string(value & maskForWidth(typeWidth(type)));
}

std::optional<std::int64_t> parseLiteral(const std::string& text) {
    if (text == "true") return 1;
    if (text == "false") return 0;
    try {
        std::size_t pos = 0;
        auto value = std::stoll(text, &pos, 0);
        if (pos == text.size()) return value;
    } catch (...) {
    }
    return std::nullopt;
}

s3statementize::SymbolId operandSymbol(const Operand& op) {
    if (op.kind == OperandKind::Var) return op.var_symbol;
    if (op.kind == OperandKind::LValueRead && op.lvalue.accesses.empty()) {
        return op.lvalue.root_symbol;
    }
    return -1;
}

Operand literalOperand(std::uint64_t value, TypeInfo type) {
    Operand out;
    out.kind = OperandKind::Literal;
    out.type = type;
    out.literal_value = literalForValue(value, type);
    return out;
}

struct ConstValue {
    std::uint64_t value = 0;
    TypeInfo type;
};

struct ConstEnv {
    std::unordered_map<s3statementize::SymbolId, ConstValue> values;
};

std::optional<ConstValue> constantOf(const Operand& op, const ConstEnv& env) {
    if (op.kind == OperandKind::Literal) {
        auto parsed = parseLiteral(op.literal_value);
        if (!parsed) return std::nullopt;
        return ConstValue{wrapValue(*parsed, op.type), op.type};
    }
    auto symbol = operandSymbol(op);
    if (symbol >= 0) {
        auto it = env.values.find(symbol);
        if (it != env.values.end()) return it->second;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> applyBinary(BinaryOp op,
                                         std::uint64_t lhs,
                                         std::uint64_t rhs,
                                         const TypeInfo& type) {
    std::uint64_t out = 0;
    switch (op) {
    case BinaryOp::Add: out = lhs + rhs; break;
    case BinaryOp::Sub: out = lhs - rhs; break;
    case BinaryOp::Mul: out = lhs * rhs; break;
    case BinaryOp::Div:
        if (rhs == 0) return std::nullopt;
        out = lhs / rhs;
        break;
    case BinaryOp::Mod:
        if (rhs == 0) return std::nullopt;
        out = lhs % rhs;
        break;
    case BinaryOp::BitAnd: out = lhs & rhs; break;
    case BinaryOp::BitOr: out = lhs | rhs; break;
    case BinaryOp::BitXor: out = lhs ^ rhs; break;
    case BinaryOp::Shl: out = lhs << rhs; break;
    case BinaryOp::Shr: out = lhs >> rhs; break;
    default:
        return std::nullopt;
    }
    return out & maskForWidth(typeWidth(type));
}

std::optional<ConstValue> evalOp(const OpExpr& op, const ConstEnv& env) {
    if (op.kind == OpExpr::Kind::Cast && op.operands.size() == 1) {
        auto value = constantOf(op.operands[0], env);
        if (!value) return std::nullopt;
        return ConstValue{wrapValue(static_cast<std::int64_t>(value->value),
                                    op.cast_type),
                          op.cast_type};
    }
    if (op.kind != OpExpr::Kind::Binary || op.operands.size() != 2) {
        return std::nullopt;
    }
    auto lhs = constantOf(op.operands[0], env);
    auto rhs = constantOf(op.operands[1], env);
    if (!lhs || !rhs) return std::nullopt;
    auto value = applyBinary(op.binary_op, lhs->value, rhs->value, op.type);
    if (!value) return std::nullopt;
    return ConstValue{*value, op.type};
}

void updateConstEnv(const CFGStmt& stmt, ConstEnv& env) {
    if (!stmt.stmt) return;
    const auto& s = *stmt.stmt;
    if (s.kind == S3StmtKind::Decl) {
        env.values.erase(s.decl_symbol);
    } else if (s.kind == S3StmtKind::Construct && s.args.size() == 1) {
        auto value = constantOf(s.args[0], env);
        if (value) env.values[s.target.root_symbol] = ConstValue{value->value, s.target.type};
        else env.values.erase(s.target.root_symbol);
    } else if (s.kind == S3StmtKind::Assign && s.target.accesses.empty()) {
        auto value = constantOf(s.value, env);
        if (value) env.values[s.target.root_symbol] = ConstValue{value->value, s.target.type};
        else env.values.erase(s.target.root_symbol);
    } else if (s.kind == S3StmtKind::Op && s.target.accesses.empty()) {
        auto value = evalOp(s.op, env);
        if (value) env.values[s.target.root_symbol] = *value;
        else env.values.erase(s.target.root_symbol);
    } else if (s.kind == S3StmtKind::Call && s.call_result &&
               s.call_result->accesses.empty()) {
        env.values.erase(s.call_result->root_symbol);
    }
}

void updateConstEnv(const BasicBlock& block, ConstEnv& env) {
    for (const auto& stmt : block.stmts) updateConstEnv(stmt, env);
}

s4cfg::CFGProgram cloneProgram(const s4cfg::CFGProgram& input);

s4cfg::FunctionCFG cloneFunction(const s4cfg::FunctionCFG& input) {
    s4cfg::FunctionCFG out;
    out.name = input.name;
    out.return_type = input.return_type;
    out.params = input.params;
    out.symbols = input.symbols;
    out.entry = input.entry;
    out.exit = input.exit;
    out.loop_regions = input.loop_regions;
    out.return_slot = input.return_slot;
    out.return_slot_symbol = input.return_slot_symbol;
    for (const auto& block : input.blocks) {
        out.blocks.push_back(std::make_unique<BasicBlock>(*block));
    }
    return out;
}

s4cfg::CFGProgram cloneProgram(const s4cfg::CFGProgram& input) {
    s4cfg::CFGProgram out;
    out.top = cloneFunction(input.top);
    for (const auto& helper : input.helpers) out.helpers.push_back(cloneFunction(helper));
    for (const auto& [name, lambda] : input.lambdas) {
        out.lambdas.emplace(name, cloneFunction(lambda));
    }
    out.struct_fields = input.struct_fields;
    out.struct_constructors = input.struct_constructors;
    out.helper_index = input.helper_index;
    out.lambda_index = input.lambda_index;
    return out;
}

bool hasLoop(const BasicBlock& block, LoopRegionId id) {
    return std::find(block.loop_stack.begin(), block.loop_stack.end(), id) !=
           block.loop_stack.end();
}

std::set<BlockId> blocksInLoop(const FunctionCFG& fn, LoopRegionId id) {
    std::set<BlockId> out;
    for (const auto& block : fn.blocks) {
        if (hasLoop(*block, id)) out.insert(block->id);
    }
    return out;
}

std::vector<CFGEdge> allOutgoingEdges(const BasicBlock& block) {
    return block.successors;
}

std::vector<CFGEdge> allIncomingEdges(const BasicBlock& block) {
    return block.predecessors;
}

void clearEdges(FunctionCFG& fn) {
    for (auto& block : fn.blocks) {
        block->successors.clear();
        block->predecessors.clear();
    }
}

void addEdge(FunctionCFG& fn, BlockId from, BlockId to, EdgeKind kind, std::string label,
             std::optional<Operand> case_value = std::nullopt) {
    if (from < 0 || to < 0 ||
        from >= static_cast<BlockId>(fn.blocks.size()) ||
        to >= static_cast<BlockId>(fn.blocks.size())) {
        fail("Invalid CFG edge after unroll");
    }
    CFGEdge edge;
    edge.from = from;
    edge.to = to;
    edge.kind = kind;
    edge.label = std::move(label);
    edge.case_value = std::move(case_value);
    fn.blocks[static_cast<std::size_t>(from)]->successors.push_back(edge);
    fn.blocks[static_cast<std::size_t>(to)]->predecessors.push_back(std::move(edge));
}

void rebuildEdges(FunctionCFG& fn) {
    clearEdges(fn);
    for (const auto& block_ptr : fn.blocks) {
        const auto& block = *block_ptr;
        switch (block.terminator.kind) {
        case TermKind::Jump:
            addEdge(fn, block.id, block.terminator.jump_target, EdgeKind::Jump, "jump");
            break;
        case TermKind::Branch:
            addEdge(fn, block.id, block.terminator.true_target, EdgeKind::True, "true");
            addEdge(fn, block.id, block.terminator.false_target, EdgeKind::False, "false");
            break;
        case TermKind::Switch:
            for (const auto& target : block.terminator.switch_targets) {
                addEdge(fn, block.id, target.target, EdgeKind::Case, "case",
                        target.value);
            }
            addEdge(fn, block.id, block.terminator.default_target, EdgeKind::Default,
                    "default");
            break;
        case TermKind::Return:
            addEdge(fn, block.id, fn.exit, EdgeKind::Return, "return");
            break;
        case TermKind::Exit:
        case TermKind::Unreachable:
            break;
        }
    }
}

std::set<BlockId> reachableBlocks(const FunctionCFG& fn) {
    std::set<BlockId> reachable;
    if (fn.entry < 0 || fn.entry >= static_cast<BlockId>(fn.blocks.size())) {
        return reachable;
    }
    std::deque<BlockId> work;
    work.push_back(fn.entry);
    reachable.insert(fn.entry);
    while (!work.empty()) {
        auto id = work.front();
        work.pop_front();
        for (const auto& edge : fn.blocks[static_cast<std::size_t>(id)]->successors) {
            if (reachable.insert(edge.to).second) work.push_back(edge.to);
        }
    }
    return reachable;
}

BlockId remapTarget(const std::unordered_map<BlockId, BlockId>& map,
                    BlockId target) {
    auto it = map.find(target);
    return it == map.end() ? target : it->second;
}

void remapTerminator(Terminator& term,
                     const std::unordered_map<BlockId, BlockId>& map) {
    term.jump_target = remapTarget(map, term.jump_target);
    term.true_target = remapTarget(map, term.true_target);
    term.false_target = remapTarget(map, term.false_target);
    term.default_target = remapTarget(map, term.default_target);
    for (auto& target : term.switch_targets) {
        target.target = remapTarget(map, target.target);
    }
}

void compactReachable(FunctionCFG& fn) {
    rebuildEdges(fn);
    auto reachable = reachableBlocks(fn);
    std::unordered_map<BlockId, BlockId> id_map;
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    for (const auto& old_block : fn.blocks) {
        if (!reachable.count(old_block->id)) continue;
        BlockId next = static_cast<BlockId>(blocks.size());
        id_map[old_block->id] = next;
        auto cloned = std::make_unique<BasicBlock>(*old_block);
        cloned->id = next;
        cloned->successors.clear();
        cloned->predecessors.clear();
        blocks.push_back(std::move(cloned));
    }
    fn.blocks = std::move(blocks);
    fn.entry = remapTarget(id_map, fn.entry);
    fn.exit = remapTarget(id_map, fn.exit);
    for (auto& block : fn.blocks) remapTerminator(block->terminator, id_map);
    for (auto& region : fn.loop_regions) {
        region.init = remapTarget(id_map, region.init);
        region.condition = remapTarget(id_map, region.condition);
        region.condition_prelude = remapTarget(id_map, region.condition_prelude);
        region.body = remapTarget(id_map, region.body);
        region.exit = remapTarget(id_map, region.exit);
    }
    rebuildEdges(fn);
}

struct LoopAnalysis {
    s3statementize::SymbolId var = -1;
    TypeInfo type;
    std::uint64_t init = 0;
    BinaryOp compare = BinaryOp::Lt;
    std::uint64_t bound = 0;
    std::int64_t step_delta = 0;
    std::unordered_map<BlockId, std::int64_t> body_entry_deltas;
    std::vector<std::uint64_t> values;
};

std::optional<s3statementize::SymbolId> assignedRoot(const CFGStmt& stmt) {
    if (!stmt.stmt) return std::nullopt;
    const auto& s = *stmt.stmt;
    if (s.kind == S3StmtKind::Assign && s.target.accesses.empty()) return s.target.root_symbol;
    if (s.kind == S3StmtKind::Construct && s.target.accesses.empty()) return s.target.root_symbol;
    if (s.kind == S3StmtKind::Op && s.target.accesses.empty()) return s.target.root_symbol;
    return std::nullopt;
}

std::optional<ConstValue> assignedConstant(const CFGStmt& stmt, const ConstEnv& env) {
    if (!stmt.stmt) return std::nullopt;
    const auto& s = *stmt.stmt;
    if (s.kind == S3StmtKind::Assign) return constantOf(s.value, env);
    if (s.kind == S3StmtKind::Construct && s.args.size() == 1) {
        return constantOf(s.args[0], env);
    }
    if (s.kind == S3StmtKind::Op) return evalOp(s.op, env);
    return std::nullopt;
}

struct DeltaValue {
    enum class Kind {
        Constant,
        Affine,
    };

    Kind kind = Kind::Constant;
    std::int64_t constant = 0;
    std::int64_t delta = 0;
    TypeInfo type;
};

struct DeltaState {
    std::int64_t induction_delta = 0;
    std::unordered_map<s3statementize::SymbolId, DeltaValue> values;
};

std::optional<DeltaValue> literalDeltaValue(const Operand& operand) {
    if (operand.kind != OperandKind::Literal) return std::nullopt;
    auto parsed = parseLiteral(operand.literal_value);
    if (!parsed) return std::nullopt;
    DeltaValue out;
    out.kind = DeltaValue::Kind::Constant;
    out.constant = *parsed;
    out.type = operand.type;
    return out;
}

std::optional<DeltaValue> operandDeltaValue(const Operand& operand,
                                            const DeltaState& state,
                                            s3statementize::SymbolId induction_symbol) {
    if (auto literal = literalDeltaValue(operand)) return literal;
    auto symbol = operandSymbol(operand);
    if (symbol == induction_symbol) {
        DeltaValue out;
        out.kind = DeltaValue::Kind::Affine;
        out.delta = state.induction_delta;
        out.type = operand.type;
        return out;
    }
    if (symbol >= 0) {
        auto it = state.values.find(symbol);
        if (it != state.values.end()) return it->second;
    }
    return std::nullopt;
}

std::optional<DeltaValue> evalDeltaOp(const OpExpr& op,
                                      const DeltaState& state,
                                      s3statementize::SymbolId induction_symbol) {
    if (op.kind == OpExpr::Kind::Cast && op.operands.size() == 1) {
        auto value = operandDeltaValue(op.operands[0], state, induction_symbol);
        if (!value) return std::nullopt;
        value->type = op.cast_type;
        return value;
    }
    if (op.kind != OpExpr::Kind::Binary || op.operands.size() != 2) {
        return std::nullopt;
    }
    auto lhs = operandDeltaValue(op.operands[0], state, induction_symbol);
    auto rhs = operandDeltaValue(op.operands[1], state, induction_symbol);
    if (!lhs || !rhs) return std::nullopt;

    auto make_affine = [&](std::int64_t delta) {
        DeltaValue out;
        out.kind = DeltaValue::Kind::Affine;
        out.delta = delta;
        out.type = op.type;
        return out;
    };
    auto make_const = [&](std::int64_t value) {
        DeltaValue out;
        out.kind = DeltaValue::Kind::Constant;
        out.constant = value;
        out.type = op.type;
        return out;
    };

    if (lhs->kind == DeltaValue::Kind::Constant &&
        rhs->kind == DeltaValue::Kind::Constant) {
        switch (op.binary_op) {
        case BinaryOp::Add: return make_const(lhs->constant + rhs->constant);
        case BinaryOp::Sub: return make_const(lhs->constant - rhs->constant);
        case BinaryOp::Mul: return make_const(lhs->constant * rhs->constant);
        default: return std::nullopt;
        }
    }

    if (lhs->kind == DeltaValue::Kind::Affine &&
        rhs->kind == DeltaValue::Kind::Constant) {
        switch (op.binary_op) {
        case BinaryOp::Add: return make_affine(lhs->delta + rhs->constant);
        case BinaryOp::Sub: return make_affine(lhs->delta - rhs->constant);
        default: return std::nullopt;
        }
    }

    if (lhs->kind == DeltaValue::Kind::Constant &&
        rhs->kind == DeltaValue::Kind::Affine) {
        if (op.binary_op == BinaryOp::Add) {
            return make_affine(rhs->delta + lhs->constant);
        }
        return std::nullopt;
    }

    return std::nullopt;
}

bool sameDeltaValue(const DeltaValue& lhs, const DeltaValue& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.constant == rhs.constant &&
           lhs.delta == rhs.delta;
}

bool mergeDeltaState(DeltaState& into, const DeltaState& incoming) {
    if (into.induction_delta != incoming.induction_delta) {
        fail("Cannot statically analyze loop: induction variable update is path-dependent");
    }
    bool changed = false;
    for (auto it = into.values.begin(); it != into.values.end();) {
        auto incoming_it = incoming.values.find(it->first);
        if (incoming_it == incoming.values.end() ||
            !sameDeltaValue(it->second, incoming_it->second)) {
            it = into.values.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

void processDeltaStmt(const CFGStmt& stmt,
                      DeltaState& state,
                      s3statementize::SymbolId induction_symbol) {
    if (!stmt.stmt) return;
    const auto& s = *stmt.stmt;

    auto erase_target = [&](const LValue& target) {
        if (target.root_symbol >= 0) state.values.erase(target.root_symbol);
    };
    auto assign_target = [&](const LValue& target, std::optional<DeltaValue> value) {
        if (target.root_symbol == induction_symbol) {
            if (!target.accesses.empty() || !value ||
                value->kind != DeltaValue::Kind::Affine) {
                fail("Cannot statically analyze loop: induction variable write is not a fixed affine update");
            }
            state.induction_delta = value->delta;
            return;
        }
        if (!target.accesses.empty()) {
            erase_target(target);
            return;
        }
        if (value) state.values[target.root_symbol] = *value;
        else erase_target(target);
    };

    switch (s.kind) {
    case S3StmtKind::Decl:
        state.values.erase(s.decl_symbol);
        return;
    case S3StmtKind::Assign:
        assign_target(s.target, operandDeltaValue(s.value, state, induction_symbol));
        return;
    case S3StmtKind::Construct:
        if (s.args.size() == 1) {
            assign_target(s.target, operandDeltaValue(s.args[0], state, induction_symbol));
        } else {
            assign_target(s.target, std::nullopt);
        }
        return;
    case S3StmtKind::Op:
        assign_target(s.target, evalDeltaOp(s.op, state, induction_symbol));
        return;
    case S3StmtKind::Call:
        if (s.call_result) assign_target(s.call_result.value(), std::nullopt);
        return;
    case S3StmtKind::Eval:
        return;
    default:
        fail("Internal error: non-sequential statement in s5 loop delta analysis");
    }
}

bool isBackedgeTarget(const LoopRegion& region, BlockId target) {
    return target == region.condition_prelude || target == region.condition;
}

bool isLoopBodyAnalysisBlock(const FunctionCFG& fn,
                             const LoopRegion& region,
                             BlockId block_id) {
    if (block_id < 0 || block_id >= static_cast<BlockId>(fn.blocks.size())) return false;
    if (block_id == region.condition || block_id == region.condition_prelude) return false;
    return hasLoop(*fn.blocks[static_cast<std::size_t>(block_id)], region.id);
}

struct BodyDeltaAnalysis {
    bool has_backedge = false;
    std::int64_t backedge_delta = 0;
    std::unordered_map<BlockId, std::int64_t> entry_deltas;
};

BodyDeltaAnalysis analyzeLoopBodyDelta(const FunctionCFG& fn,
                                       const LoopRegion& region,
                                       s3statementize::SymbolId induction_symbol) {
    if (!isLoopBodyAnalysisBlock(fn, region, region.body)) {
        fail("Cannot statically analyze loop: loop body entry is missing");
    }

    std::vector<std::optional<DeltaState>> in_states(fn.blocks.size());
    std::deque<BlockId> work;
    DeltaState initial;
    in_states[static_cast<std::size_t>(region.body)] = initial;
    work.push_back(region.body);

    std::optional<std::int64_t> backedge_delta;
    int visits = 0;
    while (!work.empty()) {
        if (++visits > static_cast<int>(fn.blocks.size()) * 16) {
            fail("Cannot statically analyze loop: loop body dataflow did not converge");
        }
        BlockId id = work.front();
        work.pop_front();
        auto state = in_states[static_cast<std::size_t>(id)].value();
        const auto& block = *fn.blocks[static_cast<std::size_t>(id)];
        for (const auto& stmt : block.stmts) {
            processDeltaStmt(stmt, state, induction_symbol);
        }

        for (const auto& edge : block.successors) {
            if (isBackedgeTarget(region, edge.to)) {
                if (!backedge_delta) {
                    backedge_delta = state.induction_delta;
                } else if (*backedge_delta != state.induction_delta) {
                    fail("Cannot statically analyze loop: different backedge paths produce different induction updates");
                }
                continue;
            }
            if (!isLoopBodyAnalysisBlock(fn, region, edge.to)) {
                continue;
            }
            auto& target_state = in_states[static_cast<std::size_t>(edge.to)];
            if (!target_state) {
                target_state = state;
                work.push_back(edge.to);
                continue;
            }
            if (mergeDeltaState(target_state.value(), state)) {
                work.push_back(edge.to);
            }
        }
    }

    if (backedge_delta && *backedge_delta == 0) {
        fail("Cannot statically analyze loop: induction variable is not updated on backedge paths");
    }
    BodyDeltaAnalysis analysis;
    analysis.has_backedge = backedge_delta.has_value();
    analysis.backedge_delta = backedge_delta.value_or(0);
    for (std::size_t i = 0; i < in_states.size(); ++i) {
        if (in_states[i]) {
            analysis.entry_deltas[static_cast<BlockId>(i)] =
                in_states[i]->induction_delta;
        }
    }
    return analysis;
}

std::vector<CFGEdge> externalPredecessors(const FunctionCFG& fn,
                                          const LoopRegion& region,
                                          BlockId target) {
    std::vector<CFGEdge> out;
    if (target < 0 || target >= static_cast<BlockId>(fn.blocks.size())) return out;
    const auto& block = *fn.blocks[static_cast<std::size_t>(target)];
    for (const auto& pred : block.predecessors) {
        if (pred.from < 0 || pred.from >= static_cast<BlockId>(fn.blocks.size())) continue;
        if (hasLoop(*fn.blocks[static_cast<std::size_t>(pred.from)], region.id)) continue;
        out.push_back(pred);
    }
    return out;
}

bool evalLoopCondition(BinaryOp compare,
                       std::uint64_t value,
                       std::uint64_t bound,
                       const TypeInfo& type) {
    std::int64_t a = signedValue(value, type);
    std::int64_t b = signedValue(bound, type);
    switch (compare) {
    case BinaryOp::Lt: return a < b;
    case BinaryOp::Le: return a <= b;
    case BinaryOp::Gt: return a > b;
    case BinaryOp::Ge: return a >= b;
    case BinaryOp::Eq: return a == b;
    case BinaryOp::Ne: return a != b;
    default: return false;
    }
}

std::uint64_t advanceInduction(std::uint64_t value,
                               std::int64_t delta,
                               const TypeInfo& type) {
    return wrapValue(signedValue(value, type) + delta, type);
}

std::optional<LoopAnalysis> analyzeLoop(const FunctionCFG& fn,
                                        const LoopRegion& region,
                                        const UnrollOptions& options) {
    const auto& header = *fn.blocks[static_cast<std::size_t>(region.condition)];
    const auto& prelude = *fn.blocks[static_cast<std::size_t>(region.condition_prelude)];
    if (header.terminator.kind != TermKind::Branch) {
        fail("Cannot statically analyze loop: condition block is not a branch");
    }
    if (!prelude.stmts.empty()) {
        for (const auto& stmt : prelude.stmts) {
            if (stmt.stmt && stmt.stmt->kind == S3StmtKind::Call) {
                fail("Cannot statically analyze loop: condition prelude contains a call");
            }
            if (stmt.stmt && stmt.stmt->kind == S3StmtKind::Construct &&
                stmt.stmt->callee != "Int" &&
                stmt.stmt->callee != "bool" &&
                stmt.stmt->callee.rfind("Int<", 0) != 0) {
                fail("Cannot statically analyze loop: condition prelude contains a constructor with possible side effects");
            }
        }
    }

    ConstEnv header_env;
    BlockId init_target = region.condition_kind == LoopConditionKind::PostTest
        ? region.body
        : region.condition_prelude;
    for (const auto& pred : externalPredecessors(fn, region, init_target)) {
        ConstEnv env;
        updateConstEnv(*fn.blocks[static_cast<std::size_t>(pred.from)], env);
        for (const auto& [symbol, value] : env.values) header_env.values[symbol] = value;
    }
    updateConstEnv(prelude, header_env);

    s3statementize::SymbolId cond_var = operandSymbol(header.terminator.condition);
    const OpExpr* cond_op = nullptr;
    for (const auto& stmt : prelude.stmts) {
        if (!stmt.stmt || stmt.stmt->kind != S3StmtKind::Op) continue;
        if (stmt.stmt->target.root_symbol == cond_var &&
            stmt.stmt->op.kind == OpExpr::Kind::Binary) {
            cond_op = &stmt.stmt->op;
        }
    }
    if (!cond_op || cond_op->operands.size() != 2) {
        fail("Cannot statically analyze loop: condition is not a simple comparison op");
    }
    BinaryOp cmp = cond_op->binary_op;
    if (cmp != BinaryOp::Lt && cmp != BinaryOp::Le && cmp != BinaryOp::Gt &&
        cmp != BinaryOp::Ge && cmp != BinaryOp::Eq && cmp != BinaryOp::Ne) {
        fail("Cannot statically analyze loop: condition op is not a comparison");
    }

    s3statementize::SymbolId lhs_var = operandSymbol(cond_op->operands[0]);
    s3statementize::SymbolId rhs_var = operandSymbol(cond_op->operands[1]);
    s3statementize::SymbolId var = -1;
    std::string var_name;
    ConstValue bound;
    bool var_on_lhs = false;
    if (lhs_var >= 0) {
        auto rhs_const = constantOf(cond_op->operands[1], header_env);
        if (rhs_const) {
            var = lhs_var;
            bound = *rhs_const;
            var_on_lhs = true;
        }
    }
    if (var < 0 && rhs_var >= 0) {
        auto lhs_const = constantOf(cond_op->operands[0], header_env);
        if (lhs_const) {
            var = rhs_var;
            bound = *lhs_const;
            var_on_lhs = false;
        }
    }
    if (var < 0) fail("Cannot statically analyze loop: condition has no variable/constant pair");
    if (!var_on_lhs) {
        switch (cmp) {
        case BinaryOp::Lt: cmp = BinaryOp::Gt; break;
        case BinaryOp::Le: cmp = BinaryOp::Ge; break;
        case BinaryOp::Gt: cmp = BinaryOp::Lt; break;
        case BinaryOp::Ge: cmp = BinaryOp::Le; break;
        default: break;
        }
    }

    ConstValue init;
    bool found_init = false;
    for (const auto& pred : externalPredecessors(fn, region, init_target)) {
        const auto& pre = *fn.blocks[static_cast<std::size_t>(pred.from)];
        ConstEnv env;
        for (const auto& stmt : pre.stmts) {
            auto root = assignedRoot(stmt);
            if (root && *root == var) {
                auto value = assignedConstant(stmt, env);
                if (!value) fail("Cannot statically analyze loop: induction init is not constant");
                init = *value;
                found_init = true;
            }
            updateConstEnv(stmt, env);
        }
    }
    if (!found_init) {
        auto it = header_env.values.find(var);
        if (it != header_env.values.end()) {
            init = it->second;
            found_init = true;
        }
    }
    if (!found_init) fail("Cannot statically analyze loop: missing constant induction init");
    if (!isIntegerType(init.type)) {
        fail("Cannot statically analyze loop: induction variable is not an integer type");
    }

    auto body_delta = analyzeLoopBodyDelta(fn, region, var);

    LoopAnalysis analysis;
    analysis.var = var;
    analysis.type = init.type;
    analysis.init = init.value;
    analysis.compare = cmp;
    analysis.bound = bound.value;
    analysis.step_delta = body_delta.backedge_delta;
    analysis.body_entry_deltas = std::move(body_delta.entry_deltas);

    auto cond = [&](std::uint64_t value) {
        return evalLoopCondition(analysis.compare, value, analysis.bound, analysis.type);
    };
    auto step_next = [&](std::uint64_t value) {
        return advanceInduction(value, analysis.step_delta, analysis.type);
    };

    std::uint64_t value = analysis.init;
    std::unordered_set<std::uint64_t> seen;
    if (!body_delta.has_backedge) {
        if (region.condition_kind == LoopConditionKind::PostTest || cond(value)) {
            analysis.values.push_back(value);
        }
    } else if (region.condition_kind == LoopConditionKind::PreTest) {
        for (int i = 0; cond(value); ++i) {
            if (i >= options.max_iterations_per_loop) {
                fail("Loop iteration count exceeds max_iterations_per_loop");
            }
            if (!seen.insert(value).second) {
                fail("Cannot statically analyze loop: induction recurrence cycles before condition becomes false");
            }
            analysis.values.push_back(value);
            value = step_next(value);
        }
    } else {
        for (int i = 0;; ++i) {
            if (i >= options.max_iterations_per_loop) {
                fail("Loop iteration count exceeds max_iterations_per_loop");
            }
            if (!seen.insert(value).second) {
                fail("Cannot statically analyze loop: induction recurrence cycles before condition becomes false");
            }
            analysis.values.push_back(value);
            value = step_next(value);
            if (!cond(value)) break;
        }
    }
    return analysis;
}

void replaceOperand(Operand& operand, s3statementize::SymbolId var, const Operand& value);

Operand deepCopyOperand(const Operand& operand);

LValue deepCopyLValue(const LValue& lvalue) {
    LValue out = lvalue;
    out.accesses.clear();
    out.accesses.reserve(lvalue.accesses.size());
    for (const auto& access : lvalue.accesses) {
        LValueAccess copied = access;
        if (access.index) {
            copied.index = std::make_shared<Operand>(deepCopyOperand(*access.index));
        }
        out.accesses.push_back(std::move(copied));
    }
    return out;
}

Operand deepCopyOperand(const Operand& operand) {
    Operand out = operand;
    if (operand.kind == OperandKind::LValueRead) {
        out.lvalue = deepCopyLValue(operand.lvalue);
    }
    return out;
}

void replaceLValue(LValue& lvalue, s3statementize::SymbolId var, const Operand& value) {
    (void)var;
    (void)value;
    for (auto& access : lvalue.accesses) {
        if (access.index) replaceOperand(*access.index, var, value);
    }
}

void replaceOperand(Operand& operand, s3statementize::SymbolId var, const Operand& value) {
    if (operand.kind == OperandKind::Var && operand.var_symbol == var) {
        operand = value;
        return;
    }
    if (operand.kind == OperandKind::LValueRead &&
        operand.lvalue.root_symbol == var && operand.lvalue.accesses.empty()) {
        operand = value;
        return;
    }
    if (operand.kind == OperandKind::LValueRead) {
        replaceLValue(operand.lvalue, var, value);
    }
}

S3StmtPtr cloneAndRewriteStmt(const S3StmtPtr& stmt,
                              s3statementize::SymbolId var,
                              const Operand& value,
                              bool replace_targets) {
    if (!stmt) return nullptr;
    auto out = std::make_shared<S3Stmt>(*stmt);
    out->target = deepCopyLValue(out->target);
    out->value = deepCopyOperand(out->value);
    for (auto& operand : out->op.operands) operand = deepCopyOperand(operand);
    if (out->call_result) out->call_result = deepCopyLValue(out->call_result.value());
    for (auto& arg : out->args) arg = deepCopyOperand(arg);
    out->condition = deepCopyOperand(out->condition);
    if (out->for_cond) out->for_cond = deepCopyOperand(out->for_cond.value());
    out->switch_value = deepCopyOperand(out->switch_value);
    if (out->return_value) out->return_value = deepCopyOperand(out->return_value.value());
    if (replace_targets) replaceLValue(out->target, var, value);
    replaceOperand(out->value, var, value);
    for (auto& operand : out->op.operands) replaceOperand(operand, var, value);
    if (out->call_result && replace_targets) {
        replaceLValue(out->call_result.value(), var, value);
    }
    for (auto& arg : out->args) replaceOperand(arg, var, value);
    replaceOperand(out->condition, var, value);
    if (out->for_cond) replaceOperand(out->for_cond.value(), var, value);
    replaceOperand(out->switch_value, var, value);
    if (out->return_value) replaceOperand(out->return_value.value(), var, value);
    return out;
}

using SymbolRemap = std::unordered_map<SymbolId, SymbolId>;

std::string uniqueSymbolName(const FunctionCFG& fn, const std::string& base);
const SymbolInfo& symbolInfo(const FunctionCFG& fn, SymbolId symbol);

std::string clonedSymbolName(const FunctionCFG& fn,
                             const SymbolInfo& old_symbol,
                             LoopRegionId loop_id,
                             std::size_t iter) {
    return uniqueSymbolName(fn,
        old_symbol.name + "__u" + std::to_string(loop_id) + "_" +
        std::to_string(iter) + "_");
}

SymbolId cloneDeclaredSymbol(FunctionCFG& fn,
                             SymbolId old_symbol,
                             LoopRegionId loop_id,
                             std::size_t iter) {
    const auto& old_info = symbolInfo(fn, old_symbol);
    SymbolInfo info = old_info;
    info.id = static_cast<SymbolId>(fn.symbols.size());
    info.name = clonedSymbolName(fn, old_info, loop_id, iter);
    fn.symbols.push_back(info);
    return info.id;
}

void collectDeclaredSymbols(const S3StmtPtr& stmt,
                            std::vector<SymbolId>& declared) {
    if (!stmt) return;
    if (stmt->kind == S3StmtKind::Decl && stmt->decl_symbol >= 0) {
        declared.push_back(stmt->decl_symbol);
    }
    for (const auto& child : stmt->condition_prelude) collectDeclaredSymbols(child, declared);
    for (const auto& child : stmt->then_body) collectDeclaredSymbols(child, declared);
    for (const auto& child : stmt->else_body) collectDeclaredSymbols(child, declared);
    for (const auto& child : stmt->for_init) collectDeclaredSymbols(child, declared);
    for (const auto& child : stmt->for_step) collectDeclaredSymbols(child, declared);
    for (const auto& child : stmt->loop_body) collectDeclaredSymbols(child, declared);
    for (const auto& c : stmt->switch_cases) {
        for (const auto& child : c.body) collectDeclaredSymbols(child, declared);
    }
}

void collectDeclaredSymbols(const BasicBlock& block,
                            std::vector<SymbolId>& declared) {
    for (const auto& stmt : block.stmts) collectDeclaredSymbols(stmt.stmt, declared);
}

void ensureClonedSymbols(FunctionCFG& fn,
                         const BasicBlock& clone,
                         LoopRegionId loop_id,
                         std::size_t iter,
                         SymbolRemap& remap) {
    std::vector<SymbolId> declared;
    collectDeclaredSymbols(clone, declared);
    for (SymbolId old_symbol : declared) {
        if (remap.count(old_symbol)) continue;
        remap[old_symbol] = cloneDeclaredSymbol(fn, old_symbol, loop_id, iter);
    }
}

void remapOperandSymbols(const FunctionCFG& fn, Operand& operand, const SymbolRemap& remap);

void remapLValueSymbols(const FunctionCFG& fn, LValue& lvalue, const SymbolRemap& remap) {
    auto found = remap.find(lvalue.root_symbol);
    if (found != remap.end()) {
        lvalue.root_symbol = found->second;
        lvalue.root = symbolInfo(fn, found->second).name;
    }
    for (auto& access : lvalue.accesses) {
        if (access.index) remapOperandSymbols(fn, *access.index, remap);
    }
}

void remapOperandSymbols(const FunctionCFG& fn, Operand& operand, const SymbolRemap& remap) {
    if (operand.kind == OperandKind::Var) {
        auto found = remap.find(operand.var_symbol);
        if (found != remap.end()) {
            operand.var_symbol = found->second;
            operand.var_name = symbolInfo(fn, found->second).name;
        }
        return;
    }
    if (operand.kind == OperandKind::LValueRead) {
        remapLValueSymbols(fn, operand.lvalue, remap);
    }
}

void remapStmtSymbols(const FunctionCFG& fn, const S3StmtPtr& stmt, const SymbolRemap& remap);

void remapStmtListSymbols(const FunctionCFG& fn,
                          std::vector<S3StmtPtr>& stmts,
                          const SymbolRemap& remap) {
    for (auto& stmt : stmts) remapStmtSymbols(fn, stmt, remap);
}

void remapStmtSymbols(const FunctionCFG& fn, const S3StmtPtr& stmt, const SymbolRemap& remap) {
    if (!stmt) return;
    if (stmt->kind == S3StmtKind::Decl) {
        auto found = remap.find(stmt->decl_symbol);
        if (found != remap.end()) {
            stmt->decl_symbol = found->second;
            stmt->decl_name = symbolInfo(fn, found->second).name;
            stmt->decl_type = symbolInfo(fn, found->second).type;
        }
    }
    remapLValueSymbols(fn, stmt->target, remap);
    remapOperandSymbols(fn, stmt->value, remap);
    for (auto& operand : stmt->op.operands) remapOperandSymbols(fn, operand, remap);
    if (stmt->call_result) remapLValueSymbols(fn, stmt->call_result.value(), remap);
    for (auto& arg : stmt->args) remapOperandSymbols(fn, arg, remap);
    remapOperandSymbols(fn, stmt->condition, remap);
    remapStmtListSymbols(fn, stmt->condition_prelude, remap);
    remapStmtListSymbols(fn, stmt->then_body, remap);
    remapStmtListSymbols(fn, stmt->else_body, remap);
    remapStmtListSymbols(fn, stmt->for_init, remap);
    if (stmt->for_cond) remapOperandSymbols(fn, stmt->for_cond.value(), remap);
    remapStmtListSymbols(fn, stmt->for_step, remap);
    remapStmtListSymbols(fn, stmt->loop_body, remap);
    remapOperandSymbols(fn, stmt->switch_value, remap);
    for (auto& c : stmt->switch_cases) {
        if (c.value) remapOperandSymbols(fn, c.value.value(), remap);
        remapStmtListSymbols(fn, c.body, remap);
    }
    if (stmt->return_value) remapOperandSymbols(fn, stmt->return_value.value(), remap);
}

void remapBlockSymbols(const FunctionCFG& fn, BasicBlock& block, const SymbolRemap& remap) {
    for (auto& stmt : block.stmts) remapStmtSymbols(fn, stmt.stmt, remap);
}

void remapTerminatorSymbols(const FunctionCFG& fn,
                            Terminator& term,
                            const SymbolRemap& remap) {
    remapOperandSymbols(fn, term.condition, remap);
    remapOperandSymbols(fn, term.switch_value, remap);
    for (auto& target : term.switch_targets) {
        if (target.value) remapOperandSymbols(fn, target.value.value(), remap);
    }
    if (term.return_value) remapOperandSymbols(fn, term.return_value.value(), remap);
}

Terminator cloneAndRewriteTerminator(const Terminator& term,
                                     s3statementize::SymbolId var,
                                     const Operand& value) {
    auto out = term;
    replaceOperand(out.condition, var, value);
    replaceOperand(out.switch_value, var, value);
    for (auto& target : out.switch_targets) {
        if (target.value) replaceOperand(target.value.value(), var, value);
    }
    if (out.return_value) replaceOperand(out.return_value.value(), var, value);
    return out;
}

void removeLoopId(std::vector<LoopRegionId>& stack, LoopRegionId id) {
    stack.erase(std::remove(stack.begin(), stack.end(), id), stack.end());
}

std::vector<LoopRegionId> parentLoopStack(const FunctionCFG& fn, const LoopRegion& region) {
    std::vector<LoopRegionId> out;
    if (region.body >= 0 && region.body < static_cast<BlockId>(fn.blocks.size())) {
        out = fn.blocks[static_cast<std::size_t>(region.body)]->loop_stack;
    }
    removeLoopId(out, region.id);
    return out;
}

bool edgeTargetsLoopBreak(const CFGEdge& edge, const LoopRegion& region) {
    return edge.to == region.exit &&
           (edge.kind == EdgeKind::Break || edge.label == "break");
}

bool loopHasBreak(const FunctionCFG& fn,
                  const LoopRegion& region,
                  const std::set<BlockId>& loop_blocks) {
    for (BlockId id : loop_blocks) {
        if (id == region.condition || id == region.condition_prelude) continue;
        const auto& block = *fn.blocks[static_cast<std::size_t>(id)];
        for (const auto& edge : block.successors) {
            if (edgeTargetsLoopBreak(edge, region)) return true;
        }
    }
    return false;
}

TypeInfo boolType() {
    TypeInfo type;
    type.name = "bool";
    type.width = 1;
    type.is_signed = false;
    type.is_hw_int = true;
    type.hw_kind = "bool";
    return type;
}

std::string uniqueSymbolName(const FunctionCFG& fn, const std::string& base) {
    std::unordered_set<std::string> used;
    for (const auto& symbol : fn.symbols) used.insert(symbol.name);
    for (int i = 0;; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!used.count(candidate)) return candidate;
    }
}

SymbolId createLoopEnableSymbol(FunctionCFG& fn, const LoopRegion& region) {
    SymbolInfo info;
    info.id = static_cast<SymbolId>(fn.symbols.size());
    info.name = uniqueSymbolName(fn,
        "__s5_loop_enable_" + std::to_string(region.id) + "_");
    info.type = boolType();
    info.declaring_scope = -1;
    fn.symbols.push_back(info);
    return info.id;
}

const SymbolInfo& symbolInfo(const FunctionCFG& fn, SymbolId symbol) {
    if (symbol < 0 || symbol >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("Internal error: invalid symbol id");
    }
    return fn.symbols[static_cast<std::size_t>(symbol)];
}

LValue symbolLValue(const FunctionCFG& fn, SymbolId symbol) {
    const auto& info = symbolInfo(fn, symbol);
    LValue out;
    out.root = info.name;
    out.root_symbol = symbol;
    out.type = info.type;
    return out;
}

Operand symbolOperand(const FunctionCFG& fn, SymbolId symbol) {
    const auto& info = symbolInfo(fn, symbol);
    Operand out;
    out.kind = OperandKind::Var;
    out.var_name = info.name;
    out.var_symbol = symbol;
    out.type = info.type;
    return out;
}

Operand boolLiteral(bool value) {
    Operand out;
    out.kind = OperandKind::Literal;
    out.type = boolType();
    out.literal_value = value ? "true" : "false";
    return out;
}

S3StmtPtr makeSymbolDecl(const FunctionCFG& fn, SymbolId symbol) {
    const auto& info = symbolInfo(fn, symbol);
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Decl;
    stmt->decl_name = info.name;
    stmt->decl_symbol = symbol;
    stmt->decl_type = info.type;
    return stmt;
}

S3StmtPtr makeSymbolAssign(const FunctionCFG& fn, SymbolId symbol, Operand value) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Assign;
    stmt->target = symbolLValue(fn, symbol);
    stmt->value = std::move(value);
    return stmt;
}

BasicBlock* appendSyntheticBlock(FunctionCFG& fn,
                                 std::vector<LoopRegionId> loop_stack) {
    auto block = std::make_unique<BasicBlock>();
    block->id = static_cast<BlockId>(fn.blocks.size());
    block->loop_stack = std::move(loop_stack);
    auto* out = block.get();
    fn.blocks.push_back(std::move(block));
    return out;
}

void rewriteTermTarget(Terminator& term, BlockId old_target, BlockId new_target) {
    if (term.jump_target == old_target) term.jump_target = new_target;
    if (term.true_target == old_target) term.true_target = new_target;
    if (term.false_target == old_target) term.false_target = new_target;
    if (term.default_target == old_target) term.default_target = new_target;
    for (auto& target : term.switch_targets) {
        if (target.target == old_target) target.target = new_target;
    }
}

void rewriteExternalPreds(FunctionCFG& fn,
                          const LoopRegion& region,
                          BlockId replacement) {
    BlockId entry_target = region.condition_kind == LoopConditionKind::PostTest
        ? region.body
        : region.condition_prelude;
    auto header_preds = allIncomingEdges(*fn.blocks[static_cast<std::size_t>(entry_target)]);
    for (const auto& edge : header_preds) {
        if (hasLoop(*fn.blocks[static_cast<std::size_t>(edge.from)], region.id)) continue;
        rewriteTermTarget(fn.blocks[static_cast<std::size_t>(edge.from)]->terminator,
                          entry_target, replacement);
    }
}

void rewriteCloneTerminatorTargets(Terminator& term,
                                   const std::unordered_map<BlockId, BlockId>& map,
                                   const LoopRegion& region,
                                   BlockId next_entry,
                                   BlockId break_target) {
    auto translate = [&](BlockId target) {
        auto it = map.find(target);
        if (it != map.end()) return it->second;
        if (target == region.condition_prelude) return next_entry;
        if (target == region.condition) return next_entry;
        if (target == region.exit) return break_target;
        return target;
    };
    term.jump_target = translate(term.jump_target);
    term.true_target = translate(term.true_target);
    term.false_target = translate(term.false_target);
    term.default_target = translate(term.default_target);
    for (auto& target : term.switch_targets) target.target = translate(target.target);
}

Operand inductionLiteralAt(std::uint64_t iteration_value,
                           std::int64_t delta,
                           const TypeInfo& type) {
    return literalOperand(wrapValue(signedValue(iteration_value, type) + delta, type), type);
}

void rewriteBlockForIteration(BasicBlock& clone,
                              const BasicBlock& original,
                              const LoopAnalysis& analysis,
                              std::uint64_t iteration_value) {
    auto it = analysis.body_entry_deltas.find(original.id);
    if (it == analysis.body_entry_deltas.end()) {
        fail("Cannot unroll loop: missing loop body delta for block bb" +
             std::to_string(original.id));
    }

    DeltaState state;
    state.induction_delta = it->second;
    for (std::size_t i = 0; i < clone.stmts.size(); ++i) {
        Operand literal = inductionLiteralAt(iteration_value,
                                             state.induction_delta,
                                             analysis.type);
        clone.stmts[i].stmt = cloneAndRewriteStmt(clone.stmts[i].stmt,
                                                  analysis.var,
                                                  literal,
                                                  true);
        processDeltaStmt(original.stmts[i], state, analysis.var);
    }

    Operand literal = inductionLiteralAt(iteration_value,
                                         state.induction_delta,
                                         analysis.type);
    clone.terminator = cloneAndRewriteTerminator(clone.terminator,
                                                 analysis.var,
                                                 literal);
}

int unrollForLoop(FunctionCFG& fn,
                  const LoopRegion& region,
                  const LoopAnalysis& analysis,
                  const UnrollOptions& options) {
    auto loop_blocks = blocksInLoop(fn, region.id);
    bool has_break = loopHasBreak(fn, region, loop_blocks);
    std::vector<BlockId> body_blocks;
    for (BlockId id : loop_blocks) {
        if (id == region.condition || id == region.condition_prelude) continue;
        if (!analysis.body_entry_deltas.count(id)) continue;
        body_blocks.push_back(id);
    }
    if (std::find(body_blocks.begin(), body_blocks.end(), region.body) ==
        body_blocks.end()) {
        fail("Cannot unroll loop: loop body entry is not in loop region");
    }

    std::vector<std::unordered_map<BlockId, BlockId>> body_maps;
    body_maps.resize(analysis.values.size());
    int cloned_blocks = 0;
    std::vector<BlockId> guard_blocks;
    std::vector<BlockId> break_blocks;
    BlockId enable_init = -1;
    SymbolId enable_symbol = -1;

    for (std::size_t iter = 0; iter < analysis.values.size(); ++iter) {
        SymbolRemap symbol_remap;
        for (BlockId old : body_blocks) {
            auto clone = std::make_unique<BasicBlock>(*fn.blocks[static_cast<std::size_t>(old)]);
            clone->id = static_cast<BlockId>(fn.blocks.size());
            clone->successors.clear();
            clone->predecessors.clear();
            removeLoopId(clone->loop_stack, region.id);
            rewriteBlockForIteration(*clone,
                                      *fn.blocks[static_cast<std::size_t>(old)],
                                      analysis,
                                      analysis.values[iter]);
            body_maps[iter][old] = clone->id;
            fn.blocks.push_back(std::move(clone));
            ++cloned_blocks;
        }

        for (const auto& [_, cloned_id] : body_maps[iter]) {
            ensureClonedSymbols(fn,
                                *fn.blocks[static_cast<std::size_t>(cloned_id)],
                                region.id,
                                iter,
                                symbol_remap);
        }
        for (const auto& [_, cloned_id] : body_maps[iter]) {
            auto& clone = *fn.blocks[static_cast<std::size_t>(cloned_id)];
            remapBlockSymbols(fn, clone, symbol_remap);
            remapTerminatorSymbols(fn, clone.terminator, symbol_remap);
        }

        if (cloned_blocks > options.max_total_cloned_blocks) {
            fail("Unrolled CFG exceeds max_total_cloned_blocks");
        }
    }

    if (has_break && !analysis.values.empty()) {
        enable_symbol = createLoopEnableSymbol(fn, region);
        std::vector<LoopRegionId> synthetic_loops = parentLoopStack(fn, region);

        auto* init = appendSyntheticBlock(fn, synthetic_loops);
        enable_init = init->id;
        init->stmts.push_back(CFGStmt{CFGStmtKind::Decl,
                                      makeSymbolDecl(fn, enable_symbol)});
        init->stmts.push_back(CFGStmt{CFGStmtKind::Assign,
                                      makeSymbolAssign(fn, enable_symbol,
                                                       boolLiteral(true))});
        ++cloned_blocks;

        guard_blocks.reserve(analysis.values.size());
        break_blocks.reserve(analysis.values.size());
        for (std::size_t iter = 0; iter < analysis.values.size(); ++iter) {
            auto* guard = appendSyntheticBlock(fn, synthetic_loops);
            guard_blocks.push_back(guard->id);
            ++cloned_blocks;
            auto* breaker = appendSyntheticBlock(fn, synthetic_loops);
            breaker->stmts.push_back(
                CFGStmt{CFGStmtKind::Assign,
                        makeSymbolAssign(fn, enable_symbol, boolLiteral(false))});
            break_blocks.push_back(breaker->id);
            ++cloned_blocks;
        }
        if (cloned_blocks > options.max_total_cloned_blocks) {
            fail("Unrolled CFG exceeds max_total_cloned_blocks");
        }
    }

    for (std::size_t iter = 0; iter < analysis.values.size(); ++iter) {
        BlockId next_entry = -1;
        if (iter + 1 < analysis.values.size()) {
            next_entry = has_break ? guard_blocks[iter + 1]
                                   : body_maps[iter + 1][region.body];
        } else {
            next_entry = region.exit;
        }
        BlockId break_target = has_break ? break_blocks[iter] : region.exit;
        for (const auto& [old, cloned_id] : body_maps[iter]) {
            auto& cloned = *fn.blocks[static_cast<std::size_t>(cloned_id)];
            rewriteCloneTerminatorTargets(cloned.terminator, body_maps[iter],
                                          region, next_entry, break_target);
        }
        if (has_break) {
            auto& guard = *fn.blocks[static_cast<std::size_t>(guard_blocks[iter])];
            guard.terminator.kind = TermKind::Branch;
            guard.terminator.condition = symbolOperand(fn, enable_symbol);
            guard.terminator.true_target = body_maps[iter][region.body];
            guard.terminator.false_target = next_entry;

            auto& breaker = *fn.blocks[static_cast<std::size_t>(break_blocks[iter])];
            breaker.terminator.kind = TermKind::Jump;
            breaker.terminator.jump_target = next_entry;
        }
    }

    if (has_break && !analysis.values.empty()) {
        auto& init = *fn.blocks[static_cast<std::size_t>(enable_init)];
        init.terminator.kind = TermKind::Jump;
        init.terminator.jump_target = guard_blocks.front();
    }

    BlockId first = analysis.values.empty() ? region.exit :
        (has_break ? enable_init : body_maps.front()[region.body]);
    rewriteExternalPreds(fn, region, first);
    rebuildEdges(fn);
    return cloned_blocks;
}

void rejectUnsupportedLoopControls(const FunctionCFG& fn, const LoopRegion& region) {
    (void)fn;
    (void)region;
}

void unrollFunction(FunctionCFG& fn,
                    const UnrollOptions& options,
                    std::vector<UnrollWarning>& warnings,
                    std::vector<UnrollSummary>& summaries) {
    (void)warnings;
    while (!fn.loop_regions.empty()) {
        auto region_it = std::max_element(
            fn.loop_regions.begin(),
            fn.loop_regions.end(),
            [](const LoopRegion& a, const LoopRegion& b) { return a.id < b.id; });
        LoopRegion region = *region_it;
        ErrorContextGuard guard("s5unroll", DebugLoc{},
                                "unrolling loop " + std::to_string(region.id) +
                                    " in function " + fn.name);
        if (!options.allow_while &&
            region.condition_kind == LoopConditionKind::PostTest) {
            fail("do-while loops are not allowed by unroll options");
        }
        rejectUnsupportedLoopControls(fn, region);
        auto analysis = analyzeLoop(fn, region, options);
        if (!analysis) fail("Cannot statically analyze loop");
        int cloned = unrollForLoop(fn, region, *analysis, options);
        UnrollSummary summary;
        summary.function_name = fn.name;
        summary.loop_id = region.id;
        summary.condition_kind = region.condition_kind;
        summary.iterations = static_cast<int>(analysis->values.size());
        summary.cloned_blocks = cloned;
        summaries.push_back(summary);
        fn.loop_regions.erase(
            std::remove_if(fn.loop_regions.begin(), fn.loop_regions.end(),
                           [&](const LoopRegion& candidate) {
                               return candidate.id == region.id;
                           }),
            fn.loop_regions.end());
        compactReachable(fn);
    }
    fn.loop_regions.clear();
    for (auto& block : fn.blocks) block->loop_stack.clear();
}

void verifyAcyclic(const FunctionCFG& fn) {
    enum class Mark { None, Visiting, Done };
    std::vector<Mark> mark(fn.blocks.size(), Mark::None);
    std::function<void(BlockId)> dfs = [&](BlockId id) {
        if (id < 0 || id >= static_cast<BlockId>(fn.blocks.size())) {
            fail("Unrolled CFG has invalid edge target");
        }
        auto& m = mark[static_cast<std::size_t>(id)];
        if (m == Mark::Visiting) fail("Unrolled CFG still contains a cycle");
        if (m == Mark::Done) return;
        m = Mark::Visiting;
        for (const auto& edge : fn.blocks[static_cast<std::size_t>(id)]->successors) {
            dfs(edge.to);
        }
        m = Mark::Done;
    };
    dfs(fn.entry);
}

void verifyNoLoops(const FunctionCFG& fn) {
    if (!fn.loop_regions.empty()) fail("Unrolled CFG still contains loop regions");
    for (const auto& block : fn.blocks) {
        if (!block->loop_stack.empty()) fail("Unrolled CFG block still has loop metadata");
        for (const auto& edge : block->successors) {
            if (edge.label == "backedge" || edge.label == "continue" ||
                edge.kind == EdgeKind::Continue) {
                fail("Unrolled CFG still contains loop-control edge");
            }
        }
    }
    verifyAcyclic(fn);
}

void verifyProgram(const CFGProgram& program) {
    verifyNoLoops(program.top);
    for (const auto& helper : program.helpers) verifyNoLoops(helper);
    for (const auto& [_, lambda] : program.lambdas) verifyNoLoops(lambda);
}

CFGProgram runUnroll(const CFGProgram& input,
                     const UnrollOptions& options,
                     std::vector<UnrollWarning>& warnings,
                     std::vector<UnrollSummary>& summaries) {
    auto out = cloneProgram(input);
    unrollFunction(out.top, options, warnings, summaries);
    for (auto& helper : out.helpers) unrollFunction(helper, options, warnings, summaries);
    for (auto& [_, lambda] : out.lambdas) unrollFunction(lambda, options, warnings, summaries);
    verifyProgram(out);
    return out;
}

std::string loopKindName(LoopConditionKind kind) {
    switch (kind) {
    case LoopConditionKind::PreTest: return "pre_test";
    case LoopConditionKind::PostTest: return "post_test";
    }
    return "loop";
}

} // namespace

std::string debugPrint(const CFGProgram& program,
                       const std::vector<UnrollSummary>& summaries) {
    std::ostringstream os;
    os << "s5unroll\n";
    for (const auto& summary : summaries) {
        os << "unrolled " << summary.function_name
           << " loop=" << summary.loop_id
           << " kind=" << loopKindName(summary.condition_kind)
           << " iterations=" << summary.iterations
           << " cloned_blocks=" << summary.cloned_blocks << "\n";
    }
    os << s4cfg::debugPrint(program);
    return os.str();
}

UnrollResult unrollCFGProgram(const CFGProgram& program,
                              const UnrollOptions& options) {
    try {
        UnrollResult result;
        result.program = runUnroll(program, options, result.warnings, result.summaries);
        if (options.debug_print) {
            result.debug_text = debugPrint(result.program.value(), result.summaries);
        }
        return result;
    } catch (const RTLZZException& ex) {
        UnrollResult result;
        UnrollError error;
        error.message = ex.message();
        error.formatted = ex.what();
        if (auto context = ex.primaryContext()) error.context = *context;
        result.error = std::move(error);
        return result;
    }
}

CFGProgram unrollCFGProgramOrThrow(const CFGProgram& program,
                                   const UnrollOptions& options) {
    std::vector<UnrollWarning> warnings;
    std::vector<UnrollSummary> summaries;
    auto out = runUnroll(program, options, warnings, summaries);
    (void)options;
    return out;
}

} // namespace pred::s5unroll
