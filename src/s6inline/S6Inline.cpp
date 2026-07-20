#include "s6inline/S6Inline.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s6inline {
namespace {

using namespace pred::s3statementize;
using namespace pred::s4cfg;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s6inline";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

bool isVoidType(const TypeInfo& type) {
    return type.name == "void" && type.struct_name.empty() && !type.is_array;
}

bool sameTypeRelaxed(TypeInfo lhs, TypeInfo rhs) {
    lhs.is_const = false;
    rhs.is_const = false;
    lhs.is_reference = false;
    rhs.is_reference = false;
    lhs.is_pointer = false;
    rhs.is_pointer = false;
    if (!lhs.struct_name.empty() || !rhs.struct_name.empty()) {
        return lhs.struct_name == rhs.struct_name;
    }
    if (lhs.is_array || rhs.is_array) {
        return lhs.is_array == rhs.is_array &&
               lhs.array_size == rhs.array_size &&
               lhs.array_dims == rhs.array_dims;
    }
    if (lhs.width > 0 && rhs.width > 0 && lhs.width != rhs.width) return false;
    if (!lhs.hw_kind.empty() && !rhs.hw_kind.empty() && lhs.hw_kind != rhs.hw_kind) {
        return false;
    }
    return true;
}

std::string sanitizeName(std::string text) {
    for (char& c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) c = '_';
    }
    if (text.empty()) text = "callee";
    return text;
}

std::string uniqueSymbolName(const FunctionCFG& fn, const std::string& base) {
    std::unordered_set<std::string> used;
    for (const auto& symbol : fn.symbols) used.insert(symbol.name);
    if (!used.count(base)) return base;
    for (int i = 0;; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!used.count(candidate)) return candidate;
    }
}

const SymbolInfo& symbolInfo(const FunctionCFG& fn, SymbolId symbol) {
    if (symbol < 0 || symbol >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("SymbolId is outside the function symbol table");
    }
    const auto& info = fn.symbols[static_cast<std::size_t>(symbol)];
    if (info.id != symbol) fail("Symbol table id invariant is broken");
    return info;
}

std::optional<SymbolId> findParamSymbol(const FunctionCFG& fn, const ParamDecl& param) {
    for (const auto& symbol : fn.symbols) {
        if (symbol.name == param.name && symbol.is_param) return symbol.id;
    }
    for (const auto& symbol : fn.symbols) {
        if (symbol.name == param.name) return symbol.id;
    }
    return std::nullopt;
}

Operand varOperand(const SymbolInfo& symbol) {
    Operand out;
    out.kind = OperandKind::Var;
    out.var_name = symbol.name;
    out.var_symbol = symbol.id;
    out.type = symbol.type;
    return out;
}

Operand lvalueOperand(LValue lv) {
    Operand out;
    out.kind = lv.accesses.empty() ? OperandKind::Var : OperandKind::LValueRead;
    out.type = lv.type;
    if (out.kind == OperandKind::Var) {
        out.var_name = lv.root;
        out.var_symbol = lv.root_symbol;
    } else {
        out.lvalue = std::move(lv);
    }
    return out;
}

LValue operandAsWritableLValue(const Operand& operand, const ParamDecl& param) {
    if (operand.kind == OperandKind::Var) {
        LValue out;
        out.root = operand.var_name;
        out.root_symbol = operand.var_symbol;
        out.type = operand.type;
        return out;
    }
    if (operand.kind == OperandKind::LValueRead) return operand.lvalue;
    fail("Mutable/output parameter '" + param.name +
         "' must be bound to a writable lvalue");
}

S3StmtPtr makeDecl(const SymbolInfo& symbol) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Decl;
    stmt->decl_name = symbol.name;
    stmt->decl_symbol = symbol.id;
    stmt->decl_type = symbol.type;
    return stmt;
}

S3StmtPtr makeAssign(LValue target, Operand value) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Assign;
    stmt->target = std::move(target);
    stmt->value = std::move(value);
    return stmt;
}

LValue symbolLValue(const SymbolInfo& symbol) {
    LValue out;
    out.root = symbol.name;
    out.root_symbol = symbol.id;
    out.type = symbol.type;
    return out;
}

