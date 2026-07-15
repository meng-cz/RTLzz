#include "s4cfg/S4CFG.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s4cfg {
namespace {

using namespace pred::s3statementize;

std::string sanitizeName(std::string name) {
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    if (name.empty()) name = "fn";
    return name;
}

bool isVoidType(const TypeInfo& type) {
    return type.name == "void" && type.struct_name.empty() && !type.is_array;
}

ErrorContext makeContext(DebugLoc loc, std::string note = {}) {
    ErrorContext context;
    context.stage = "s4cfg";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(DebugLoc loc, const std::string& message) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

CFGStmtKind cfgStmtKind(S3StmtKind kind, DebugLoc loc) {
    switch (kind) {
    case S3StmtKind::Decl: return CFGStmtKind::Decl;
    case S3StmtKind::Assign: return CFGStmtKind::Assign;
    case S3StmtKind::Op: return CFGStmtKind::Op;
    case S3StmtKind::Call: return CFGStmtKind::Call;
    case S3StmtKind::Construct: return CFGStmtKind::Construct;
    case S3StmtKind::Eval: return CFGStmtKind::Eval;
    case S3StmtKind::If:
    case S3StmtKind::For:
    case S3StmtKind::While:
    case S3StmtKind::DoWhile:
    case S3StmtKind::Switch:
    case S3StmtKind::Break:
    case S3StmtKind::Continue:
    case S3StmtKind::Return:
        fail(std::move(loc), "Control-flow statement cannot be stored inside a CFG basic block");
    }
    fail(std::move(loc), "Unknown S3 statement kind");
}

bool isSequentialStmt(S3StmtKind kind) {
    switch (kind) {
    case S3StmtKind::Decl:
    case S3StmtKind::Assign:
    case S3StmtKind::Op:
    case S3StmtKind::Call:
    case S3StmtKind::Construct:
    case S3StmtKind::Eval:
        return true;
    default:
        return false;
    }
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

std::string scopeKindName(ScopeKind kind) {
    switch (kind) {
    case ScopeKind::Function: return "function";
    case ScopeKind::Block: return "block";
    case ScopeKind::IfThen: return "if_then";
    case ScopeKind::IfElse: return "if_else";
    case ScopeKind::LoopBody: return "loop_body";
    case ScopeKind::SwitchCase: return "switch_case";
    }
    return "scope";
}

std::string loopConditionKindName(LoopConditionKind kind) {
    switch (kind) {
    case LoopConditionKind::PreTest: return "pre_test";
    case LoopConditionKind::PostTest: return "post_test";
    }
    return "loop";
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

std::string operandText(const Operand& op);

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

std::string operandText(const Operand& op) {
    switch (op.kind) {
    case OperandKind::Literal: return op.literal_value;
    case OperandKind::Var: return op.var_name;
    case OperandKind::LValueRead: return lvalueText(op.lvalue);
    }
    return "<operand>";
}

std::string opText(const OpExpr& op) {
    std::ostringstream os;
    if (op.kind == OpExpr::Kind::Unary) os << unaryName(op.unary_op);
    else if (op.kind == OpExpr::Kind::Binary) os << binaryName(op.binary_op);
    else if (op.kind == OpExpr::Kind::Ternary) os << "Ternary";
    else if (op.kind == OpExpr::Kind::Cast) os << "Cast";
    else os << "Hardware";
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
    case S3StmtKind::Call: {
        std::ostringstream os;
        os << "call ";
        if (s.call_result) os << lvalueText(s.call_result.value()) << " = ";
        os << s.callee << "(";
        for (std::size_t i = 0; i < s.args.size(); ++i) {
            if (i) os << ", ";
            os << operandText(s.args[i]);
        }
        os << ")";
        return os.str();
    }
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
    default:
        return "<control>";
    }
}

std::string edgeLabel(EdgeKind kind, const std::string& label = {}) {
    if (!label.empty()) return label;
    return edgeKindName(kind);
}

class CFGBuilder {
public:
    CFGBuilder(const StatementizedFunction& fn, std::vector<CFGWarning>& warnings)
        : fn_(fn), warnings_(warnings) {}

    FunctionCFG build() {
        cfg_.name = fn_.name;
        cfg_.return_type = fn_.return_type;
        cfg_.params = fn_.params;
        cfg_.s3_scopes = fn_.scopes;
        cfg_.symbols = fn_.symbols;
        createScope(std::nullopt, ScopeKind::Function, fn_.name);

        auto* entry = newBlock();
        auto* body = newBlock();
        auto* exit = newBlock();
        cfg_.entry = entry->id;
        cfg_.exit = exit->id;
        exit->terminator.kind = TermKind::Exit;

        setCurrent(entry);
        terminateJump(body->id, EdgeKind::Fallthrough, "entry");
        setCurrent(body);
        buildStmtList(fn_.body);

        if (current_) {
            if (!isVoidType(fn_.return_type)) {
                fail(DebugLoc{}, "Non-void function '" + fn_.name +
                                     "' may fall through without return");
            }
            terminateReturn(std::nullopt);
        }

        lowerFunctionExits(cfg_, warnings_);
        verifyReachabilityWarnings();
        verifyFunctionCFG(cfg_);
        return std::move(cfg_);
    }

private:
    const StatementizedFunction& fn_;
    std::vector<CFGWarning>& warnings_;
    FunctionCFG cfg_;
    BasicBlock* current_ = nullptr;
    std::vector<ScopeId> scope_stack_{0};
    std::vector<LoopRegionId> loop_stack_;
    std::vector<BlockId> break_targets_;
    std::vector<BlockId> continue_targets_;

    ScopeId createScope(std::optional<ScopeId> parent, ScopeKind kind, std::string label) {
        ScopeInfo scope;
        scope.id = static_cast<ScopeId>(cfg_.scopes.size());
        scope.parent = parent;
        scope.kind = kind;
        scope.label = std::move(label);
        cfg_.scopes.push_back(std::move(scope));
        return cfg_.scopes.back().id;
    }

    ScopeId childScope(ScopeKind kind, const std::string& label) {
        ScopeId parent = scope_stack_.empty() ? 0 : scope_stack_.back();
        return createScope(parent, kind, label);
    }

    struct ScopeGuard {
        CFGBuilder& builder;
        bool active = true;
        ScopeGuard(CFGBuilder& builder, ScopeId scope) : builder(builder) {
            builder.scope_stack_.push_back(scope);
        }
        ~ScopeGuard() {
            if (active) builder.scope_stack_.pop_back();
        }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
    };

    BasicBlock* newBlock(std::vector<ScopeId> scopes = {},
                         std::vector<LoopRegionId> loops = {}) {
        auto block = std::make_unique<BasicBlock>();
        block->id = static_cast<BlockId>(cfg_.blocks.size());
        block->scope_stack = scopes.empty() ? scope_stack_ : std::move(scopes);
        block->loop_stack = loops.empty() ? loop_stack_ : std::move(loops);
        auto* ptr = block.get();
        cfg_.blocks.push_back(std::move(block));
        return ptr;
    }

    BasicBlock* block(BlockId id) {
        if (id < 0 || id >= static_cast<BlockId>(cfg_.blocks.size())) return nullptr;
        return cfg_.blocks[static_cast<std::size_t>(id)].get();
    }

    void setCurrent(BasicBlock* block) {
        current_ = block;
    }

    void addEdge(BlockId from, BlockId to, EdgeKind kind, std::string label,
                 std::optional<Operand> case_value = std::nullopt) {
        auto* src = block(from);
        auto* dst = block(to);
        if (!src || !dst) {
            fail(DebugLoc{}, "CFG edge points to invalid block");
        }
        CFGEdge edge;
        edge.from = from;
        edge.to = to;
        edge.kind = kind;
        edge.label = edgeLabel(kind, std::move(label));
        edge.case_value = std::move(case_value);
        src->successors.push_back(edge);
        dst->predecessors.push_back(std::move(edge));
    }

    void emit(const S3StmtPtr& stmt) {
        if (!current_) return;
        current_->stmts.push_back(CFGStmt{cfgStmtKind(stmt->kind, stmt->debug_loc), stmt});
    }

    void emitList(const std::vector<S3StmtPtr>& stmts) {
        for (const auto& stmt : stmts) {
            if (!stmt) continue;
            if (!isSequentialStmt(stmt->kind)) {
                fail(stmt->debug_loc, "Condition/step prelude contains a control-flow statement");
            }
            emit(stmt);
        }
    }

    void terminateJump(BlockId target, EdgeKind edge_kind, std::string label = {}) {
        if (!current_) return;
        current_->terminator.kind = TermKind::Jump;
        current_->terminator.jump_target = target;
        addEdge(current_->id, target, edge_kind, std::move(label));
        current_ = nullptr;
    }

    void terminateBranch(Operand condition, BlockId true_target, BlockId false_target) {
        if (!current_) return;
        current_->terminator.kind = TermKind::Branch;
        current_->terminator.condition = std::move(condition);
        current_->terminator.true_target = true_target;
        current_->terminator.false_target = false_target;
        addEdge(current_->id, true_target, EdgeKind::True, "true");
        addEdge(current_->id, false_target, EdgeKind::False, "false");
        current_ = nullptr;
    }

    void terminateSwitch(Operand selector,
                         std::vector<SwitchTarget> targets,
                         BlockId default_target) {
        if (!current_) return;
        current_->terminator.kind = TermKind::Switch;
        current_->terminator.switch_value = std::move(selector);
        current_->terminator.switch_targets = std::move(targets);
        current_->terminator.default_target = default_target;
        for (const auto& target : current_->terminator.switch_targets) {
            std::string label = target.value ? "case " + operandText(target.value.value()) : "case";
            addEdge(current_->id, target.target, EdgeKind::Case, std::move(label), target.value);
        }
        addEdge(current_->id, default_target, EdgeKind::Default, "default");
        current_ = nullptr;
    }

    void terminateReturn(std::optional<Operand> value) {
        if (!current_) return;
        current_->terminator.kind = TermKind::Return;
        current_->terminator.return_value = std::move(value);
        addEdge(current_->id, cfg_.exit, EdgeKind::Return, "return");
        current_ = nullptr;
    }

    void warnUnreachable(const S3StmtPtr& stmt) {
        CFGWarning warning;
        warning.context = makeContext(stmt ? stmt->debug_loc : DebugLoc{}, fn_.name);
        warning.message = "Ignoring unreachable statement after a terminating control-flow statement";
        warnings_.push_back(std::move(warning));
    }

    void buildStmtList(const std::vector<S3StmtPtr>& stmts) {
        for (const auto& stmt : stmts) {
            if (!current_) {
                warnUnreachable(stmt);
                continue;
            }
            buildStmt(stmt);
        }
    }

    void buildStmt(const S3StmtPtr& stmt) {
        if (!stmt) return;
        ErrorContextGuard guard("s4cfg", stmt->debug_loc, "building CFG statement");
        switch (stmt->kind) {
        case S3StmtKind::Decl:
        case S3StmtKind::Assign:
        case S3StmtKind::Op:
        case S3StmtKind::Call:
        case S3StmtKind::Construct:
        case S3StmtKind::Eval:
            emit(stmt);
            return;
        case S3StmtKind::If:
            buildIf(stmt);
            return;
        case S3StmtKind::For:
            buildFor(stmt);
            return;
        case S3StmtKind::While:
            buildWhile(stmt);
            return;
        case S3StmtKind::DoWhile:
            buildDoWhile(stmt);
            return;
        case S3StmtKind::Switch:
            buildSwitch(stmt);
            return;
        case S3StmtKind::Break:
            if (break_targets_.empty()) {
                fail(stmt->debug_loc, "break statement is not inside a loop or switch");
            }
            terminateJump(break_targets_.back(), EdgeKind::Break, "break");
            return;
        case S3StmtKind::Continue:
            if (continue_targets_.empty()) {
                fail(stmt->debug_loc, "continue statement is not inside a loop");
            }
            terminateJump(continue_targets_.back(), EdgeKind::Continue, "continue");
            return;
        case S3StmtKind::Return:
            if (!isVoidType(fn_.return_type) && !stmt->return_value) {
                fail(stmt->debug_loc, "Non-void function '" + fn_.name +
                                          "' returns without a value");
            }
            terminateReturn(stmt->return_value);
            return;
        }
    }

    void buildIf(const S3StmtPtr& stmt) {
        auto* then_block = newBlock();
        auto* else_block = newBlock();
        auto* merge_block = newBlock();
        terminateBranch(stmt->condition, then_block->id, else_block->id);

        {
            ScopeGuard guard(*this, childScope(ScopeKind::IfThen, "then"));
            then_block->scope_stack = scope_stack_;
            setCurrent(then_block);
            buildStmtList(stmt->then_body);
            if (current_) terminateJump(merge_block->id, EdgeKind::Fallthrough, "then_merge");
        }

        {
            ScopeGuard guard(*this, childScope(ScopeKind::IfElse, "else"));
            else_block->scope_stack = scope_stack_;
            setCurrent(else_block);
            buildStmtList(stmt->else_body);
            if (current_) terminateJump(merge_block->id, EdgeKind::Fallthrough, "else_merge");
        }

        setCurrent(merge_block->predecessors.empty() ? nullptr : merge_block);
    }

    LoopRegionId createLoopRegion(LoopConditionKind kind,
                                  BlockId init,
                                  BlockId condition,
                                  BlockId condition_prelude,
                                  BlockId body,
                                  BlockId exit) {
        LoopRegion region;
        region.id = static_cast<LoopRegionId>(cfg_.loop_regions.size());
        region.condition_kind = kind;
        region.init = init;
        region.condition = condition;
        region.condition_prelude = condition_prelude;
        region.body = body;
        region.exit = exit;
        cfg_.loop_regions.push_back(region);
        return region.id;
    }

    struct LoopGuard {
        CFGBuilder& builder;
        LoopGuard(CFGBuilder& builder, LoopRegionId id) : builder(builder) {
            builder.loop_stack_.push_back(id);
        }
        ~LoopGuard() { builder.loop_stack_.pop_back(); }
        LoopGuard(const LoopGuard&) = delete;
        LoopGuard& operator=(const LoopGuard&) = delete;
    };

    void buildFor(const S3StmtPtr& stmt) {
        BlockId init = current_ ? current_->id : -1;
        emitList(stmt->for_init);
        auto* condition_prelude = newBlock();
        auto* condition = newBlock();
        auto* body = newBlock();
        auto* step = newBlock();
        auto* exit = newBlock();
        LoopRegionId loop_id = createLoopRegion(LoopConditionKind::PreTest,
                                                init, condition->id, condition_prelude->id,
                                                body->id, exit->id);
        condition_prelude->loop_stack.push_back(loop_id);
        condition->loop_stack.push_back(loop_id);
        body->loop_stack.push_back(loop_id);
        step->loop_stack.push_back(loop_id);

        if (current_) {
            terminateJump(condition_prelude->id, EdgeKind::Fallthrough,
                          "loop_condition_prelude");
        }

        setCurrent(condition_prelude);
        emitList(stmt->condition_prelude);
        terminateJump(condition->id, EdgeKind::Fallthrough, "condition");

        setCurrent(condition);
        if (stmt->for_cond) terminateBranch(stmt->for_cond.value(), body->id, exit->id);
        else terminateJump(body->id, EdgeKind::Jump, "true");

        break_targets_.push_back(exit->id);
        continue_targets_.push_back(step->id);
        {
            LoopGuard loop_guard(*this, loop_id);
            ScopeGuard scope_guard(*this, childScope(ScopeKind::LoopBody, "for_body"));
            body->scope_stack = scope_stack_;
            setCurrent(body);
            buildStmtList(stmt->loop_body);
            if (current_) terminateJump(step->id, EdgeKind::Fallthrough, "body_next");
        }

        {
            LoopGuard loop_guard(*this, loop_id);
            ScopeGuard scope_guard(*this, childScope(ScopeKind::LoopBody, "for_body"));
            step->scope_stack = scope_stack_;
            setCurrent(step);
            emitList(stmt->for_step);
            terminateJump(condition_prelude->id, EdgeKind::Jump, "backedge");
        }
        break_targets_.pop_back();
        continue_targets_.pop_back();
        setCurrent(exit);
    }

    void buildWhile(const S3StmtPtr& stmt) {
        auto* condition_prelude = newBlock();
        auto* condition = newBlock();
        auto* body = newBlock();
        auto* exit = newBlock();
        LoopRegionId loop_id = createLoopRegion(LoopConditionKind::PreTest,
                                                -1, condition->id, condition_prelude->id,
                                                body->id, exit->id);
        condition_prelude->loop_stack.push_back(loop_id);
        condition->loop_stack.push_back(loop_id);
        body->loop_stack.push_back(loop_id);

        if (current_) {
            terminateJump(condition_prelude->id, EdgeKind::Fallthrough,
                          "loop_condition_prelude");
        }

        setCurrent(condition_prelude);
        emitList(stmt->condition_prelude);
        terminateJump(condition->id, EdgeKind::Fallthrough, "condition");

        setCurrent(condition);
        terminateBranch(stmt->condition, body->id, exit->id);

        break_targets_.push_back(exit->id);
        continue_targets_.push_back(condition_prelude->id);
        {
            LoopGuard loop_guard(*this, loop_id);
            ScopeGuard scope_guard(*this, childScope(ScopeKind::LoopBody, "while_body"));
            body->scope_stack = scope_stack_;
            setCurrent(body);
            buildStmtList(stmt->loop_body);
            if (current_) terminateJump(condition_prelude->id, EdgeKind::Jump, "backedge");
        }
        break_targets_.pop_back();
        continue_targets_.pop_back();
        setCurrent(exit);
    }

    void buildDoWhile(const S3StmtPtr& stmt) {
        auto* body = newBlock();
        auto* condition_prelude = newBlock();
        auto* condition = newBlock();
        auto* exit = newBlock();
        LoopRegionId loop_id = createLoopRegion(LoopConditionKind::PostTest,
                                                -1, condition->id, condition_prelude->id,
                                                body->id, exit->id);
        body->loop_stack.push_back(loop_id);
        condition_prelude->loop_stack.push_back(loop_id);
        condition->loop_stack.push_back(loop_id);

        if (current_) terminateJump(body->id, EdgeKind::Fallthrough, "loop_body");

        break_targets_.push_back(exit->id);
        continue_targets_.push_back(condition_prelude->id);
        {
            LoopGuard loop_guard(*this, loop_id);
            ScopeGuard scope_guard(*this, childScope(ScopeKind::LoopBody, "do_body"));
            body->scope_stack = scope_stack_;
            setCurrent(body);
            buildStmtList(stmt->loop_body);
            if (current_) {
                terminateJump(condition_prelude->id, EdgeKind::Fallthrough,
                              "body_condition");
            }
        }
        break_targets_.pop_back();
        continue_targets_.pop_back();

        setCurrent(condition_prelude);
        emitList(stmt->condition_prelude);
        terminateJump(condition->id, EdgeKind::Fallthrough, "condition");

        setCurrent(condition);
        terminateBranch(stmt->condition, body->id, exit->id);
        setCurrent(exit);
    }

    void buildSwitch(const S3StmtPtr& stmt) {
        auto* exit = newBlock();
        std::vector<BasicBlock*> case_blocks;
        case_blocks.reserve(stmt->switch_cases.size());
        BlockId default_target = exit->id;
        for (std::size_t i = 0; i < stmt->switch_cases.size(); ++i) {
            auto* bb = newBlock();
            case_blocks.push_back(bb);
            if (!stmt->switch_cases[i].value && default_target == exit->id) {
                default_target = bb->id;
            }
        }

        std::vector<SwitchTarget> targets;
        for (std::size_t i = 0; i < stmt->switch_cases.size(); ++i) {
            if (stmt->switch_cases[i].value) {
                targets.push_back(SwitchTarget{stmt->switch_cases[i].value, case_blocks[i]->id});
            }
        }
        terminateSwitch(stmt->switch_value, std::move(targets), default_target);

        break_targets_.push_back(exit->id);
        for (std::size_t i = 0; i < stmt->switch_cases.size(); ++i) {
            ScopeGuard scope_guard(*this, childScope(ScopeKind::SwitchCase, "case"));
            case_blocks[i]->scope_stack = scope_stack_;
            setCurrent(case_blocks[i]);
            buildStmtList(stmt->switch_cases[i].body);
            if (current_) {
                BlockId target = i + 1 < case_blocks.size()
                    ? case_blocks[i + 1]->id
                    : exit->id;
                terminateJump(target, EdgeKind::Fallthrough, "fallthrough");
            }
        }
        break_targets_.pop_back();
        setCurrent(exit);
    }

    std::set<BlockId> reachableBlocks() const {
        std::set<BlockId> reachable;
        if (cfg_.entry < 0) return reachable;
        std::deque<BlockId> work;
        work.push_back(cfg_.entry);
        reachable.insert(cfg_.entry);
        while (!work.empty()) {
            BlockId id = work.front();
            work.pop_front();
            const auto& block = *cfg_.blocks[static_cast<std::size_t>(id)];
            for (const auto& edge : block.successors) {
                if (reachable.insert(edge.to).second) work.push_back(edge.to);
            }
        }
        return reachable;
    }

    void verifyReachabilityWarnings() {
        auto reachable = reachableBlocks();
        for (const auto& block : cfg_.blocks) {
            if (!reachable.count(block->id)) {
                CFGWarning warning;
                warning.context = makeContext(DebugLoc{}, cfg_.name);
                warning.message = "CFG contains unreachable block bb" +
                                  std::to_string(block->id);
                warnings_.push_back(std::move(warning));
            }
        }
    }

    void verifyFunctionCFG(const FunctionCFG& cfg) {
        if (cfg.entry < 0 || cfg.exit < 0) fail(DebugLoc{}, "CFG is missing entry or exit block");
        if (cfg.exit >= static_cast<BlockId>(cfg.blocks.size()) ||
            cfg.entry >= static_cast<BlockId>(cfg.blocks.size())) {
            fail(DebugLoc{}, "CFG entry or exit block id is invalid");
        }
        for (std::size_t i = 0; i < cfg.blocks.size(); ++i) {
            const auto& block = *cfg.blocks[i];
            if (block.id != static_cast<BlockId>(i)) {
                fail(DebugLoc{}, "CFG block id does not match vector index");
            }
            for (const auto& stmt : block.stmts) {
                if (!stmt.stmt || !isSequentialStmt(stmt.stmt->kind)) {
                    fail(stmt.stmt ? stmt.stmt->debug_loc : DebugLoc{},
                         "CFG block contains non-sequential statement");
                }
            }
            if (block.id == cfg.exit) {
                if (block.terminator.kind != TermKind::Exit) {
                    fail(DebugLoc{}, "CFG exit block must have Exit terminator");
                }
                if (!block.successors.empty()) {
                    fail(DebugLoc{}, "CFG exit block must not have successors");
                }
                continue;
            }
            if (block.terminator.kind == TermKind::Exit) {
                fail(DebugLoc{}, "Only CFG exit block may use Exit terminator");
            }
            verifyTerminatorTargets(cfg, block);
            verifyEdgeLists(cfg, block);
        }
    }

    void verifyTarget(const FunctionCFG& cfg, BlockId target) {
        if (target < 0 || target >= static_cast<BlockId>(cfg.blocks.size())) {
            fail(DebugLoc{}, "CFG terminator target is invalid");
        }
    }

    void verifyTerminatorTargets(const FunctionCFG& cfg, const BasicBlock& block) {
        std::vector<BlockId> expected;
        switch (block.terminator.kind) {
        case TermKind::Jump:
            verifyTarget(cfg, block.terminator.jump_target);
            expected.push_back(block.terminator.jump_target);
            verifySuccessorTargets(block, expected);
            return;
        case TermKind::Branch:
            verifyTarget(cfg, block.terminator.true_target);
            verifyTarget(cfg, block.terminator.false_target);
            expected.push_back(block.terminator.true_target);
            expected.push_back(block.terminator.false_target);
            verifySuccessorTargets(block, expected);
            return;
        case TermKind::Switch:
            for (const auto& target : block.terminator.switch_targets) {
                verifyTarget(cfg, target.target);
                expected.push_back(target.target);
            }
            verifyTarget(cfg, block.terminator.default_target);
            expected.push_back(block.terminator.default_target);
            verifySuccessorTargets(block, expected);
            return;
        case TermKind::Return:
            verifySuccessorTargets(block, {cfg.exit});
            return;
        case TermKind::Unreachable:
            verifySuccessorTargets(block, {});
            return;
        case TermKind::Exit:
            fail(DebugLoc{}, "Unexpected Exit terminator outside exit block");
        }
    }

    void verifySuccessorTargets(const BasicBlock& block,
                                const std::vector<BlockId>& expected) {
        std::vector<BlockId> actual;
        actual.reserve(block.successors.size());
        for (const auto& edge : block.successors) actual.push_back(edge.to);
        auto sorted_actual = actual;
        auto sorted_expected = expected;
        std::sort(sorted_actual.begin(), sorted_actual.end());
        std::sort(sorted_expected.begin(), sorted_expected.end());
        if (sorted_actual != sorted_expected) {
            fail(DebugLoc{}, "CFG successor list does not match terminator targets");
        }
    }

    void verifyEdgeLists(const FunctionCFG& cfg, const BasicBlock& block) {
        for (const auto& edge : block.successors) {
            verifyTarget(cfg, edge.to);
            const auto& target = *cfg.blocks[static_cast<std::size_t>(edge.to)];
            auto found = std::any_of(target.predecessors.begin(), target.predecessors.end(),
                                     [&](const CFGEdge& pred) {
                                         return pred.from == block.id &&
                                                pred.to == edge.to &&
                                                pred.label == edge.label;
                                     });
            if (!found) {
                fail(DebugLoc{}, "CFG successor edge is missing matching predecessor edge");
            }
        }
        for (const auto& edge : block.predecessors) {
            verifyTarget(cfg, edge.from);
            const auto& source = *cfg.blocks[static_cast<std::size_t>(edge.from)];
            auto found = std::any_of(source.successors.begin(), source.successors.end(),
                                     [&](const CFGEdge& succ) {
                                         return succ.from == edge.from &&
                                                succ.to == block.id &&
                                                succ.label == edge.label;
                                     });
            if (!found) {
                fail(DebugLoc{}, "CFG predecessor edge is missing matching successor edge");
            }
        }
    }
};

FunctionCFG buildOneFunction(const StatementizedFunction& fn,
                             std::vector<CFGWarning>& warnings) {
    CFGBuilder builder(fn, warnings);
    return builder.build();
}

CFGProgram runBuildCFGProgram(const StatementizedProgram& input,
                              std::vector<CFGWarning>& warnings) {
    CFGProgram out;
    out.struct_fields = input.struct_fields;
    out.struct_constructors = input.struct_constructors;
    out.top = buildOneFunction(input.top, warnings);
    for (const auto& helper : input.helpers) {
        out.helper_index[helper.name] = out.helpers.size();
        out.helpers.push_back(buildOneFunction(helper, warnings));
    }
    for (const auto& [name, lambda] : input.lambdas) {
        out.lambda_index[name] = name;
        out.lambdas.emplace(name, buildOneFunction(lambda, warnings));
    }
    return out;
}

std::string pad(int indent) {
    return std::string(static_cast<std::size_t>(indent), ' ');
}

void printBlock(std::ostream& os, const BasicBlock& block, int indent) {
    os << pad(indent) << "bb" << block.id << " scopes=[";
    for (std::size_t i = 0; i < block.scope_stack.size(); ++i) {
        if (i) os << ",";
        os << block.scope_stack[i];
    }
    os << "] loops=[";
    for (std::size_t i = 0; i < block.loop_stack.size(); ++i) {
        if (i) os << ",";
        os << block.loop_stack[i];
    }
    os << "] preds=[";
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
        os << " " << operandText(block.terminator.switch_value);
        for (const auto& target : block.terminator.switch_targets) {
            os << " case ";
            if (target.value) os << operandText(target.value.value());
            else os << "<none>";
            os << " -> bb" << target.target;
        }
        os << " default -> bb" << block.terminator.default_target;
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

void printFunction(std::ostream& os, const FunctionCFG& fn, const std::string& kind) {
    os << kind << " " << fn.name << " entry=bb" << fn.entry
       << " exit=bb" << fn.exit;
    if (fn.return_slot) os << " return_slot=" << fn.return_slot.value();
    os << "\n";
    for (const auto& scope : fn.scopes) {
        os << "  scope " << scope.id << " " << scopeKindName(scope.kind);
        if (scope.parent) os << " parent=" << scope.parent.value();
        if (!scope.label.empty()) os << " label=" << scope.label;
        os << "\n";
    }
    for (const auto& loop : fn.loop_regions) {
        os << "  loop " << loop.id << " " << loopConditionKindName(loop.condition_kind)
           << " init=bb" << loop.init
           << " condition=bb" << loop.condition
           << " condition_prelude=bb" << loop.condition_prelude
           << " body=bb" << loop.body
           << " exit=bb" << loop.exit << "\n";
    }
    for (const auto& block : fn.blocks) printBlock(os, *block, 2);
}

} // namespace

std::string debugPrint(const CFGProgram& program) {
    std::ostringstream os;
    os << "s4cfg\n";
    printFunction(os, program.top, "top");
    for (const auto& helper : program.helpers) printFunction(os, helper, "helper");
    for (const auto& [name, lambda] : program.lambdas) {
        (void)name;
        printFunction(os, lambda, "lambda");
    }
    return os.str();
}

CFGResult buildCFGProgram(const StatementizedProgram& program,
                          const CFGOptions& options) {
    try {
        CFGResult result;
        result.program = runBuildCFGProgram(program, result.warnings);
        if (options.debug_print) result.debug_text = debugPrint(result.program.value());
        return result;
    } catch (const RTLZZException& ex) {
        CFGResult result;
        CFGError error;
        error.message = ex.message();
        error.formatted = ex.what();
        if (auto context = ex.primaryContext()) error.context = *context;
        result.error = std::move(error);
        return result;
    }
}

CFGProgram buildCFGProgramOrThrow(const StatementizedProgram& program,
                                  const CFGOptions& options) {
    std::vector<CFGWarning> warnings;
    auto out = runBuildCFGProgram(program, warnings);
    (void)options;
    return out;
}

} // namespace pred::s4cfg
