#include "s9ssa/S9SSA.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s9ssa {
namespace {

using namespace pred::s8opnorm;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s9ssa";
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

bool typeEq(const S9Type& lhs, const S9Type& rhs) {
    return lhs.kind == rhs.kind && lhs.width == rhs.width;
}

bool isBoolType(const S9Type& type) {
    return type.kind == S8TypeKind::Bool && type.width == 1;
}

std::string typeText(const S9Type& type) {
    return type.kind == S8TypeKind::Bool ? "bool" : ("u" + std::to_string(type.width));
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

S9Literal makeLiteralValue(std::uint64_t value, S9Type type, bool signed_view = false) {
    S9Literal literal;
    literal.valid_width = type.width;
    literal.is_signed = signed_view;
    literal.source_text = std::to_string(value);
    literal.words.push_back(value);
    trimToWidth(literal.words, type.width);
    return literal;
}

S9Operand literalOperand(std::uint64_t value,
                         S9Type type,
                         bool signed_view,
                         DebugLoc loc) {
    S9Operand out;
    out.kind = S9OperandKind::Literal;
    out.type = type;
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.literal = makeLiteralValue(value, type, signed_view);
    return out;
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

const S8Symbol& s8SymbolAt(const S8NormCFG& fn, SymbolId id) {
    if (id < 0 || id >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("Invalid S8 symbol reference");
    }
    const auto& symbol = fn.symbols[static_cast<std::size_t>(id)];
    if (symbol.id != id) fail("Broken S8 symbol table invariant");
    return symbol;
}

const S9Value& valueAt(const S9SSACFG& fn, S9ValueId id) {
    if (id < 0 || id >= static_cast<S9ValueId>(fn.values.size())) {
        fail("Invalid S9 value reference");
    }
    const auto& value = fn.values[static_cast<std::size_t>(id)];
    if (value.id != id) fail("Broken S9 value table invariant");
    return value;
}

std::string symbolName(const S9SSACFG& fn, SymbolId symbol) {
    if (symbol < 0 || symbol >= static_cast<SymbolId>(fn.base_symbols.size())) {
        return "<generated>";
    }
    return fn.base_symbols[static_cast<std::size_t>(symbol)].debug_name;
}

std::string valueName(const S9SSACFG& fn, S9ValueId value_id) {
    const auto& value = valueAt(fn, value_id);
    if (value.base_symbol >= 0) {
        return symbolName(fn, value.base_symbol) + "_v" + std::to_string(value.version);
    }
    return value.debug_name + "#" + std::to_string(value.id);
}

std::string valueKindName(S9ValueKind kind) {
    switch (kind) {
    case S9ValueKind::Initial: return "initial";
    case S9ValueKind::Statement: return "stmt";
    case S9ValueKind::Phi: return "phi";
    case S9ValueKind::Generated: return "generated";
    }
    return "value";
}

std::string opName(S9OpKind kind) {
    switch (kind) {
    case S9OpKind::AssignCast: return "AssignCast";
    case S9OpKind::Add: return "Add";
    case S9OpKind::Sub: return "Sub";
    case S9OpKind::Mul: return "Mul";
    case S9OpKind::Neg: return "Neg";
    case S9OpKind::BitNot: return "BitNot";
    case S9OpKind::LogicalNot: return "LogicalNot";
    case S9OpKind::BitAnd: return "BitAnd";
    case S9OpKind::BitOr: return "BitOr";
    case S9OpKind::BitXor: return "BitXor";
    case S9OpKind::BoolAnd: return "BoolAnd";
    case S9OpKind::BoolOr: return "BoolOr";
    case S9OpKind::Shl: return "Shl";
    case S9OpKind::LShr: return "LShr";
    case S9OpKind::AShr: return "AShr";
    case S9OpKind::Eq: return "Eq";
    case S9OpKind::Ne: return "Ne";
    case S9OpKind::Lt: return "Lt";
    case S9OpKind::Le: return "Le";
    case S9OpKind::Gt: return "Gt";
    case S9OpKind::Ge: return "Ge";
    case S9OpKind::Mux: return "Mux";
    case S9OpKind::ZExt: return "ZExt";
    case S9OpKind::SExt: return "SExt";
    case S9OpKind::Trunc: return "Trunc";
    case S9OpKind::Slice: return "Slice";
    case S9OpKind::BitSelect: return "BitSelect";
    case S9OpKind::DynamicSlice: return "DynamicSlice";
    case S9OpKind::DynamicBitSelect: return "DynamicBitSelect";
    case S9OpKind::WriteSlice: return "WriteSlice";
    case S9OpKind::WriteBit: return "WriteBit";
    case S9OpKind::DynamicWriteSlice: return "DynamicWriteSlice";
    case S9OpKind::DynamicWriteBit: return "DynamicWriteBit";
    case S9OpKind::Concat: return "Concat";
    case S9OpKind::Repeat: return "Repeat";
    case S9OpKind::ReduceOr: return "ReduceOr";
    case S9OpKind::ReduceAnd: return "ReduceAnd";
    case S9OpKind::ReduceXor: return "ReduceXor";
    }
    return "Op";
}

std::string termName(S9TermKind kind) {
    switch (kind) {
    case S9TermKind::Jump: return "jump";
    case S9TermKind::Branch: return "branch";
    case S9TermKind::Switch: return "switch";
    case S9TermKind::Exit: return "exit";
    case S9TermKind::Unreachable: return "unreachable";
    }
    return "term";
}

std::vector<BlockId> successorsOf(const S8Terminator& term) {
    std::vector<BlockId> out;
    switch (term.kind) {
    case S8TermKind::Jump:
        out.push_back(term.jump_target);
        break;
    case S8TermKind::Branch:
        out.push_back(term.true_target);
        out.push_back(term.false_target);
        break;
    case S8TermKind::Switch:
        for (const auto& target : term.switch_targets) out.push_back(target.target);
        out.push_back(term.default_target);
        break;
    case S8TermKind::Exit:
    case S8TermKind::Unreachable:
        break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct CFGInfo {
    std::vector<std::vector<BlockId>> succs;
    std::vector<std::vector<BlockId>> preds;
    std::vector<bool> reachable;
    std::vector<BlockId> topo;
    BlockId effective_exit = -1;
};

CFGInfo analyzeCFG(const S8NormCFG& fn) {
    if (fn.entry < 0 || fn.entry >= static_cast<BlockId>(fn.blocks.size())) {
        fail("S8 CFG has invalid entry block");
    }
    if (fn.exit < 0 || fn.exit >= static_cast<BlockId>(fn.blocks.size())) {
        fail("S8 CFG has invalid exit block");
    }
    const int n = static_cast<int>(fn.blocks.size());
    for (int i = 0; i < n; ++i) {
        if (fn.blocks[static_cast<std::size_t>(i)].id != i) {
            fail("S9 currently requires dense block ids");
        }
    }

    CFGInfo info;
    info.succs.resize(static_cast<std::size_t>(n));
    info.preds.resize(static_cast<std::size_t>(n));
    info.reachable.assign(static_cast<std::size_t>(n), false);
    for (int b = 0; b < n; ++b) {
        info.succs[static_cast<std::size_t>(b)] = successorsOf(fn.blocks[static_cast<std::size_t>(b)].terminator);
        for (BlockId succ : info.succs[static_cast<std::size_t>(b)]) {
            if (succ < 0 || succ >= n) fail("S8 CFG has invalid successor block");
            info.preds[static_cast<std::size_t>(succ)].push_back(b);
        }
    }
    for (auto& preds : info.preds) {
        std::sort(preds.begin(), preds.end());
        preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
    }

    std::vector<int> color(static_cast<std::size_t>(n), 0);
    std::vector<BlockId> postorder;
    std::function<void(BlockId)> dfs = [&](BlockId b) {
        color[static_cast<std::size_t>(b)] = 1;
        info.reachable[static_cast<std::size_t>(b)] = true;
        for (BlockId succ : info.succs[static_cast<std::size_t>(b)]) {
            if (color[static_cast<std::size_t>(succ)] == 1) {
                fail("S9 SSA does not support cyclic CFG; loops must be fully unrolled before S9");
            }
            if (color[static_cast<std::size_t>(succ)] == 0) dfs(succ);
        }
        color[static_cast<std::size_t>(b)] = 2;
        postorder.push_back(b);
    };
    dfs(fn.entry);
    info.topo.assign(postorder.rbegin(), postorder.rend());
    if (info.reachable[static_cast<std::size_t>(fn.exit)]) {
        info.effective_exit = fn.exit;
    } else {
        for (BlockId b : info.topo) {
            if (fn.blocks[static_cast<std::size_t>(b)].terminator.kind != S8TermKind::Exit) continue;
            if (info.effective_exit >= 0) {
                fail("S8 CFG has multiple reachable exit terminators but exit metadata is unreachable");
            }
            info.effective_exit = b;
        }
        if (info.effective_exit < 0) {
            fail("S8 CFG exit block is not reachable from entry and no reachable exit terminator exists");
        }
    }
    return info;
}

void addUse(const S8Operand& operand,
            const std::set<SymbolId>& local_defs,
            std::set<SymbolId>& uses) {
    if (operand.kind != S8OperandKind::Var) return;
    if (!local_defs.count(operand.symbol)) uses.insert(operand.symbol);
}

void collectStmtUsesDefs(const S8Stmt& stmt,
                         std::set<SymbolId>& uses,
                         std::set<SymbolId>& defs) {
    switch (stmt.kind) {
    case S8StmtKind::Assign:
        addUse(stmt.value, defs, uses);
        defs.insert(stmt.target);
        break;
    case S8StmtKind::Op:
        for (const auto& operand : stmt.op.operands) addUse(operand, defs, uses);
        defs.insert(stmt.target);
        break;
    case S8StmtKind::Lookup:
        addUse(stmt.lookup_index, defs, uses);
        for (const auto& elem : stmt.lookup_elements) addUse(elem, defs, uses);
        defs.insert(stmt.target);
        break;
    case S8StmtKind::LookupWrite:
        addUse(stmt.lookup_index, defs, uses);
        addUse(stmt.lookup_value, defs, uses);
        for (const auto& elem : stmt.lookup_elements) addUse(elem, defs, uses);
        for (SymbolId target : stmt.lookup_write_targets) defs.insert(target);
        break;
    }
}

void collectTermUses(const S8Terminator& term,
                     const std::set<SymbolId>& local_defs,
                     std::set<SymbolId>& uses) {
    if (term.kind == S8TermKind::Branch) {
        addUse(term.condition, local_defs, uses);
    } else if (term.kind == S8TermKind::Switch) {
        addUse(term.switch_value, local_defs, uses);
        for (const auto& target : term.switch_targets) {
            if (target.value) addUse(target.value.value(), local_defs, uses);
        }
    }
}

struct Liveness {
    std::vector<std::set<SymbolId>> live_in;
    std::vector<std::set<SymbolId>> live_out;
};

bool isFinalPort(const S8Port& port) {
    return port.direction == ParamDirection::Output ||
           port.passing == ParamPassingKind::MutableRef;
}

Liveness computeLiveness(const S8NormCFG& fn, const CFGInfo& cfg) {
    const int n = static_cast<int>(fn.blocks.size());
    std::vector<std::set<SymbolId>> uses(static_cast<std::size_t>(n));
    std::vector<std::set<SymbolId>> defs(static_cast<std::size_t>(n));
    for (BlockId b : cfg.topo) {
        const auto& block = fn.blocks[static_cast<std::size_t>(b)];
        for (const auto& stmt : block.stmts) {
            collectStmtUsesDefs(stmt, uses[static_cast<std::size_t>(b)],
                                defs[static_cast<std::size_t>(b)]);
        }
        collectTermUses(block.terminator, defs[static_cast<std::size_t>(b)],
                        uses[static_cast<std::size_t>(b)]);
    }

    Liveness live;
    live.live_in.resize(static_cast<std::size_t>(n));
    live.live_out.resize(static_cast<std::size_t>(n));

    std::set<SymbolId> final_symbols;
    for (const auto& port : fn.ports) {
        if (isFinalPort(port)) final_symbols.insert(port.symbol);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = cfg.topo.rbegin(); it != cfg.topo.rend(); ++it) {
            BlockId b = *it;
            auto new_out = b == cfg.effective_exit ? final_symbols : std::set<SymbolId>{};
            for (BlockId succ : cfg.succs[static_cast<std::size_t>(b)]) {
                if (!cfg.reachable[static_cast<std::size_t>(succ)]) continue;
                new_out.insert(live.live_in[static_cast<std::size_t>(succ)].begin(),
                               live.live_in[static_cast<std::size_t>(succ)].end());
            }

            auto new_in = uses[static_cast<std::size_t>(b)];
            for (SymbolId symbol : new_out) {
                if (!defs[static_cast<std::size_t>(b)].count(symbol)) new_in.insert(symbol);
            }
            if (new_out != live.live_out[static_cast<std::size_t>(b)] ||
                new_in != live.live_in[static_cast<std::size_t>(b)]) {
                live.live_out[static_cast<std::size_t>(b)] = std::move(new_out);
                live.live_in[static_cast<std::size_t>(b)] = std::move(new_in);
                changed = true;
            }
        }
    }
    return live;
}

struct Context {
    const S8NormCFG* input = nullptr;
    S9SSACFG output;
    SSAOptions options;
    SSASummary summary;
    std::vector<int> next_version;
    int generated_counter = 0;
};

S9ValueId addValue(Context& ctx,
                   SymbolId base_symbol,
                   S9Type type,
                   S9ValueKind kind,
                   std::string debug_name,
                   BlockId def_block,
                   std::optional<int> forced_version = std::nullopt) {
    if (static_cast<int>(ctx.output.values.size() + 1) > ctx.options.max_values) {
        fail("S9 value limit exceeded");
    }
    S9Value value;
    value.id = static_cast<S9ValueId>(ctx.output.values.size());
    value.base_symbol = base_symbol;
    value.type = type;
    value.kind = kind;
    value.debug_name = std::move(debug_name);
    value.def_block = def_block;
    if (base_symbol >= 0) {
        if (base_symbol >= static_cast<SymbolId>(ctx.next_version.size())) {
            fail("Invalid base symbol while creating S9 value");
        }
        if (forced_version) {
            value.version = *forced_version;
            ctx.next_version[static_cast<std::size_t>(base_symbol)] =
                std::max(ctx.next_version[static_cast<std::size_t>(base_symbol)],
                         value.version + 1);
        } else {
            value.version = ctx.next_version[static_cast<std::size_t>(base_symbol)]++;
        }
    }
    ctx.output.values.push_back(std::move(value));
    return static_cast<S9ValueId>(ctx.output.values.size() - 1);
}

S9Operand valueOperand(const S9SSACFG& fn,
                       S9ValueId value,
                       bool signed_view,
                       DebugLoc loc = {}) {
    S9Operand out;
    out.kind = S9OperandKind::Value;
    out.type = valueAt(fn, value).type;
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.value = value;
    return out;
}

using Env = std::vector<std::optional<S9ValueId>>;

S9Operand rewriteOperand(Context& ctx,
                         const S8Operand& operand,
                         const Env& env,
                         BlockId block,
                         const std::string& note = {}) {
    S9Operand out;
    out.type = operand.type;
    out.signed_view = operand.signed_view;
    out.debug_loc = operand.debug_loc;
    if (operand.kind == S8OperandKind::Literal) {
        out.kind = S9OperandKind::Literal;
        out.literal = operand.literal;
        return out;
    }
    if (operand.symbol < 0 || operand.symbol >= static_cast<SymbolId>(env.size()) ||
        !env[static_cast<std::size_t>(operand.symbol)]) {
        std::string message = "Read before definition for symbol '" +
            s8SymbolAt(*ctx.input, operand.symbol).debug_name + "'";
        fail(message, operand.debug_loc,
             note.empty() ? ("bb" + std::to_string(block)) : note);
    }
    S9ValueId value = env[static_cast<std::size_t>(operand.symbol)].value();
    const auto& ssa_value = valueAt(ctx.output, value);
    if (!typeEq(ssa_value.type, operand.type)) {
        fail("SSA operand type does not match source operand type", operand.debug_loc);
    }
    return valueOperand(ctx.output, value, operand.signed_view, operand.debug_loc);
}

bool isInputOnlyPort(const S8NormCFG& fn, SymbolId symbol) {
    for (const auto& port : fn.ports) {
        if (port.symbol == symbol) {
            return port.direction == ParamDirection::Input &&
                   port.passing != ParamPassingKind::MutableRef;
        }
    }
    return false;
}

S9ValueId defineBase(Context& ctx,
                     Env& env,
                     SymbolId target,
                     S9ValueKind kind,
                     DebugLoc loc,
                     BlockId block) {
    if (isInputOnlyPort(*ctx.input, target)) {
        fail("Cannot assign to input-only port in S9 SSA", loc,
             "symbol=" + s8SymbolAt(*ctx.input, target).debug_name);
    }
    const auto& symbol = s8SymbolAt(*ctx.input, target);
    S9ValueId value = addValue(ctx, target, symbol.type, kind,
                               symbol.debug_name, block);
    env[static_cast<std::size_t>(target)] = value;
    return value;
}

S9ValueId defineGenerated(Context& ctx,
                          S9Type type,
                          const std::string& hint,
                          BlockId block) {
    std::string name = "__s9_" + hint + "_" + std::to_string(ctx.generated_counter++);
    ++ctx.summary.generated_ops;
    return addValue(ctx, -1, type, S9ValueKind::Generated, std::move(name), block);
}

S9Operation copyOperation(const S8Operation& op) {
    S9Operation out;
    out.kind = op.kind;
    out.debug_loc = op.debug_loc;
    out.hi = op.hi;
    out.lo = op.lo;
    out.bit = op.bit;
    out.times = op.times;
    out.result_width = op.result_width;
    return out;
}

S9Stmt makeOpStmt(S9ValueId target,
                  S9OpKind kind,
                  S9Type result_type,
                  std::vector<S9Operand> operands,
                  DebugLoc loc,
                  std::string note = {}) {
    S9Stmt stmt;
    stmt.kind = S9StmtKind::Op;
    stmt.debug_loc = loc;
    stmt.target = target;
    stmt.op.kind = kind;
    stmt.op.debug_loc = loc;
    stmt.op.result_width = result_type.width;
    stmt.op.operands = std::move(operands);
    stmt.debug_note = std::move(note);
    return stmt;
}

S9Operand castTo(Context& ctx,
                 S9Operand value,
                 S9Type target_type,
                 std::vector<S9Stmt>& out,
                 DebugLoc loc,
                 BlockId block,
                 const std::string& hint) {
    if (typeEq(value.type, target_type)) return value;
    S9OpKind kind = S9OpKind::AssignCast;
    if (value.type.kind != S8TypeKind::Bool && target_type.kind == S8TypeKind::Bool) {
        kind = S9OpKind::ReduceOr;
    } else if (value.type.width < target_type.width) {
        kind = value.signed_view ? S9OpKind::SExt : S9OpKind::ZExt;
    } else if (value.type.width > target_type.width) {
        kind = S9OpKind::Trunc;
    }
    S9ValueId target = defineGenerated(ctx, target_type, hint, block);
    out.push_back(makeOpStmt(target, kind, target_type, {std::move(value)}, loc,
                             "cast_for_lookupwrite"));
    return valueOperand(ctx.output, target, false, loc);
}

void rewriteLookupWrite(Context& ctx,
                        const S8Stmt& stmt,
                        Env& env,
                        S9BasicBlock& out_block) {
    if (stmt.lookup_write_targets.empty()) {
        fail("LookupWrite has no targets", stmt.debug_loc);
    }
    if (stmt.lookup_write_targets.size() != stmt.lookup_elements.size()) {
        fail("LookupWrite target/element count mismatch", stmt.debug_loc);
    }
    std::unordered_set<SymbolId> seen_targets;
    for (SymbolId target : stmt.lookup_write_targets) {
        if (!seen_targets.insert(target).second) {
            fail("LookupWrite targets must be unique", stmt.debug_loc);
        }
    }

    S9Operand index = rewriteOperand(ctx, stmt.lookup_index, env, out_block.id,
                                     "lookupwrite index");
    S9Operand value = rewriteOperand(ctx, stmt.lookup_value, env, out_block.id,
                                     "lookupwrite value");
    std::vector<S9Operand> old_elements;
    old_elements.reserve(stmt.lookup_elements.size());
    for (const auto& elem : stmt.lookup_elements) {
        old_elements.push_back(rewriteOperand(ctx, elem, env, out_block.id,
                                              "lookupwrite old element"));
    }

    for (std::size_t i = 0; i < stmt.lookup_write_targets.size(); ++i) {
        SymbolId target_symbol = stmt.lookup_write_targets[i];
        const auto& target_info = s8SymbolAt(*ctx.input, target_symbol);
        S9Type target_type = target_info.type;
        S9Operand case_lit = literalOperand(static_cast<std::uint64_t>(i),
                                            index.type, false, stmt.debug_loc);
        S9ValueId eq_value = defineGenerated(ctx, S9Type{S8TypeKind::Bool, 1},
                                             "lookupwrite_eq", out_block.id);
        out_block.stmts.push_back(makeOpStmt(eq_value, S9OpKind::Eq,
                                             S9Type{S8TypeKind::Bool, 1},
                                             {index, std::move(case_lit)},
                                             stmt.debug_loc,
                                             "lowered_lookupwrite"));
        S9Operand new_value = castTo(ctx, value, target_type, out_block.stmts,
                                     stmt.debug_loc, out_block.id,
                                     "lookupwrite_value_cast");
        S9Operand old_value = castTo(ctx, old_elements[i], target_type, out_block.stmts,
                                     stmt.debug_loc, out_block.id,
                                     "lookupwrite_old_cast");
        S9ValueId target = defineBase(ctx, env, target_symbol, S9ValueKind::Statement,
                                      stmt.debug_loc, out_block.id);
        out_block.stmts.push_back(makeOpStmt(target, S9OpKind::Mux, target_type,
                                             {valueOperand(ctx.output, eq_value, false,
                                                           stmt.debug_loc),
                                              std::move(new_value),
                                              std::move(old_value)},
                                             stmt.debug_loc,
                                             "lowered_lookupwrite"));
        ++ctx.summary.lowered_lookupwrites;
    }
}

void rewriteStmt(Context& ctx,
                 const S8Stmt& stmt,
                 Env& env,
                 S9BasicBlock& out_block) {
    switch (stmt.kind) {
    case S8StmtKind::Assign: {
        S9Stmt out;
        out.kind = S9StmtKind::Assign;
        out.debug_loc = stmt.debug_loc;
        out.value = rewriteOperand(ctx, stmt.value, env, out_block.id);
        out.target = defineBase(ctx, env, stmt.target, S9ValueKind::Statement,
                                stmt.debug_loc, out_block.id);
        out_block.stmts.push_back(std::move(out));
        break;
    }
    case S8StmtKind::Op: {
        S9Stmt out;
        out.kind = S9StmtKind::Op;
        out.debug_loc = stmt.debug_loc;
        out.op = copyOperation(stmt.op);
        for (const auto& operand : stmt.op.operands) {
            out.op.operands.push_back(rewriteOperand(ctx, operand, env, out_block.id));
        }
        out.target = defineBase(ctx, env, stmt.target, S9ValueKind::Statement,
                                stmt.debug_loc, out_block.id);
        out_block.stmts.push_back(std::move(out));
        break;
    }
    case S8StmtKind::Lookup: {
        S9Stmt out;
        out.kind = S9StmtKind::Lookup;
        out.debug_loc = stmt.debug_loc;
        out.lookup_index = rewriteOperand(ctx, stmt.lookup_index, env, out_block.id);
        for (const auto& elem : stmt.lookup_elements) {
            out.lookup_elements.push_back(rewriteOperand(ctx, elem, env, out_block.id));
        }
        out.target = defineBase(ctx, env, stmt.target, S9ValueKind::Statement,
                                stmt.debug_loc, out_block.id);
        out_block.stmts.push_back(std::move(out));
        break;
    }
    case S8StmtKind::LookupWrite:
        rewriteLookupWrite(ctx, stmt, env, out_block);
        break;
    }
}

S9Terminator rewriteTerminator(Context& ctx,
                               const S8Terminator& term,
                               const Env& env,
                               BlockId block) {
    S9Terminator out;
    out.kind = term.kind;
    out.jump_target = term.jump_target;
    out.true_target = term.true_target;
    out.false_target = term.false_target;
    out.default_target = term.default_target;
    if (term.kind == S8TermKind::Branch) {
        out.condition = rewriteOperand(ctx, term.condition, env, block,
                                       "branch condition");
    } else if (term.kind == S8TermKind::Switch) {
        out.switch_value = rewriteOperand(ctx, term.switch_value, env, block,
                                          "switch value");
        for (const auto& target : term.switch_targets) {
            S9SwitchTarget converted;
            converted.target = target.target;
            if (target.value) {
                converted.value = rewriteOperand(ctx, target.value.value(), env, block,
                                                 "switch case value");
            }
            out.switch_targets.push_back(std::move(converted));
        }
    }
    return out;
}

Env initialEnv(Context& ctx) {
    Env env(ctx.output.base_symbols.size());
    for (const auto& port : ctx.input->ports) {
        const auto& symbol = s8SymbolAt(*ctx.input, port.symbol);
        S9ValueId value = addValue(ctx, port.symbol, symbol.type, S9ValueKind::Initial,
                                   symbol.debug_name, -1, 0);
        env[static_cast<std::size_t>(port.symbol)] = value;
    }
    return env;
}

Env mergePredecessorEnvs(Context& ctx,
                         const CFGInfo& cfg,
                         const Liveness& live,
                         const std::vector<Env>& env_out,
                         S9BasicBlock& block) {
    const auto& preds = cfg.preds[static_cast<std::size_t>(block.id)];
    if (block.id == ctx.input->entry) return initialEnv(ctx);
    if (preds.empty()) {
        fail("Reachable non-entry block has no predecessors", {}, "bb" + std::to_string(block.id));
    }
    if (preds.size() == 1) return env_out[static_cast<std::size_t>(preds.front())];

    Env env(ctx.output.base_symbols.size());
    const auto& live_in = live.live_in[static_cast<std::size_t>(block.id)];
    for (SymbolId symbol : live_in) {
        std::optional<S9ValueId> first;
        bool all_same = true;
        std::vector<S9PhiIncoming> incoming;
        for (BlockId pred : preds) {
            if (!cfg.reachable[static_cast<std::size_t>(pred)]) continue;
            const Env& pred_env = env_out[static_cast<std::size_t>(pred)];
            if (symbol < 0 || symbol >= static_cast<SymbolId>(pred_env.size()) ||
                !pred_env[static_cast<std::size_t>(symbol)]) {
                fail("Missing incoming SSA value at CFG merge", {},
                     "bb" + std::to_string(block.id) + " symbol=" +
                         s8SymbolAt(*ctx.input, symbol).debug_name);
            }
            S9ValueId value = pred_env[static_cast<std::size_t>(symbol)].value();
            if (!first) first = value;
            else if (*first != value) all_same = false;
            incoming.push_back(S9PhiIncoming{pred, value});
        }
        if (!first) continue;
        if (all_same) {
            env[static_cast<std::size_t>(symbol)] = *first;
            continue;
        }
        const auto& base = s8SymbolAt(*ctx.input, symbol);
        S9ValueId result = addValue(ctx, symbol, base.type, S9ValueKind::Phi,
                                    base.debug_name, block.id);
        S9Phi phi;
        phi.result = result;
        phi.base_symbol = symbol;
        phi.incoming = std::move(incoming);
        block.phis.push_back(std::move(phi));
        env[static_cast<std::size_t>(symbol)] = result;
        ++ctx.summary.phis;
    }
    return env;
}

S9SSACFG buildFunction(const S8NormCFG& fn,
                       const SSAOptions& options,
                       SSASummary& summary) {
    verifyNormProgram(S8NormProgram{fn});
    CFGInfo cfg = analyzeCFG(fn);
    Liveness live = computeLiveness(fn, cfg);

    Context ctx;
    ctx.input = &fn;
    ctx.options = options;
    ctx.output.name = fn.name;
    ctx.output.entry = fn.entry;
    ctx.output.exit = cfg.effective_exit;
    ctx.summary.function_name = fn.name;
    ctx.output.base_symbols.reserve(fn.symbols.size());
    ctx.next_version.assign(fn.symbols.size(), 0);
    for (const auto& symbol : fn.symbols) {
        S9Symbol out;
        out.id = symbol.id;
        out.type = symbol.type;
        out.debug_name = symbol.debug_name;
        out.role = symbol.role;
        if (out.id != static_cast<SymbolId>(ctx.output.base_symbols.size())) {
            fail("S8 symbols must be dense before S9 SSA");
        }
        ctx.output.base_symbols.push_back(std::move(out));
    }
    for (const auto& port : fn.ports) {
        S9Port out;
        out.symbol = port.symbol;
        out.direction = port.direction;
        out.passing = port.passing;
        ctx.output.ports.push_back(out);
    }
    for (const auto& group : fn.port_groups) {
        S9PortGroup out;
        out.source_name = group.source_name;
        out.direction = group.direction;
        out.passing = group.passing;
        out.source_type = group.source_type;
        out.scalar_source_type = group.scalar_source_type;
        out.scalar_type = group.scalar_type;
        out.array_dims = group.array_dims;
        for (const auto& element : group.elements) {
            S9PortElement out_element;
            out_element.symbol = element.symbol;
            out_element.indices = element.indices;
            out.elements.push_back(std::move(out_element));
        }
        ctx.output.port_groups.push_back(std::move(out));
    }
    ctx.output.blocks.resize(fn.blocks.size());
    for (const auto& block : fn.blocks) {
        S9BasicBlock out;
        out.id = block.id;
        out.reachable = cfg.reachable[static_cast<std::size_t>(block.id)];
        ctx.output.blocks[static_cast<std::size_t>(block.id)] = std::move(out);
    }

    std::vector<Env> env_out(fn.blocks.size());
    for (BlockId block_id : cfg.topo) {
        S9BasicBlock& out_block = ctx.output.blocks[static_cast<std::size_t>(block_id)];
        Env env = mergePredecessorEnvs(ctx, cfg, live, env_out, out_block);
        const auto& block = fn.blocks[static_cast<std::size_t>(block_id)];
        for (const auto& stmt : block.stmts) rewriteStmt(ctx, stmt, env, out_block);
        out_block.terminator = rewriteTerminator(ctx, block.terminator, env, block_id);
        env_out[static_cast<std::size_t>(block_id)] = std::move(env);
        ++ctx.summary.reachable_blocks;
    }

    const Env& final_env = env_out[static_cast<std::size_t>(cfg.effective_exit)];
    for (auto& port : ctx.output.ports) {
        if (port.symbol >= 0 &&
            port.symbol < static_cast<SymbolId>(final_env.size()) &&
            final_env[static_cast<std::size_t>(port.symbol)]) {
            port.final_value = final_env[static_cast<std::size_t>(port.symbol)].value();
        }
        for (const auto& value : ctx.output.values) {
            if (value.base_symbol == port.symbol && value.kind == S9ValueKind::Initial) {
                port.initial_value = value.id;
                break;
            }
        }
        if (isFinalPort(S8Port{port.symbol, port.direction, port.passing}) &&
            !port.final_value) {
            fail("Output port has no final SSA value", {},
                 "symbol=" + symbolName(ctx.output, port.symbol));
        }
    }
    ctx.summary.values = static_cast<int>(ctx.output.values.size());
    summary = ctx.summary;
    return std::move(ctx.output);
}

std::string operandText(const S9SSACFG& fn, const S9Operand& operand) {
    std::ostringstream os;
    if (operand.signed_view) os << "s:";
    if (operand.kind == S9OperandKind::Value) {
        os << valueName(fn, operand.value);
    } else {
        os << wordsHex(operand.literal.words, operand.literal.valid_width);
    }
    os << "<" << typeText(operand.type) << ">";
    return os.str();
}

std::string stmtText(const S9SSACFG& fn, const S9Stmt& stmt) {
    std::ostringstream os;
    switch (stmt.kind) {
    case S9StmtKind::Assign:
        os << "assign " << valueName(fn, stmt.target) << " = "
           << operandText(fn, stmt.value);
        break;
    case S9StmtKind::Op:
        os << "op " << valueName(fn, stmt.target) << " = "
           << opName(stmt.op.kind) << "<" << stmt.op.result_width << ">(";
        for (std::size_t i = 0; i < stmt.op.operands.size(); ++i) {
            if (i) os << ", ";
            os << operandText(fn, stmt.op.operands[i]);
        }
        os << ")";
        if (stmt.op.hi >= 0 || stmt.op.lo >= 0 || stmt.op.bit >= 0 || stmt.op.times > 0) {
            os << " meta{hi=" << stmt.op.hi << ",lo=" << stmt.op.lo
               << ",bit=" << stmt.op.bit << ",times=" << stmt.op.times << "}";
        }
        break;
    case S9StmtKind::Lookup:
        os << "lookup " << valueName(fn, stmt.target) << " = lookup("
           << operandText(fn, stmt.lookup_index);
        for (const auto& elem : stmt.lookup_elements) os << ", " << operandText(fn, elem);
        os << ")";
        break;
    }
    if (!stmt.debug_note.empty()) os << " ; " << stmt.debug_note;
    return os.str();
}

void verifyOperand(const S9SSACFG& fn, const S9Operand& operand) {
    if (operand.type.width <= 0) fail("S9 operand has invalid width");
    if (operand.kind == S9OperandKind::Value) {
        const auto& value = valueAt(fn, operand.value);
        if (!typeEq(value.type, operand.type)) fail("S9 operand type mismatch");
    } else {
        if (operand.literal.valid_width != operand.type.width) {
            fail("S9 literal width does not match operand type");
        }
        if (static_cast<int>(operand.literal.words.size()) != wordCount(operand.literal.valid_width)) {
            fail("S9 literal word count mismatch");
        }
    }
}

} // namespace

void verifySSAProgram(const S9SSAProgram& program) {
    const auto& fn = program.top;
    if (fn.entry < 0 || fn.entry >= static_cast<BlockId>(fn.blocks.size())) {
        fail("S9 CFG has invalid entry");
    }
    if (fn.exit < 0 || fn.exit >= static_cast<BlockId>(fn.blocks.size())) {
        fail("S9 CFG has invalid exit");
    }
    for (std::size_t i = 0; i < fn.base_symbols.size(); ++i) {
        const auto& symbol = fn.base_symbols[i];
        if (symbol.id != static_cast<SymbolId>(i)) fail("S9 base symbol ids must be dense");
        if (symbol.type.width <= 0) fail("S9 base symbol has invalid type");
    }
    for (std::size_t i = 0; i < fn.values.size(); ++i) {
        const auto& value = fn.values[i];
        if (value.id != static_cast<S9ValueId>(i)) fail("S9 value ids must be dense");
        if (value.type.width <= 0) fail("S9 value has invalid type");
        if (value.base_symbol >= 0) {
            if (value.base_symbol >= static_cast<SymbolId>(fn.base_symbols.size())) {
                fail("S9 value has invalid base symbol");
            }
            if (!typeEq(value.type, fn.base_symbols[static_cast<std::size_t>(value.base_symbol)].type)) {
                fail("S9 value type does not match base symbol type");
            }
        }
    }

    std::vector<std::vector<BlockId>> preds(fn.blocks.size());
    for (const auto& block : fn.blocks) {
        if (block.id < 0 || block.id >= static_cast<BlockId>(fn.blocks.size())) {
            fail("S9 block has invalid id");
        }
        auto add_pred = [&](BlockId succ) {
            if (succ < 0 || succ >= static_cast<BlockId>(fn.blocks.size())) {
                fail("S9 terminator has invalid successor");
            }
            preds[static_cast<std::size_t>(succ)].push_back(block.id);
        };
        if (block.terminator.kind == S9TermKind::Jump) add_pred(block.terminator.jump_target);
        else if (block.terminator.kind == S9TermKind::Branch) {
            verifyOperand(fn, block.terminator.condition);
            if (!isBoolType(block.terminator.condition.type)) {
                fail("S9 branch condition must be bool");
            }
            add_pred(block.terminator.true_target);
            add_pred(block.terminator.false_target);
        } else if (block.terminator.kind == S9TermKind::Switch) {
            verifyOperand(fn, block.terminator.switch_value);
            for (const auto& target : block.terminator.switch_targets) {
                if (target.value) verifyOperand(fn, target.value.value());
                add_pred(target.target);
            }
            add_pred(block.terminator.default_target);
        }
    }
    for (auto& block_preds : preds) {
        std::sort(block_preds.begin(), block_preds.end());
        block_preds.erase(std::unique(block_preds.begin(), block_preds.end()), block_preds.end());
    }

    for (const auto& block : fn.blocks) {
        if (!block.reachable) continue;
        for (const auto& phi : block.phis) {
            const auto& result = valueAt(fn, phi.result);
            if (result.kind != S9ValueKind::Phi) fail("S9 phi result is not a phi value");
            if (result.base_symbol != phi.base_symbol) fail("S9 phi base symbol mismatch");
            std::set<BlockId> seen_preds;
            for (const auto& incoming : phi.incoming) {
                if (!seen_preds.insert(incoming.pred).second) {
                    fail("S9 phi has duplicate incoming predecessor");
                }
                const auto& value = valueAt(fn, incoming.value);
                if (!typeEq(value.type, result.type)) fail("S9 phi incoming type mismatch");
            }
            for (BlockId pred : preds[static_cast<std::size_t>(block.id)]) {
                if (fn.blocks[static_cast<std::size_t>(pred)].reachable &&
                    !seen_preds.count(pred)) {
                    fail("S9 phi is missing a reachable predecessor");
                }
            }
        }
        for (const auto& stmt : block.stmts) {
            const auto& target = valueAt(fn, stmt.target);
            switch (stmt.kind) {
            case S9StmtKind::Assign:
                verifyOperand(fn, stmt.value);
                if (!typeEq(target.type, stmt.value.type)) fail("S9 assign type mismatch");
                break;
            case S9StmtKind::Op:
                if (stmt.op.result_width != target.type.width) {
                    fail("S9 op result width does not match target value");
                }
                for (const auto& operand : stmt.op.operands) verifyOperand(fn, operand);
                break;
            case S9StmtKind::Lookup:
                verifyOperand(fn, stmt.lookup_index);
                for (const auto& elem : stmt.lookup_elements) {
                    verifyOperand(fn, elem);
                    if (!typeEq(elem.type, target.type)) fail("S9 lookup element type mismatch");
                }
                break;
            }
        }
    }
    for (const auto& port : fn.ports) {
        if (port.initial_value) (void)valueAt(fn, port.initial_value.value());
        if (port.final_value) (void)valueAt(fn, port.final_value.value());
    }
}

std::string debugPrint(const S9SSAProgram& program,
                       const std::vector<SSASummary>& summaries) {
    std::ostringstream os;
    os << "s9ssa\n";
    for (const auto& summary : summaries) {
        os << "summary function=" << summary.function_name
           << " values=" << summary.values
           << " phis=" << summary.phis
           << " lowered_lookupwrites=" << summary.lowered_lookupwrites
           << " generated_ops=" << summary.generated_ops
           << " reachable_blocks=" << summary.reachable_blocks << "\n";
    }
    const auto& fn = program.top;
    os << "top " << fn.name << " entry=bb" << fn.entry << " exit=bb" << fn.exit << "\n";
    os << "symbols\n";
    for (const auto& symbol : fn.base_symbols) {
        os << "  %" << symbol.id << " " << symbol.debug_name
           << " " << typeText(symbol.type) << "\n";
    }
    os << "values\n";
    for (const auto& value : fn.values) {
        os << "  $" << value.id << " " << valueName(fn, value.id)
           << " " << typeText(value.type)
           << " kind=" << valueKindName(value.kind);
        if (value.def_block >= 0) os << " def=bb" << value.def_block;
        os << "\n";
    }
    os << "ports\n";
    for (const auto& port : fn.ports) {
        os << "  " << symbolName(fn, port.symbol);
        if (port.initial_value) os << " initial=" << valueName(fn, port.initial_value.value());
        if (port.final_value) os << " final=" << valueName(fn, port.final_value.value());
        os << "\n";
    }
    for (const auto& block : fn.blocks) {
        os << "  bb" << block.id;
        if (!block.reachable) os << " unreachable";
        os << "\n";
        for (const auto& phi : block.phis) {
            os << "    phi " << valueName(fn, phi.result) << " = phi(";
            for (std::size_t i = 0; i < phi.incoming.size(); ++i) {
                if (i) os << ", ";
                os << "bb" << phi.incoming[i].pred << ":"
                   << valueName(fn, phi.incoming[i].value);
            }
            os << ")\n";
        }
        for (const auto& stmt : block.stmts) os << "    " << stmtText(fn, stmt) << "\n";
        os << "    term " << termName(block.terminator.kind);
        if (block.terminator.kind == S9TermKind::Branch) {
            os << " " << operandText(fn, block.terminator.condition)
               << " ? bb" << block.terminator.true_target
               << " : bb" << block.terminator.false_target;
        } else if (block.terminator.kind == S9TermKind::Jump) {
            os << " bb" << block.terminator.jump_target;
        } else if (block.terminator.kind == S9TermKind::Switch) {
            os << " " << operandText(fn, block.terminator.switch_value);
        }
        os << "\n";
    }
    return os.str();
}

SSAResult buildSSA(const S8NormProgram& program,
                   const SSAOptions& options) {
    try {
        SSAResult result;
        SSASummary summary;
        S9SSAProgram out;
        out.top = buildFunction(program.top, options, summary);
        verifySSAProgram(out);
        result.summaries.push_back(summary);
        if (options.debug_print) result.debug_text = debugPrint(out, result.summaries);
        result.program = std::move(out);
        return result;
    } catch (const RTLZZException& ex) {
        SSAResult result;
        SSABuildError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        SSAResult result;
        SSABuildError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

S9SSAProgram buildSSAOrThrow(const S8NormProgram& program,
                             const SSAOptions& options) {
    auto result = buildSSA(program, options);
    if (!result.ok()) throw RTLZZException(result.error->context, result.error->message);
    return std::move(result.program.value());
}

} // namespace pred::s9ssa