FunctionCFG cloneFunction(const FunctionCFG& input) {
    FunctionCFG out;
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

struct SymbolBinding {
    enum class Kind { FreshSymbol, AliasLValue };
    Kind kind = Kind::FreshSymbol;
    SymbolId symbol = -1;
    LValue alias;
};

using SymbolMap = std::unordered_map<SymbolId, SymbolBinding>;

LValue cloneLValue(const LValue& in);
Operand cloneOperand(const Operand& in);

LValue cloneLValue(const LValue& in) {
    LValue out = in;
    out.accesses.clear();
    for (const auto& access : in.accesses) {
        LValueAccess copied = access;
        if (access.index) copied.index = std::make_shared<Operand>(cloneOperand(*access.index));
        out.accesses.push_back(std::move(copied));
    }
    return out;
}

Operand cloneOperand(const Operand& in) {
    Operand out = in;
    if (in.kind == OperandKind::LValueRead) out.lvalue = cloneLValue(in.lvalue);
    return out;
}

LValue applyLValueBinding(const FunctionCFG& fn, const LValue& input, const SymbolMap& map) {
    LValue out = cloneLValue(input);
    auto found = map.find(input.root_symbol);
    if (found != map.end()) {
        const auto& binding = found->second;
        if (binding.kind == SymbolBinding::Kind::FreshSymbol) {
            const auto& info = symbolInfo(fn, binding.symbol);
            out.root = info.name;
            out.root_symbol = info.id;
        } else {
            LValue alias = cloneLValue(binding.alias);
            alias.accesses.insert(alias.accesses.end(), out.accesses.begin(), out.accesses.end());
            alias.type = out.type;
            out = std::move(alias);
        }
    }
    for (auto& access : out.accesses) {
        if (access.index) {
            *access.index = cloneOperand(*access.index);
            Operand remapped = *access.index;
            if (remapped.kind == OperandKind::Var ||
                remapped.kind == OperandKind::LValueRead) {
                // Re-enter through applyOperandBinding below without a forward
                // declaration cycle in the public helper shape.
            }
        }
    }
    return out;
}

Operand applyOperandBinding(const FunctionCFG& fn, const Operand& input, const SymbolMap& map);

void remapLValueAccessOperands(const FunctionCFG& fn, LValue& value, const SymbolMap& map) {
    for (auto& access : value.accesses) {
        if (access.index) {
            *access.index = applyOperandBinding(fn, *access.index, map);
        }
    }
}

Operand applyOperandBinding(const FunctionCFG& fn, const Operand& input, const SymbolMap& map) {
    if (input.kind == OperandKind::Var) {
        auto found = map.find(input.var_symbol);
        if (found == map.end()) return input;
        const auto& binding = found->second;
        if (binding.kind == SymbolBinding::Kind::FreshSymbol) {
            return varOperand(symbolInfo(fn, binding.symbol));
        }
        LValue alias = cloneLValue(binding.alias);
        remapLValueAccessOperands(fn, alias, map);
        return lvalueOperand(std::move(alias));
    }
    if (input.kind == OperandKind::LValueRead) {
        LValue lv = applyLValueBinding(fn, input.lvalue, map);
        remapLValueAccessOperands(fn, lv, map);
        return lvalueOperand(std::move(lv));
    }
    return input;
}

LValue remapLValue(const FunctionCFG& fn, const LValue& input, const SymbolMap& map) {
    LValue out = applyLValueBinding(fn, input, map);
    remapLValueAccessOperands(fn, out, map);
    return out;
}

S3StmtPtr remapStmt(const FunctionCFG& fn, const S3StmtPtr& stmt, const SymbolMap& map);

std::vector<S3StmtPtr> remapStmtList(const FunctionCFG& fn,
                                     const std::vector<S3StmtPtr>& input,
                                     const SymbolMap& map) {
    std::vector<S3StmtPtr> out;
    out.reserve(input.size());
    for (const auto& stmt : input) out.push_back(remapStmt(fn, stmt, map));
    return out;
}

S3StmtPtr remapStmt(const FunctionCFG& fn, const S3StmtPtr& stmt, const SymbolMap& map) {
    if (!stmt) return nullptr;
    auto out = std::make_shared<S3Stmt>(*stmt);
    if (stmt->kind == S3StmtKind::Decl) {
        auto found = map.find(stmt->decl_symbol);
        if (found != map.end() &&
            found->second.kind == SymbolBinding::Kind::FreshSymbol) {
            const auto& info = symbolInfo(fn, found->second.symbol);
            out->decl_symbol = info.id;
            out->decl_name = info.name;
            out->decl_type = info.type;
        }
    }
    out->target = remapLValue(fn, stmt->target, map);
    out->value = applyOperandBinding(fn, stmt->value, map);
    for (auto& operand : out->op.operands) {
        operand = applyOperandBinding(fn, operand, map);
    }
    if (stmt->call_result) out->call_result = remapLValue(fn, stmt->call_result.value(), map);
    for (auto& arg : out->args) arg = applyOperandBinding(fn, arg, map);
    out->condition = applyOperandBinding(fn, stmt->condition, map);
    out->condition_prelude = remapStmtList(fn, stmt->condition_prelude, map);
    out->then_body = remapStmtList(fn, stmt->then_body, map);
    out->else_body = remapStmtList(fn, stmt->else_body, map);
    out->for_init = remapStmtList(fn, stmt->for_init, map);
    if (stmt->for_cond) out->for_cond = applyOperandBinding(fn, stmt->for_cond.value(), map);
    out->for_step = remapStmtList(fn, stmt->for_step, map);
    out->loop_body = remapStmtList(fn, stmt->loop_body, map);
    out->switch_value = applyOperandBinding(fn, stmt->switch_value, map);
    for (auto& c : out->switch_cases) {
        if (c.value) c.value = applyOperandBinding(fn, c.value.value(), map);
        c.body = remapStmtList(fn, c.body, map);
    }
    if (stmt->return_value) out->return_value = applyOperandBinding(fn, stmt->return_value.value(), map);
    return out;
}

Terminator remapTerminator(const FunctionCFG& fn,
                           const Terminator& term,
                           const SymbolMap& symbol_map,
                           const std::unordered_map<BlockId, BlockId>& block_map,
                           BlockId source_exit) {
    Terminator out = term;
    out.condition = applyOperandBinding(fn, term.condition, symbol_map);
    out.switch_value = applyOperandBinding(fn, term.switch_value, symbol_map);
    if (term.return_value) out.return_value = applyOperandBinding(fn, term.return_value.value(), symbol_map);
    auto map_block = [&](BlockId id) {
        auto it = block_map.find(id);
        if (it == block_map.end()) fail("Internal error: missing cloned block mapping");
        return it->second;
    };
    switch (term.kind) {
    case TermKind::Jump:
        out.jump_target = map_block(term.jump_target);
        break;
    case TermKind::Branch:
        out.true_target = map_block(term.true_target);
        out.false_target = map_block(term.false_target);
        break;
    case TermKind::Switch:
        for (auto& target : out.switch_targets) {
            if (target.value) target.value = applyOperandBinding(fn, target.value.value(), symbol_map);
            target.target = map_block(target.target);
        }
        out.default_target = map_block(term.default_target);
        break;
    case TermKind::Return:
        out.kind = TermKind::Jump;
        out.return_value.reset();
        out.jump_target = map_block(source_exit);
        break;
    case TermKind::Unreachable:
    case TermKind::Exit:
        break;
    }
    return out;
}

std::string edgeKindName(EdgeKind kind) {
    switch (kind) {
    case EdgeKind::Fallthrough: return "fallthrough";
    case EdgeKind::Jump: return "jump";
    case EdgeKind::True: return "true";
    case EdgeKind::False: return "false";
    case EdgeKind::Case: return "case";
    case EdgeKind::Default: return "default";
    case EdgeKind::Break: return "break";
    case EdgeKind::Continue: return "continue";
    case EdgeKind::Return: return "return";
    }
    return "edge";
}

void addEdge(FunctionCFG& fn,
             BlockId from,
             BlockId to,
             EdgeKind kind,
             std::string label,
             std::optional<Operand> case_value = std::nullopt) {
    if (from < 0 || to < 0 ||
        from >= static_cast<BlockId>(fn.blocks.size()) ||
        to >= static_cast<BlockId>(fn.blocks.size())) {
        fail("CFG edge points to an invalid block");
    }
    if (label.empty()) label = edgeKindName(kind);
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
    for (auto& block : fn.blocks) {
        block->successors.clear();
        block->predecessors.clear();
    }
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
                addEdge(fn, block.id, target.target, EdgeKind::Case, "case", target.value);
            }
            addEdge(fn, block.id, block.terminator.default_target, EdgeKind::Default, "default");
            break;
        case TermKind::Return:
            addEdge(fn, block.id, fn.exit, EdgeKind::Return, "return");
            break;
        case TermKind::Unreachable:
        case TermKind::Exit:
            break;
        }
    }
}

