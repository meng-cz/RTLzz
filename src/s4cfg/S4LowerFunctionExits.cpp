#include "s4cfg/S4CFG.h"

#include <cctype>
#include <unordered_set>

namespace pred::s4cfg {
namespace {

using namespace pred::s3statementize;

bool isVoidType(const TypeInfo& type) {
    return type.name == "void" && type.struct_name.empty() && !type.is_array;
}

std::string sanitizeName(std::string name) {
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    if (name.empty()) name = "fn";
    return name;
}

void collectLValueName(const LValue& lv, std::unordered_set<std::string>& names) {
    if (!lv.root.empty()) names.insert(lv.root);
}

void collectOperandName(const Operand& op, std::unordered_set<std::string>& names) {
    if (op.kind == OperandKind::Var && !op.var_name.empty()) names.insert(op.var_name);
    if (op.kind == OperandKind::LValueRead) collectLValueName(op.lvalue, names);
}

std::unordered_set<std::string> collectNames(const FunctionCFG& cfg) {
    std::unordered_set<std::string> names;
    for (const auto& param : cfg.params) names.insert(param.name);
    for (const auto& symbol : cfg.symbols) names.insert(symbol.name);
    for (const auto& block : cfg.blocks) {
        for (const auto& stmt : block->stmts) {
            if (!stmt.stmt) continue;
            const auto& s = *stmt.stmt;
            if (s.kind == S3StmtKind::Decl) names.insert(s.decl_name);
            collectLValueName(s.target, names);
            collectOperandName(s.value, names);
            if (s.call_result) collectLValueName(s.call_result.value(), names);
            for (const auto& arg : s.args) collectOperandName(arg, names);
            for (const auto& arg : s.op.operands) collectOperandName(arg, names);
        }
        if (block->terminator.return_value) {
            collectOperandName(block->terminator.return_value.value(), names);
        }
    }
    return names;
}

std::string makeReturnSlotName(const FunctionCFG& cfg) {
    auto used = collectNames(cfg);
    std::string base = "__ret_" + sanitizeName(cfg.name) + "_";
    for (int i = 0;; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!used.count(candidate)) return candidate;
    }
}

LValue slotLValue(const std::string& name, SymbolId symbol, TypeInfo type) {
    LValue lv;
    lv.root = name;
    lv.root_symbol = symbol;
    lv.type = std::move(type);
    return lv;
}

Operand slotOperand(const std::string& name, SymbolId symbol, TypeInfo type) {
    Operand operand;
    operand.kind = OperandKind::Var;
    operand.var_name = name;
    operand.var_symbol = symbol;
    operand.type = std::move(type);
    return operand;
}

S3StmtPtr makeDecl(const std::string& name, SymbolId symbol, TypeInfo type) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_symbol = symbol;
    stmt->decl_type = std::move(type);
    return stmt;
}

S3StmtPtr makeAssign(const std::string& name, SymbolId symbol, TypeInfo type, Operand value) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Assign;
    stmt->target = slotLValue(name, symbol, type);
    stmt->value = std::move(value);
    return stmt;
}

} // namespace

void lowerFunctionExits(FunctionCFG& cfg, std::vector<CFGWarning>&) {
    if (isVoidType(cfg.return_type)) return;

    std::string slot = makeReturnSlotName(cfg);
    cfg.return_slot = slot;
    SymbolId slot_symbol = static_cast<SymbolId>(cfg.symbols.size());
    cfg.return_slot_symbol = slot_symbol;
    SymbolInfo info;
    info.id = slot_symbol;
    info.name = slot;
    info.type = cfg.return_type;
    info.declaring_scope = -1;
    cfg.symbols.push_back(std::move(info));

    auto* entry = cfg.blocks.empty() ? nullptr : cfg.blocks[static_cast<std::size_t>(cfg.entry)].get();
    if (entry) {
        entry->stmts.insert(entry->stmts.begin(),
                            CFGStmt{CFGStmtKind::Decl,
                                    makeDecl(slot, slot_symbol, cfg.return_type)});
    }

    for (auto& block : cfg.blocks) {
        if (block->terminator.kind != TermKind::Return) continue;
        if (!block->terminator.return_value) {
            ErrorContext context;
            context.stage = "s4cfg";
            throwRTLZZ(std::move(context),
                       "Non-void function '" + cfg.name + "' has return without value");
        }
        auto value = block->terminator.return_value.value();
        block->stmts.push_back(
            CFGStmt{CFGStmtKind::Assign,
                    makeAssign(slot, slot_symbol, cfg.return_type, value)});
        block->terminator.return_value = slotOperand(slot, slot_symbol, cfg.return_type);
    }
}

} // namespace pred::s4cfg