void setJump(BasicBlock& block, BlockId target) {
    block.terminator = Terminator{};
    block.terminator.kind = TermKind::Jump;
    block.terminator.jump_target = target;
}

BasicBlock* appendBlock(FunctionCFG& fn) {
    auto block = std::make_unique<BasicBlock>();
    block->id = static_cast<BlockId>(fn.blocks.size());
    auto* out = block.get();
    fn.blocks.push_back(std::move(block));
    return out;
}

SymbolInfo cloneSymbol(FunctionCFG& caller,
                       const FunctionCFG& callee,
                       SymbolId old_symbol,
                       const std::string& site_prefix,
                       bool cloned_param) {
    SymbolInfo info = symbolInfo(callee, old_symbol);
    info.id = static_cast<SymbolId>(caller.symbols.size());
    info.name = uniqueSymbolName(caller, site_prefix + "_" + sanitizeName(info.name) + "_");
    info.declaring_scope = -1;
    info.source_valid_scope_ids.clear();
    info.is_param = false;
    if (cloned_param) info.is_temp = true;
    caller.symbols.push_back(info);
    return caller.symbols.back();
}

struct ResolvedCallee {
    enum class Kind { Helper, Lambda };
    Kind kind = Kind::Helper;
    std::size_t helper_index = 0;
    std::string lambda_key;
    const FunctionCFG* function = nullptr;

    std::string key() const {
        if (kind == Kind::Lambda) return "lambda:" + lambda_key;
        return "helper:" + std::to_string(helper_index);
    }
};

class InlineEngine {
public:
    InlineEngine(const CFGProgram& input,
                 const InlineOptions& options,
                 std::vector<InlineWarning>& warnings,
                 std::vector<InlineSummary>& summaries)
        : input_(input), options_(options), warnings_(warnings), summaries_(summaries) {}

    InlinedCFGProgram run() {
        verifyNoLoops(input_.top);
        for (const auto& helper : input_.helpers) verifyNoLoops(helper);
        for (const auto& [_, lambda] : input_.lambdas) verifyNoLoops(lambda);

        FunctionCFG top = cloneFunction(input_.top);
        inlineFunction(top, 0);
        verifyNoResidualCalls(top);
        verifyFunction(top);
        InlinedCFGProgram out;
        out.struct_fields = input_.struct_fields;
        out.struct_constructors = input_.struct_constructors;
        out.top = convertFunction(top);
        verifyInlinedFunction(out.top);
        return out;
    }

private:
    const CFGProgram& input_;
    const InlineOptions& options_;
    std::vector<InlineWarning>& warnings_;
    std::vector<InlineSummary>& summaries_;
    std::unordered_map<std::string, FunctionCFG> cache_;
    std::vector<std::string> stack_;
    int call_site_counter_ = 0;

    void warn(const std::string& message, const std::string& note = {}) {
        InlineWarning warning;
        warning.context = makeContext(DebugLoc{}, note);
        warning.message = message;
        warnings_.push_back(std::move(warning));
    }

    void verifyNoLoops(const FunctionCFG& fn) const {
        if (!fn.loop_regions.empty()) fail("S6 input function '" + fn.name + "' still has loop regions");
        for (const auto& block : fn.blocks) {
            if (!block->loop_stack.empty()) {
                fail("S6 input function '" + fn.name + "' still has loop metadata");
            }
        }
    }

    ResolvedCallee resolve(const S3Stmt& call) const {
        std::vector<ResolvedCallee> lambda_matches;
        auto lambda_it = input_.lambdas.find(call.callee);
        if (lambda_it != input_.lambdas.end() &&
            paramsMatch(lambda_it->second.params, call.args)) {
            ResolvedCallee out;
            out.kind = ResolvedCallee::Kind::Lambda;
            out.lambda_key = lambda_it->first;
            out.function = &lambda_it->second;
            lambda_matches.push_back(out);
        }
        if (lambda_matches.size() == 1) return lambda_matches.front();

        std::vector<ResolvedCallee> helper_matches;
        for (std::size_t i = 0; i < input_.helpers.size(); ++i) {
            const auto& helper = input_.helpers[i];
            if (helper.name != call.callee) continue;
            if (!paramsMatch(helper.params, call.args)) continue;
            ResolvedCallee out;
            out.kind = ResolvedCallee::Kind::Helper;
            out.helper_index = i;
            out.function = &helper;
            helper_matches.push_back(out);
        }
        if (helper_matches.size() == 1) return helper_matches.front();
        if (lambda_matches.size() + helper_matches.size() > 1) {
            fail("Ambiguous helper/lambda call '" + call.callee + "'", call.debug_loc);
        }
        std::string detail;
        if (lambda_it != input_.lambdas.end()) {
            detail += "; lambda params=" +
                std::to_string(lambda_it->second.params.size()) +
                " args=" + std::to_string(call.args.size());
            const auto& params = lambda_it->second.params;
            const std::size_t count = std::min(params.size(), call.args.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (!sameTypeRelaxed(params[i].type, call.args[i].type)) {
                    detail += "; mismatch[" + std::to_string(i) + "] param=" +
                        params[i].type.name + "/" +
                        std::to_string(params[i].type.width) + "/" +
                        params[i].type.hw_kind + "/array=" +
                        std::to_string(params[i].type.is_array) + " arg=" +
                        call.args[i].type.name + "/" +
                        std::to_string(call.args[i].type.width) + "/" +
                        call.args[i].type.hw_kind + "/array=" +
                        std::to_string(call.args[i].type.is_array);
                }
            }
        }
        fail("Unknown helper/lambda call '" + call.callee + "'" + detail,
             call.debug_loc);
    }

    bool paramsMatch(const std::vector<ParamDecl>& params,
                     const std::vector<Operand>& args) const {
        if (params.size() != args.size()) return false;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (!sameTypeRelaxed(params[i].type, args[i].type)) return false;
        }
        return true;
    }

    const FunctionCFG& fullyInlinedCallee(const ResolvedCallee& resolved, int depth) {
        if (depth > options_.max_inline_depth) fail("S6 inline depth limit exceeded");
        std::string key = resolved.key();
        auto cached = cache_.find(key);
        if (cached != cache_.end()) return cached->second;
        if (std::find(stack_.begin(), stack_.end(), key) != stack_.end()) {
            fail("Recursive helper/lambda call graph reached S6");
        }
        stack_.push_back(key);
        FunctionCFG fn = cloneFunction(*resolved.function);
        inlineFunction(fn, depth + 1);
        verifyNoResidualCalls(fn);
        verifyFunction(fn);
        stack_.pop_back();
        auto inserted = cache_.emplace(key, std::move(fn));
        return inserted.first->second;
    }

    bool inlineFunction(FunctionCFG& fn, int depth) {
        bool changed = false;
        for (;;) {
            bool inlined_one = false;
            for (std::size_t bi = 0; bi < fn.blocks.size() && !inlined_one; ++bi) {
                auto& block = *fn.blocks[bi];
                for (std::size_t si = 0; si < block.stmts.size(); ++si) {
                    const auto& cfg_stmt = block.stmts[si];
                    if (!cfg_stmt.stmt || cfg_stmt.stmt->kind != S3StmtKind::Call) continue;
                    ResolvedCallee resolved = resolve(*cfg_stmt.stmt);
                    const FunctionCFG& callee = fullyInlinedCallee(resolved, depth + 1);
                    inlineSite(fn, block.id, si, callee, *cfg_stmt.stmt);
                    changed = true;
                    inlined_one = true;
                    break;
                }
            }
            if (!inlined_one) break;
        }
        return changed;
    }

    void inlineSite(FunctionCFG& caller,
                    BlockId call_block_id,
                    std::size_t stmt_index,
                    const FunctionCFG& callee,
                    const S3Stmt& call_stmt) {
        if (caller.blocks.size() > static_cast<std::size_t>(options_.max_cloned_blocks)) {
            fail("S6 cloned block limit exceeded");
        }
        BasicBlock& call_block = *caller.blocks[static_cast<std::size_t>(call_block_id)];
        Terminator old_term = call_block.terminator;
        std::vector<CFGStmt> post;
        post.assign(call_block.stmts.begin() + static_cast<std::ptrdiff_t>(stmt_index + 1),
                    call_block.stmts.end());
        call_block.stmts.erase(call_block.stmts.begin() + static_cast<std::ptrdiff_t>(stmt_index),
                               call_block.stmts.end());

        auto* continuation = appendBlock(caller);
        continuation->stmts = std::move(post);
        continuation->terminator = old_term;

        auto* binding = appendBlock(caller);

        SymbolMap symbol_map;
        std::string site_prefix = "__s6_" + sanitizeName(callee.name) + "_" +
                                  std::to_string(call_site_counter_++);
        bindParams(caller, callee, call_stmt, site_prefix, symbol_map, *binding);

        for (const auto& symbol : callee.symbols) {
            if (symbol_map.count(symbol.id)) continue;
            SymbolInfo cloned = cloneSymbol(caller, callee, symbol.id, site_prefix, false);
            symbol_map[symbol.id] = SymbolBinding{SymbolBinding::Kind::FreshSymbol,
                                                  cloned.id, {}};
        }

        std::unordered_map<BlockId, BlockId> block_map;
        for (const auto& old_block : callee.blocks) {
            auto* clone = appendBlock(caller);
            block_map[old_block->id] = clone->id;
        }

        bool needs_writeback = call_stmt.call_result.has_value() && !isVoidType(callee.return_type);
        auto* writeback = needs_writeback ? appendBlock(caller) : nullptr;

        for (const auto& old_block : callee.blocks) {
            auto& clone = *caller.blocks[static_cast<std::size_t>(block_map.at(old_block->id))];
            clone.stmts.clear();
            clone.loop_stack.clear();
            for (const auto& stmt : old_block->stmts) {
                clone.stmts.push_back(CFGStmt{stmt.kind, remapStmt(caller, stmt.stmt, symbol_map)});
            }
            clone.terminator = remapTerminator(caller, old_block->terminator,
                                               symbol_map, block_map, callee.exit);
            if (old_block->id == callee.exit) {
                setJump(clone, writeback ? writeback->id : continuation->id);
            }
        }

        if (needs_writeback) {
            if (callee.return_slot_symbol < 0) {
                fail("Non-void callee '" + callee.name + "' has no return slot");
            }
            auto found = symbol_map.find(callee.return_slot_symbol);
            if (found == symbol_map.end() ||
                found->second.kind != SymbolBinding::Kind::FreshSymbol) {
                fail("Internal error: cloned return slot is missing");
            }
            Operand value = varOperand(symbolInfo(caller, found->second.symbol));
            writeback->stmts.push_back(CFGStmt{
                CFGStmtKind::Assign,
                makeAssign(remapLValue(caller, call_stmt.call_result.value(), {}), value)});
            setJump(*writeback, continuation->id);
        } else if (call_stmt.call_result && isVoidType(callee.return_type)) {
            fail("Void callee '" + callee.name + "' cannot produce a call result",
                 call_stmt.debug_loc);
        } else if (!call_stmt.call_result && !isVoidType(callee.return_type)) {
            warn("Discarding non-void return value from callee '" + callee.name + "'",
                 caller.name);
        }

        setJump(*binding, block_map.at(callee.entry));
        setJump(call_block, binding->id);
        rebuildEdges(caller);

        InlineSummary summary;
        summary.caller = caller.name;
        summary.callee = callee.name;
        summary.call_block = call_block_id;
        summary.cloned_blocks = static_cast<int>(callee.blocks.size());
        summaries_.push_back(std::move(summary));

        if (caller.blocks.size() > static_cast<std::size_t>(options_.max_cloned_blocks)) {
            fail("S6 cloned block limit exceeded");
        }
    }

    void bindParams(FunctionCFG& caller,
                    const FunctionCFG& callee,
                    const S3Stmt& call_stmt,
                    const std::string& site_prefix,
                    SymbolMap& symbol_map,
                    BasicBlock& binding) {
        if (callee.params.size() != call_stmt.args.size()) {
            fail("Argument count mismatch for call '" + call_stmt.callee + "'",
                 call_stmt.debug_loc);
        }
        for (std::size_t i = 0; i < callee.params.size(); ++i) {
            const auto& param = callee.params[i];
            auto param_symbol = findParamSymbol(callee, param);
            if (!param_symbol) fail("Callee parameter symbol is missing: " + param.name);
            if (!sameTypeRelaxed(param.type, call_stmt.args[i].type)) {
                fail("Argument type mismatch for parameter '" + param.name + "'",
                     call_stmt.debug_loc);
            }
            if (param.passing == ParamPassingKind::MutableRef ||
                param.direction == ParamDirection::Output) {
                LValue alias = operandAsWritableLValue(call_stmt.args[i], param);
                symbol_map[*param_symbol] = SymbolBinding{
                    SymbolBinding::Kind::AliasLValue, -1, std::move(alias)};
                continue;
            }
            if (param.passing == ParamPassingKind::Pointer ||
                param.passing == ParamPassingKind::RValueRef) {
                fail("Pointer/RValueRef parameter reached S6 inline: " + param.name,
                     call_stmt.debug_loc);
            }
            SymbolInfo cloned = cloneSymbol(caller, callee, *param_symbol, site_prefix, true);
            symbol_map[*param_symbol] = SymbolBinding{SymbolBinding::Kind::FreshSymbol,
                                                      cloned.id, {}};
            binding.stmts.push_back(CFGStmt{CFGStmtKind::Decl, makeDecl(cloned)});
            binding.stmts.push_back(CFGStmt{
                CFGStmtKind::Assign,
                makeAssign(symbolLValue(cloned), cloneOperand(call_stmt.args[i]))});
        }
    }

    void verifyNoResidualCalls(const FunctionCFG& fn) const {
        for (const auto& block : fn.blocks) {
            for (const auto& stmt : block->stmts) {
                if (stmt.stmt && stmt.stmt->kind == S3StmtKind::Call) {
                    fail("S6 output still contains call '" + stmt.stmt->callee + "'",
                         stmt.stmt->debug_loc);
                }
            }
        }
    }

    void verifyFunction(const FunctionCFG& fn) const {
        verifyNoLoops(fn);
        if (fn.entry < 0 || fn.exit < 0 ||
            fn.entry >= static_cast<BlockId>(fn.blocks.size()) ||
            fn.exit >= static_cast<BlockId>(fn.blocks.size())) {
            fail("S6 CFG entry/exit is invalid");
        }
        for (std::size_t i = 0; i < fn.symbols.size(); ++i) {
            if (fn.symbols[i].id != static_cast<SymbolId>(i)) {
                fail("S6 symbol id is not function-local dense");
            }
        }
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            const auto& block = *fn.blocks[i];
            if (block.id != static_cast<BlockId>(i)) fail("S6 block id mismatch");
            if (block.id == fn.exit && block.terminator.kind != TermKind::Exit) {
                fail("S6 function exit must use Exit terminator");
            }
            verifyTerminatorTargets(fn, block);
            for (const auto& stmt : block.stmts) verifyStmtSymbols(fn, stmt.stmt);
        }
        verifyAcyclic(fn);
    }

    void verifyTarget(const FunctionCFG& fn, BlockId target) const {
        if (target < 0 || target >= static_cast<BlockId>(fn.blocks.size())) {
            fail("S6 CFG has invalid terminator target");
        }
    }

    void verifyTerminatorTargets(const FunctionCFG& fn, const BasicBlock& block) const {
        switch (block.terminator.kind) {
        case TermKind::Jump:
            verifyTarget(fn, block.terminator.jump_target);
            break;
        case TermKind::Branch:
            verifyTarget(fn, block.terminator.true_target);
            verifyTarget(fn, block.terminator.false_target);
            verifyOperandSymbol(fn, block.terminator.condition);
            break;
        case TermKind::Switch:
            verifyOperandSymbol(fn, block.terminator.switch_value);
            for (const auto& target : block.terminator.switch_targets) {
                verifyTarget(fn, target.target);
                if (target.value) verifyOperandSymbol(fn, target.value.value());
            }
            verifyTarget(fn, block.terminator.default_target);
            break;
        case TermKind::Return:
            if (block.id != fn.exit) verifyTarget(fn, fn.exit);
            if (block.terminator.return_value) verifyOperandSymbol(fn, block.terminator.return_value.value());
            break;
        case TermKind::Unreachable:
        case TermKind::Exit:
            break;
        }
    }

    void verifyAcyclic(const FunctionCFG& fn) const {
        enum class Mark { None, Visiting, Done };
        std::vector<Mark> marks(fn.blocks.size(), Mark::None);
        std::function<void(BlockId)> dfs = [&](BlockId id) {
            auto& mark = marks[static_cast<std::size_t>(id)];
            if (mark == Mark::Visiting) fail("S6 CFG contains a cycle");
            if (mark == Mark::Done) return;
            mark = Mark::Visiting;
            for (const auto& edge : fn.blocks[static_cast<std::size_t>(id)]->successors) {
                verifyTarget(fn, edge.to);
                dfs(edge.to);
            }
            mark = Mark::Done;
        };
        dfs(fn.entry);
    }

    void verifySymbol(const FunctionCFG& fn, SymbolId symbol) const {
        if (symbol < 0 || symbol >= static_cast<SymbolId>(fn.symbols.size())) {
            fail("S6 statement references a symbol outside its function");
        }
    }

    void verifyLValueSymbol(const FunctionCFG& fn, const LValue& value) const {
        if (value.root_symbol >= 0) verifySymbol(fn, value.root_symbol);
        for (const auto& access : value.accesses) {
            if (access.index) verifyOperandSymbol(fn, *access.index);
        }
    }

    void verifyOperandSymbol(const FunctionCFG& fn, const Operand& operand) const {
        if (operand.kind == OperandKind::Var) verifySymbol(fn, operand.var_symbol);
        if (operand.kind == OperandKind::LValueRead) verifyLValueSymbol(fn, operand.lvalue);
    }

    void verifyStmtSymbols(const FunctionCFG& fn, const S3StmtPtr& stmt) const {
        if (!stmt) return;
        switch (stmt->kind) {
        case S3StmtKind::Decl:
            verifySymbol(fn, stmt->decl_symbol);
            return;
        case S3StmtKind::Assign:
            verifyLValueSymbol(fn, stmt->target);
            verifyOperandSymbol(fn, stmt->value);
            return;
        case S3StmtKind::Op:
            verifyLValueSymbol(fn, stmt->target);
            for (const auto& operand : stmt->op.operands) verifyOperandSymbol(fn, operand);
            return;
        case S3StmtKind::Construct:
            verifyLValueSymbol(fn, stmt->target);
            for (const auto& arg : stmt->args) verifyOperandSymbol(fn, arg);
            return;
        case S3StmtKind::Eval:
            verifyOperandSymbol(fn, stmt->value);
            return;
        case S3StmtKind::Call:
            if (stmt->call_result) verifyLValueSymbol(fn, stmt->call_result.value());
            for (const auto& arg : stmt->args) verifyOperandSymbol(fn, arg);
            return;
        default:
            fail("S6 output contains a non-sequential statement");
        }
    }

    InlinedFunction convertFunction(const FunctionCFG& fn) const {
        InlinedFunction out;
        out.name = fn.name;
        out.return_type = fn.return_type;
        out.params = fn.params;
        out.symbols = fn.symbols;
        out.entry = fn.entry;
        out.exit = fn.exit;
        out.return_slot = fn.return_slot;
        out.return_slot_symbol = fn.return_slot_symbol;
        for (const auto& block : fn.blocks) {
            auto converted = std::make_unique<InlinedBasicBlock>();
            converted->id = block->id;
            converted->stmts = block->stmts;
            converted->terminator = block->terminator;
            converted->successors = block->successors;
            converted->predecessors = block->predecessors;
            out.blocks.push_back(std::move(converted));
        }
        return out;
    }

    void verifyInlinedFunction(const InlinedFunction& fn) const {
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            if (fn.blocks[i]->id != static_cast<BlockId>(i)) {
                fail("S6 inlined block id mismatch");
            }
            for (const auto& stmt : fn.blocks[i]->stmts) {
                if (stmt.stmt && stmt.stmt->kind == S3StmtKind::Call) {
                    fail("S6 inlined output still contains a call");
                }
            }
        }
    }
};

std::string unaryName(UnaryOp op) {
    switch (op) {
    case UnaryOp::LogicalNot: return "LogicalNot";
    case UnaryOp::BitNot: return "BitNot";
    case UnaryOp::Negate: return "Negate";
    case UnaryOp::Plus: return "Plus";
    }
    return "Unary";
}

std::string binaryName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "Add";
    case BinaryOp::Sub: return "Sub";
    case BinaryOp::Mul: return "Mul";
    case BinaryOp::Div: return "Div";
    case BinaryOp::Mod: return "Mod";
    case BinaryOp::Shl: return "Shl";
    case BinaryOp::Shr: return "Shr";
    case BinaryOp::BitAnd: return "BitAnd";
    case BinaryOp::BitOr: return "BitOr";
    case BinaryOp::BitXor: return "BitXor";
    case BinaryOp::LogicalAnd: return "LogicalAnd";
    case BinaryOp::LogicalOr: return "LogicalOr";
    case BinaryOp::Eq: return "Eq";
    case BinaryOp::Ne: return "Ne";
    case BinaryOp::Lt: return "Lt";
    case BinaryOp::Le: return "Le";
    case BinaryOp::Gt: return "Gt";
    case BinaryOp::Ge: return "Ge";
    }
    return "Binary";
}

std::string hardwareName(HardwareOp op) {
    switch (op) {
    case HardwareOp::ZExt: return "ZExt";
    case HardwareOp::SExt: return "SExt";
    case HardwareOp::Trunc: return "Trunc";
    case HardwareOp::Slice: return "Slice";
    case HardwareOp::BitSelect: return "BitSelect";
    case HardwareOp::DynamicSlice: return "DynamicSlice";
    case HardwareOp::DynamicBitSelect: return "DynamicBitSelect";
    case HardwareOp::WriteSlice: return "WriteSlice";
    case HardwareOp::WriteBit: return "WriteBit";
    case HardwareOp::DynamicWriteSlice: return "DynamicWriteSlice";
    case HardwareOp::DynamicWriteBit: return "DynamicWriteBit";
    case HardwareOp::Concat: return "Concat";
    case HardwareOp::Repeat: return "Repeat";
    case HardwareOp::ReduceOr: return "ReduceOr";
    case HardwareOp::ReduceAnd: return "ReduceAnd";
    case HardwareOp::ReduceXor: return "ReduceXor";
    }
    return "Hardware";
}

std::string operandText(const Operand& operand);

std::string lvalueText(const LValue& lv) {
    std::string out = lv.root;
    for (const auto& access : lv.accesses) {
        if (access.kind == LValueAccessKind::Field) {
            out += "." + access.field;
        } else {
            out += "[";
            out += access.index ? operandText(*access.index) : "<null>";
            out += "]";
        }
    }
    return out;
}

std::string operandText(const Operand& operand) {
    switch (operand.kind) {
    case OperandKind::Literal: return operand.literal_value;
    case OperandKind::Var: return operand.var_name;
    case OperandKind::LValueRead: return lvalueText(operand.lvalue);
    }
    return "<operand>";
}

std::string opText(const OpExpr& op) {
    std::ostringstream os;
    if (op.kind == OpExpr::Kind::Unary) os << unaryName(op.unary_op);
    else if (op.kind == OpExpr::Kind::Binary) os << binaryName(op.binary_op);
    else if (op.kind == OpExpr::Kind::Ternary) os << "Ternary";
    else if (op.kind == OpExpr::Kind::Cast) os << "Cast";
    else os << hardwareName(op.hardware_op);
    os << "(";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        if (i) os << ", ";
        os << operandText(op.operands[i]);
    }
    os << ")";
    return os.str();
}

std::string stmtText(const CFGStmt& stmt) {
    if (!stmt.stmt) return "<null>";
    const auto& s = *stmt.stmt;
    switch (s.kind) {
    case S3StmtKind::Decl:
        return "decl " + s.decl_name;
    case S3StmtKind::Assign:
        return "assign " + lvalueText(s.target) + " = " + operandText(s.value);
    case S3StmtKind::Op:
        return "op " + lvalueText(s.target) + " = " + opText(s.op);
    case S3StmtKind::Construct: {
        std::ostringstream os;
        os << "construct " << lvalueText(s.target) << " = " << s.callee << "(";
        for (std::size_t i = 0; i < s.args.size(); ++i) {
            if (i) os << ", ";
            os << operandText(s.args[i]);
        }
        os << ")";
        return os.str();
    }
    case S3StmtKind::Eval:
        return "eval " + operandText(s.value);
    case S3StmtKind::Call:
        return "call " + s.callee;
    default:
        return "<control>";
    }
}

std::string termKindName(TermKind kind) {
    switch (kind) {
    case TermKind::Jump: return "jump";
    case TermKind::Branch: return "branch";
    case TermKind::Switch: return "switch";
    case TermKind::Return: return "return";
    case TermKind::Unreachable: return "unreachable";
    case TermKind::Exit: return "exit";
    }
    return "term";
}

std::string pad(int indent) {
    return std::string(static_cast<std::size_t>(indent), ' ');
}

void printBlock(std::ostream& os, const InlinedBasicBlock& block, int indent) {
    os << pad(indent) << "bb" << block.id << " preds=[";
    for (std::size_t i = 0; i < block.predecessors.size(); ++i) {
        if (i) os << ", ";
        os << block.predecessors[i].label << ":bb" << block.predecessors[i].from;
    }
    os << "] succs=[";
    for (std::size_t i = 0; i < block.successors.size(); ++i) {
        if (i) os << ", ";
        os << block.successors[i].label << ":bb" << block.successors[i].to;
    }
    os << "]\n";
    for (const auto& stmt : block.stmts) {
        os << pad(indent + 2) << stmtText(stmt) << "\n";
    }
    os << pad(indent + 2) << "term " << termKindName(block.terminator.kind);
    switch (block.terminator.kind) {
    case TermKind::Jump:
        os << " bb" << block.terminator.jump_target;
        break;
    case TermKind::Branch:
        os << " " << operandText(block.terminator.condition)
           << " ? bb" << block.terminator.true_target
           << " : bb" << block.terminator.false_target;
        break;
    case TermKind::Switch:
        os << " " << operandText(block.terminator.switch_value)
           << " default -> bb" << block.terminator.default_target;
        break;
    case TermKind::Return:
        if (block.terminator.return_value) {
            os << " " << operandText(block.terminator.return_value.value());
        }
        break;
    case TermKind::Unreachable:
    case TermKind::Exit:
        break;
    }
    os << "\n";
}

} // namespace

std::string debugPrint(const InlinedCFGProgram& program,
                       const std::vector<InlineSummary>& summaries) {
    std::ostringstream os;
    os << "s6inline\n";
    for (const auto& summary : summaries) {
        os << "inlined caller=" << summary.caller
           << " callee=" << summary.callee
           << " call_block=bb" << summary.call_block
           << " cloned_blocks=" << summary.cloned_blocks << "\n";
    }
    const auto& fn = program.top;
    os << "top " << fn.name << " entry=bb" << fn.entry << " exit=bb" << fn.exit;
    if (fn.return_slot) os << " return_slot=" << fn.return_slot.value();
    os << "\n";
    for (const auto& block : fn.blocks) printBlock(os, *block, 2);
    return os.str();
}

InlineResult inlineCFGProgram(const CFGProgram& program,
                              const InlineOptions& options) {
    try {
        InlineResult result;
        InlineEngine engine(program, options, result.warnings, result.summaries);
        result.program = engine.run();
        if (options.debug_print) {
            result.debug_text = debugPrint(result.program.value(), result.summaries);
        }
        return result;
    } catch (const RTLZZException& ex) {
        InlineResult result;
        InlineError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        InlineResult result;
        InlineError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

InlinedCFGProgram inlineCFGProgramOrThrow(const CFGProgram& program,
                                          const InlineOptions& options) {
    auto result = inlineCFGProgram(program, options);
    if (!result.ok()) {
        throw RTLZZException(result.error->context, result.error->message);
    }
    return std::move(result.program.value());
}

} // namespace pred::s6inline
