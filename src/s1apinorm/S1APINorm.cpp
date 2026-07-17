#include "s1apinorm/S1APINorm.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace pred::s1apinorm {

using FunctionAST = pred::v2::FunctionAST;
using Expr = pred::v2::Expr;
using ExprPtr = pred::v2::ExprPtr;
using ExprKind = pred::v2::ExprKind;
using Stmt = pred::v2::Stmt;
using StmtPtr = pred::v2::StmtPtr;
using StmtKind = pred::v2::StmtKind;
using CaseClause = pred::v2::CaseClause;
using IntrinsicKind = pred::v2::IntrinsicKind;
using pred::v2::canonicalize_bool_type;
using pred::v2::make_hw_type;

namespace {

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s1apinorm";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

std::optional<int> literalInt(const ExprPtr& expr) {
    if (!expr || expr->kind != ExprKind::Literal) return std::nullopt;
    try {
        std::size_t pos = 0;
        int value = std::stoi(expr->literal_value, &pos, 0);
        if (pos == expr->literal_value.size()) return value;
    } catch (...) {
    }
    return std::nullopt;
}

std::string canonicalCallee(std::string name) {
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), name.end());
    auto scope = name.rfind("::");
    if (scope != std::string::npos) name = name.substr(scope + 2);
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

std::string canonicalTypeName(std::string name) {
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    return canonicalCallee(std::move(name));
}

bool isDynamicRangeAt(const Expr& expr) {
    return expr.intrinsic == IntrinsicKind::DynamicRangeAt ||
           expr.callee == "__dynamic_range_at";
}

bool isDynamicBitAt(const Expr& expr) {
    return expr.intrinsic == IntrinsicKind::DynamicBitAt ||
           expr.callee == "__dynamic_bit_at";
}

struct NormStats {
    int normalized_calls = 0;
    int normalized_writes = 0;
};

S1ExprPtr makeHardware(S1HardwareOp op, TypeInfo type, DebugLoc loc = {}) {
    auto out = std::make_shared<S1Expr>();
    out->kind = S1ExprKind::HardwareOp;
    out->hardware_op = op;
    out->type = std::move(type);
    out->debug_loc = std::move(loc);
    return out;
}

S1ExprPtr makeCast(S1ExprPtr value, TypeInfo target) {
    auto out = std::make_shared<S1Expr>();
    out->kind = S1ExprKind::Cast;
    out->cast_type = target;
    out->type = std::move(target);
    out->debug_loc = value ? value->debug_loc : DebugLoc{};
    out->cast_expr = std::move(value);
    return out;
}

S1ExprPtr makeZExt(S1ExprPtr value, int width) {
    auto out = makeHardware(S1HardwareOp::ZExt, make_hw_type("UInt", width, false),
                            value ? value->debug_loc : DebugLoc{});
    out->cast_expr = std::move(value);
    out->to_width = width;
    return out;
}

S1ExprPtr makeSExt(S1ExprPtr value, int width) {
    auto out = makeHardware(S1HardwareOp::SExt, make_hw_type("Int", width, true),
                            value ? value->debug_loc : DebugLoc{});
    out->cast_expr = std::move(value);
    out->to_width = width;
    return out;
}

S1ExprPtr makeTrunc(S1ExprPtr value, int width, bool is_signed) {
    auto out = makeHardware(S1HardwareOp::Trunc,
                            make_hw_type(is_signed ? "Int" : "UInt", width, is_signed),
                            value ? value->debug_loc : DebugLoc{});
    out->cast_expr = std::move(value);
    out->to_width = width;
    return out;
}

S1ExprPtr makeSlice(S1ExprPtr base, int hi, int lo, TypeInfo type) {
    if (type.width <= 0) type = make_hw_type("UInt", hi >= lo ? hi - lo + 1 : 0, false);
    int width = hi >= lo ? hi - lo + 1 : type.width;
    type = canonicalize_bool_type(std::move(type));
    type.width = width;
    auto out = makeHardware(S1HardwareOp::Slice, std::move(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->hi = hi;
    out->lo = lo;
    return out;
}

S1ExprPtr makeBitSelect(S1ExprPtr base, int bit) {
    auto out = makeHardware(S1HardwareOp::BitSelect, make_hw_type("bool", 1, false),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->bit = bit;
    return out;
}

S1ExprPtr makeDynamicSlice(S1ExprPtr base, S1ExprPtr index, TypeInfo type, int width) {
    if (type.width <= 0 && width > 0) type = make_hw_type("UInt", width, false);
    auto out = makeHardware(S1HardwareOp::DynamicSlice, std::move(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->index = std::move(index);
    out->to_width = width;
    return out;
}

S1ExprPtr makeDynamicBitSelect(S1ExprPtr base, S1ExprPtr index) {
    auto out = makeHardware(S1HardwareOp::DynamicBitSelect,
                            make_hw_type("bool", 1, false),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->index = std::move(index);
    return out;
}

S1ExprPtr makeWriteSlice(S1ExprPtr base, int hi, int lo, S1ExprPtr value) {
    TypeInfo type = base ? base->type : TypeInfo{};
    auto out = makeHardware(S1HardwareOp::WriteSlice, canonicalize_bool_type(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->hi = hi;
    out->lo = lo;
    out->value = std::move(value);
    return out;
}

S1ExprPtr makeWriteBit(S1ExprPtr base, int bit, S1ExprPtr value) {
    TypeInfo type = base ? base->type : TypeInfo{};
    auto out = makeHardware(S1HardwareOp::WriteBit, canonicalize_bool_type(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->bit = bit;
    out->value = std::move(value);
    return out;
}

S1ExprPtr makeDynamicWriteSlice(S1ExprPtr base, S1ExprPtr index, S1ExprPtr value) {
    TypeInfo type = base ? base->type : TypeInfo{};
    auto out = makeHardware(S1HardwareOp::DynamicWriteSlice, canonicalize_bool_type(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->index = std::move(index);
    out->value = std::move(value);
    return out;
}

S1ExprPtr makeDynamicWriteBit(S1ExprPtr base, S1ExprPtr index, S1ExprPtr value) {
    TypeInfo type = base ? base->type : TypeInfo{};
    auto out = makeHardware(S1HardwareOp::DynamicWriteBit, canonicalize_bool_type(type),
                            base ? base->debug_loc : DebugLoc{});
    out->base = std::move(base);
    out->index = std::move(index);
    out->value = std::move(value);
    return out;
}

S1ExprPtr makeConcat(std::vector<S1ExprPtr> parts) {
    int width = 0;
    bool is_signed = false;
    for (const auto& part : parts) {
        if (!part) continue;
        width += part->type.width;
        is_signed = is_signed || part->type.is_signed;
    }
    auto out = makeHardware(S1HardwareOp::Concat,
                            make_hw_type(is_signed ? "Int" : "UInt", width, is_signed));
    out->parts = std::move(parts);
    return out;
}

S1ExprPtr makeRepeat(S1ExprPtr value, int times) {
    int width = value ? value->type.width * times : 0;
    bool is_signed = value ? value->type.is_signed : false;
    auto out = makeHardware(S1HardwareOp::Repeat,
                            make_hw_type(is_signed ? "Int" : "UInt", width, is_signed),
                            value ? value->debug_loc : DebugLoc{});
    out->operand = std::move(value);
    out->times = times;
    return out;
}

S1ExprPtr makeReduce(S1HardwareOp op, S1ExprPtr value) {
    auto out = makeHardware(op, make_hw_type("bool", 1, false),
                            value ? value->debug_loc : DebugLoc{});
    out->operand = std::move(value);
    return out;
}

S1ExprPtr cloneExpr(const S1ExprPtr& expr) {
    if (!expr) return nullptr;
    auto out = std::make_shared<S1Expr>(*expr);
    out->left = cloneExpr(expr->left);
    out->right = cloneExpr(expr->right);
    out->operand = cloneExpr(expr->operand);
    out->array_base = cloneExpr(expr->array_base);
    out->index = cloneExpr(expr->index);
    out->struct_base = cloneExpr(expr->struct_base);
    out->args.clear();
    for (const auto& arg : expr->args) out->args.push_back(cloneExpr(arg));
    out->cast_expr = cloneExpr(expr->cast_expr);
    out->cond = cloneExpr(expr->cond);
    out->then_expr = cloneExpr(expr->then_expr);
    out->else_expr = cloneExpr(expr->else_expr);
    out->base = cloneExpr(expr->base);
    out->value = cloneExpr(expr->value);
    out->parts.clear();
    for (const auto& part : expr->parts) out->parts.push_back(cloneExpr(part));
    return out;
}

class Normalizer {
public:
    S1FunctionAST run(const FunctionAST& input, std::vector<APINormSummary>& summaries) {
        return normalizeFunction(input, summaries);
    }

private:
    S1FunctionAST normalizeFunction(const FunctionAST& input,
                                    std::vector<APINormSummary>& summaries) {
        S1FunctionAST out;
        out.name = input.name;
        out.return_type = input.return_type;
        out.params = input.params;
        out.struct_fields = input.struct_fields;
        out.struct_constructors = input.struct_constructors;

        NormStats stats;
        out.body = normalizeStmtList(input.body, stats);
        for (const auto& helper : input.helpers) {
            if (!helper) continue;
            out.helpers.push_back(
                std::make_shared<S1FunctionAST>(normalizeFunction(*helper, summaries)));
        }
        for (const auto& [name, lambda] : input.lambdas) {
            if (!lambda) continue;
            out.lambdas[name] =
                std::make_shared<S1FunctionAST>(normalizeFunction(*lambda, summaries));
        }

        APINormSummary summary;
        summary.function_name = out.name;
        summary.normalized_calls = stats.normalized_calls;
        summary.normalized_writes = stats.normalized_writes;
        summaries.push_back(std::move(summary));
        return out;
    }

    std::vector<S1StmtPtr> normalizeStmtList(const std::vector<StmtPtr>& input,
                                             NormStats& stats) {
        std::vector<S1StmtPtr> out;
        out.reserve(input.size());
        for (const auto& stmt : input) {
            auto normalized = normalizeStmt(stmt, stats);
            out.insert(out.end(), normalized.begin(), normalized.end());
        }
        return out;
    }

    std::vector<S1CaseClause> normalizeCases(const std::vector<CaseClause>& input,
                                             NormStats& stats) {
        std::vector<S1CaseClause> out;
        out.reserve(input.size());
        for (const auto& one : input) {
            S1CaseClause c;
            if (one.value) c.value = normalizeExpr(one.value.value(), stats);
            c.body = normalizeStmtList(one.body, stats);
            out.push_back(std::move(c));
        }
        return out;
    }

    std::vector<S1StmtPtr> normalizeStmt(const StmtPtr& stmt, NormStats& stats) {
        if (!stmt) return {};
        ErrorContextGuard guard("s1apinorm", stmt->debug_loc, "normalizing statement");
        auto out = std::make_shared<S1Stmt>();
        out->debug_loc = stmt->debug_loc;
        switch (stmt->kind) {
        case StmtKind::Assign:
            out->kind = S1StmtKind::Assign;
            normalizeAssign(*out, stmt, stats);
            return {out};
        case StmtKind::Decl:
            return normalizeDecl(stmt, stats);
        case StmtKind::If:
            out->kind = S1StmtKind::If;
            out->if_cond = normalizeExpr(stmt->if_cond, stats);
            out->if_then = normalizeStmtList(stmt->if_then, stats);
            out->if_else = normalizeStmtList(stmt->if_else, stats);
            return {out};
        case StmtKind::For:
            out->kind = S1StmtKind::For;
            out->for_init = normalizeStmt(stmt->for_init, stats);
            out->for_cond = normalizeExpr(stmt->for_cond, stats);
            out->for_step = normalizeExpr(stmt->for_step, stats);
            out->for_body = normalizeStmtList(stmt->for_body, stats);
            return {out};
        case StmtKind::While:
            out->kind = S1StmtKind::While;
            out->while_cond = normalizeExpr(stmt->while_cond, stats);
            out->while_body = normalizeStmtList(stmt->while_body, stats);
            return {out};
        case StmtKind::DoWhile:
            out->kind = S1StmtKind::DoWhile;
            out->while_cond = normalizeExpr(stmt->while_cond, stats);
            out->while_body = normalizeStmtList(stmt->while_body, stats);
            return {out};
        case StmtKind::Switch:
            out->kind = S1StmtKind::Switch;
            out->switch_expr = normalizeExpr(stmt->switch_expr, stats);
            out->switch_cases = normalizeCases(stmt->switch_cases, stats);
            return {out};
        case StmtKind::Block:
            if (stmt->synthetic_flatten_block) {
                return normalizeStmtList(stmt->block_stmts, stats);
            }
            out->kind = S1StmtKind::Block;
            out->block_stmts = normalizeStmtList(stmt->block_stmts, stats);
            return {out};
        case StmtKind::Return:
            out->kind = S1StmtKind::Return;
            if (stmt->return_value) out->return_value = normalizeExpr(stmt->return_value.value(), stats);
            return {out};
        case StmtKind::ExprStmt:
            out->kind = S1StmtKind::ExprStmt;
            out->expr_stmt = normalizeExpr(stmt->expr_stmt, stats);
            return {out};
        case StmtKind::Break:
            out->kind = S1StmtKind::Break;
            return {out};
        case StmtKind::Continue:
            out->kind = S1StmtKind::Continue;
            return {out};
        }
        return {out};
    }

    S1ExprPtr makeVarRef(const std::string& name, TypeInfo type, DebugLoc loc) {
        auto expr = std::make_shared<S1Expr>();
        expr->kind = S1ExprKind::VarRef;
        expr->var_name = name;
        expr->type = std::move(type);
        expr->debug_loc = std::move(loc);
        return expr;
    }

    std::shared_ptr<S1Stmt> makeDeclStmt(const StmtPtr& stmt) {
        auto out = std::make_shared<S1Stmt>();
        out->kind = S1StmtKind::Decl;
        out->debug_loc = stmt->debug_loc;
        out->decl_type = stmt->decl_type;
        out->decl_name = stmt->decl_name;
        out->decl_default_constructed = stmt->decl_default_constructed;
        return out;
    }

    std::shared_ptr<S1Stmt> makeAssignStmt(S1ExprPtr target, S1ExprPtr value, DebugLoc loc) {
        auto out = std::make_shared<S1Stmt>();
        out->kind = S1StmtKind::Assign;
        out->debug_loc = std::move(loc);
        out->assign_target = std::move(target);
        out->assign_value = std::move(value);
        return out;
    }

    std::shared_ptr<S1Stmt> makeConstructStmt(S1ExprPtr target,
                                              std::string callee,
                                              std::vector<S1ExprPtr> args,
                                              TypeInfo type,
                                              DebugLoc loc) {
        auto out = std::make_shared<S1Stmt>();
        out->kind = S1StmtKind::Construct;
        out->debug_loc = std::move(loc);
        out->construct_target = std::move(target);
        out->construct_callee = std::move(callee);
        out->construct_args = std::move(args);
        out->construct_type = std::move(type);
        return out;
    }

    std::vector<S1ExprPtr> normalizeExprList(const std::vector<ExprPtr>& input,
                                             NormStats& stats) {
        std::vector<S1ExprPtr> out;
        out.reserve(input.size());
        for (const auto& expr : input) out.push_back(normalizeExpr(expr, stats));
        return out;
    }

    bool isConstructorInit(const ExprPtr& init, const TypeInfo& decl_type) const {
        if (!init || init->kind != ExprKind::Call) return false;
        std::string callee = canonicalTypeName(init->callee);
        std::vector<std::string> type_names;
        if (!decl_type.struct_name.empty()) type_names.push_back(decl_type.struct_name);
        if (!decl_type.name.empty()) type_names.push_back(decl_type.name);
        if (!init->type.struct_name.empty()) type_names.push_back(init->type.struct_name);
        if (!init->type.name.empty()) type_names.push_back(init->type.name);
        if (decl_type.hw_kind == "Int" || decl_type.hw_kind == "UInt" ||
            decl_type.hw_kind == "bool") {
            type_names.push_back(decl_type.hw_kind);
        }
        for (const auto& name : type_names) {
            if (!name.empty() && callee == canonicalTypeName(name)) return true;
        }
        return false;
    }

    std::vector<S1StmtPtr> normalizeDecl(const StmtPtr& stmt, NormStats& stats) {
        std::vector<S1StmtPtr> out;
        out.push_back(makeDeclStmt(stmt));

        auto target = makeVarRef(stmt->decl_name, stmt->decl_type, stmt->debug_loc);
        if (stmt->decl_init) {
            if (isConstructorInit(stmt->decl_init.value(), stmt->decl_type)) {
                auto init = stmt->decl_init.value();
                out.push_back(makeConstructStmt(
                    std::move(target),
                    init->callee,
                    normalizeExprList(init->args, stats),
                    stmt->decl_type,
                    init->debug_loc));
            } else {
                out.push_back(makeAssignStmt(
                    std::move(target),
                    normalizeExpr(stmt->decl_init.value(), stats),
                    stmt->debug_loc));
            }
            return out;
        }

        if (!stmt->decl_init_args.empty()) {
            out.push_back(makeConstructStmt(
                std::move(target),
                !stmt->decl_type.struct_name.empty()
                    ? stmt->decl_type.struct_name
                    : stmt->decl_type.name,
                normalizeExprList(stmt->decl_init_args, stats),
                stmt->decl_type,
                stmt->debug_loc));
            return out;
        }

        if (stmt->decl_default_constructed) {
            out.push_back(makeConstructStmt(
                std::move(target),
                !stmt->decl_type.struct_name.empty()
                    ? stmt->decl_type.struct_name
                    : stmt->decl_type.name,
                {},
                stmt->decl_type,
                stmt->debug_loc));
        }
        return out;
    }

    void normalizeAssign(S1Stmt& out, const StmtPtr& stmt, NormStats& stats) {
        auto value = normalizeExpr(stmt->assign_value, stats);
        if (stmt->assign_target && stmt->assign_target->kind == ExprKind::Call) {
            if (rewriteStaticSliceWrite(out, *stmt->assign_target, value, stats)) return;
            if (rewriteDynamicWrite(out, *stmt->assign_target, value, stats)) return;
        }
        out.assign_target = normalizeExpr(stmt->assign_target, stats);
        out.assign_value = std::move(value);
    }

    bool rewriteStaticSliceWrite(S1Stmt& stmt,
                                 const Expr& target,
                                 S1ExprPtr value,
                                 NormStats& stats) {
        if (target.callee == "at" || target.callee == "_at") {
            if (target.args.empty()) {
                fail("Int at assignment is missing a base expression", target.debug_loc);
            }
            if (target.hi < 0 || target.lo < 0) {
                fail("Int at assignment requires static template indices", target.debug_loc);
            }
            auto base = normalizeExpr(target.args[0], stats);
            stmt.assign_target = cloneExpr(base);
            stmt.assign_value = makeWriteSlice(std::move(base), target.hi, target.lo,
                                               std::move(value));
            ++stats.normalized_writes;
            return true;
        }
        if (target.callee != "__slice" && target.callee != "__bit") return false;
        if (target.args.empty()) fail("Int slice assignment is missing a base expression", target.debug_loc);
        auto base = normalizeExpr(target.args[0], stats);
        if (target.callee == "__bit") {
            if (target.args.size() < 2) fail("Int bit assignment is missing a bit index", target.debug_loc);
            auto bit = literalInt(target.args[1]);
            if (!bit) fail("Int bit assignment requires a static bit index", target.debug_loc);
            stmt.assign_target = cloneExpr(base);
            stmt.assign_value = makeWriteBit(std::move(base), *bit, std::move(value));
            ++stats.normalized_writes;
            return true;
        }
        if (target.args.size() < 3) fail("Int slice assignment is missing hi/lo indices", target.debug_loc);
        auto hi = literalInt(target.args[1]);
        auto lo = literalInt(target.args[2]);
        if (!hi || !lo) fail("Int slice assignment requires static hi/lo indices", target.debug_loc);
        stmt.assign_target = cloneExpr(base);
        stmt.assign_value = makeWriteSlice(std::move(base), *hi, *lo, std::move(value));
        ++stats.normalized_writes;
        return true;
    }

    bool rewriteDynamicWrite(S1Stmt& stmt,
                             const Expr& target,
                             S1ExprPtr value,
                             NormStats& stats) {
        const std::string callee = canonicalCallee(target.callee);
        const bool range_call = isDynamicRangeAt(target) ||
            callee == "range_at" || callee == "pick";
        const bool bit_call = isDynamicBitAt(target) || callee == "bit_at";
        if (!range_call && !bit_call) return false;
        if (target.args.size() < 2) fail("Dynamic Int assignment is missing base or index", target.debug_loc);
        auto base = normalizeExpr(target.args[0], stats);
        auto index = normalizeExpr(target.args[1], stats);
        stmt.assign_target = cloneExpr(base);
        stmt.assign_value = bit_call
            ? makeDynamicWriteBit(std::move(base), std::move(index), std::move(value))
            : makeDynamicWriteSlice(std::move(base), std::move(index), std::move(value));
        ++stats.normalized_writes;
        return true;
    }

    S1ExprPtr normalizeExpr(const ExprPtr& expr, NormStats& stats) {
        if (!expr) return nullptr;
        ErrorContextGuard guard("s1apinorm", expr->debug_loc, "normalizing expression");
        switch (expr->kind) {
        case ExprKind::Literal: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::Literal;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->literal_value = expr->literal_value;
            return out;
        }
        case ExprKind::VarRef: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::VarRef;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->var_name = expr->var_name;
            return out;
        }
        case ExprKind::BinaryOp: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::BinaryOp;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->op = expr->op;
            out->left = normalizeExpr(expr->left, stats);
            out->right = normalizeExpr(expr->right, stats);
            return out;
        }
        case ExprKind::UnaryOp: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::UnaryOp;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->op = expr->op;
            out->operand = normalizeExpr(expr->operand, stats);
            return out;
        }
        case ExprKind::ArrayAccess: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::ArrayAccess;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->array_base = normalizeExpr(expr->array_base, stats);
            out->index = normalizeExpr(expr->index, stats);
            return out;
        }
        case ExprKind::FieldAccess: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::FieldAccess;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->struct_base = normalizeExpr(expr->struct_base, stats);
            out->field_name = expr->field_name;
            return out;
        }
        case ExprKind::Call:
            return normalizeCall(*expr, stats);
        case ExprKind::Cast:
            return normalizeCast(expr, stats);
        case ExprKind::Ternary: {
            auto out = std::make_shared<S1Expr>();
            out->kind = S1ExprKind::Ternary;
            out->type = expr->type;
            out->debug_loc = expr->debug_loc;
            out->cond = normalizeExpr(expr->cond, stats);
            out->then_expr = normalizeExpr(expr->then_expr, stats);
            out->else_expr = normalizeExpr(expr->else_expr, stats);
            return out;
        }
        case ExprKind::ZExt:
            return makeZExt(normalizeExpr(expr->cast_expr, stats), expr->to_width);
        case ExprKind::SExt:
            return makeSExt(normalizeExpr(expr->cast_expr, stats), expr->to_width);
        case ExprKind::Trunc:
            return makeTrunc(normalizeExpr(expr->cast_expr, stats), expr->to_width,
                             expr->type.is_signed);
        case ExprKind::Slice:
            return makeSlice(normalizeExpr(expr->base, stats), expr->hi, expr->lo, expr->type);
        case ExprKind::BitSelect:
            return makeBitSelect(normalizeExpr(expr->base, stats), expr->bit);
        case ExprKind::WriteSlice:
            return makeWriteSlice(normalizeExpr(expr->base, stats), expr->hi, expr->lo,
                                  normalizeExpr(expr->value, stats));
        case ExprKind::WriteBit:
            return makeWriteBit(normalizeExpr(expr->base, stats), expr->bit,
                                normalizeExpr(expr->value, stats));
        case ExprKind::DynamicWriteSlice:
            return makeDynamicWriteSlice(normalizeExpr(expr->base, stats),
                                         normalizeExpr(expr->index, stats),
                                         normalizeExpr(expr->value, stats));
        case ExprKind::DynamicWriteBit:
            return makeDynamicWriteBit(normalizeExpr(expr->base, stats),
                                       normalizeExpr(expr->index, stats),
                                       normalizeExpr(expr->value, stats));
        case ExprKind::Concat: {
            std::vector<S1ExprPtr> parts;
            for (const auto& part : expr->parts) parts.push_back(normalizeExpr(part, stats));
            return makeConcat(std::move(parts));
        }
        case ExprKind::Repeat:
            return makeRepeat(normalizeExpr(expr->operand, stats), expr->times);
        case ExprKind::ReduceOr:
            return makeReduce(S1HardwareOp::ReduceOr, normalizeExpr(expr->operand, stats));
        case ExprKind::ReduceAnd:
            return makeReduce(S1HardwareOp::ReduceAnd, normalizeExpr(expr->operand, stats));
        case ExprKind::ReduceXor:
            return makeReduce(S1HardwareOp::ReduceXor, normalizeExpr(expr->operand, stats));
        }
        return nullptr;
    }

    S1ExprPtr normalizeCast(const ExprPtr& expr, NormStats& stats) {
        auto value = normalizeExpr(expr->cast_expr, stats);
        TypeInfo target = expr->cast_type.width > 0 || !expr->cast_type.name.empty()
            ? expr->cast_type
            : expr->type;
        return makeCast(std::move(value), std::move(target));
    }

    S1ExprPtr normalizeCall(const Expr& expr, NormStats& stats) {
        std::vector<S1ExprPtr> args;
        args.reserve(expr.args.size());
        for (const auto& arg : expr.args) args.push_back(normalizeExpr(arg, stats));

        const std::string callee = canonicalCallee(expr.callee);
        if ((expr.callee == "at" || expr.callee == "_at") && !args.empty()) {
            ++stats.normalized_calls;
            if (expr.hi >= 0 && expr.lo >= 0) {
                return makeSlice(std::move(args[0]), expr.hi, expr.lo, expr.type);
            }
            fail("Int at API normalization requires static template indices", expr.debug_loc);
        }
        if ((expr.callee == "__slice" || expr.callee == "__bit") && !args.empty()) {
            ++stats.normalized_calls;
            return normalizeStaticSliceRead(expr, std::move(args));
        }
        if (((isDynamicRangeAt(expr) || isDynamicBitAt(expr)) ||
             callee == "range_at" || callee == "bit_at" || callee == "pick") &&
            args.size() >= 2) {
            ++stats.normalized_calls;
            bool is_bit = isDynamicBitAt(expr) || callee == "bit_at";
            return is_bit
                ? makeDynamicBitSelect(std::move(args[0]), std::move(args[1]))
                : makeDynamicSlice(std::move(args[0]), std::move(args[1]), expr.type,
                                   expr.to_width);
        }

        if (callee == "Cat" || callee == "cat" || callee == "concat") {
            ++stats.normalized_calls;
            return makeConcat(std::move(args));
        }
        if ((callee == "Repeat" || callee == "repeat") && !args.empty()) {
            int times = expr.times;
            if (times <= 0 && args.size() >= 2) {
                auto source_count = expr.args.empty() ? std::nullopt : literalInt(expr.args.front());
                if (source_count) {
                    times = *source_count;
                    args.erase(args.begin());
                }
            }
            if (times <= 0) times = expr.to_width;
            if (times <= 0) fail("Repeat API normalization requires a positive repeat count", expr.debug_loc);
            ++stats.normalized_calls;
            return makeRepeat(std::move(args.back()), times);
        }
        if ((callee == "ReduceOr" || callee == "reduce_or") && !args.empty()) {
            ++stats.normalized_calls;
            return makeReduce(S1HardwareOp::ReduceOr, std::move(args.back()));
        }
        if ((callee == "ReduceAnd" || callee == "reduce_and") && !args.empty()) {
            ++stats.normalized_calls;
            return makeReduce(S1HardwareOp::ReduceAnd, std::move(args.back()));
        }
        if ((callee == "ReduceXor" || callee == "reduce_xor") && !args.empty()) {
            ++stats.normalized_calls;
            return makeReduce(S1HardwareOp::ReduceXor, std::move(args.back()));
        }
        if ((callee == "zext" || callee == "ZExt") && !args.empty()) {
            int width = expr.to_width > 0 ? expr.to_width : expr.type.width;
            if (width <= 0) fail("zext API normalization requires a target width", expr.debug_loc);
            ++stats.normalized_calls;
            return makeZExt(std::move(args.back()), width);
        }
        if ((callee == "trunc" || callee == "Trunc") && !args.empty()) {
            int width = expr.to_width > 0 ? expr.to_width : expr.type.width;
            if (width <= 0) fail("trunc API normalization requires a target width", expr.debug_loc);
            ++stats.normalized_calls;
            return makeTrunc(std::move(args.back()), width, expr.type.is_signed);
        }
        if ((callee == "to" || callee == "To") && !args.empty()) {
            TypeInfo target = expr.type;
            if (target.width <= 0) fail("to API normalization requires a target type", expr.debug_loc);
            auto value = std::move(args.back());
            ++stats.normalized_calls;
            if (value && value->type.width > 0 && target.width > value->type.width) {
                return target.is_signed ? makeSExt(std::move(value), target.width)
                                        : makeZExt(std::move(value), target.width);
            }
            if (value && value->type.width > 0 && target.width < value->type.width) {
                return makeTrunc(std::move(value), target.width, target.is_signed);
            }
            return makeCast(std::move(value), std::move(target));
        }
        if ((callee == "sint" || callee == "sint_view") && !args.empty()) {
            auto value = std::move(args.back());
            TypeInfo target = value ? value->type : expr.type;
            target.is_signed = true;
            target.is_hw_int = true;
            target.hw_kind = "signed_view";
            if (target.width > 0) {
                target.name = "IntSignedView<" + std::to_string(target.width) + ">";
            }
            ++stats.normalized_calls;
            return makeCast(std::move(value), std::move(target));
        }

        auto out = std::make_shared<S1Expr>();
        out->kind = S1ExprKind::Call;
        out->type = expr.type;
        out->debug_loc = expr.debug_loc;
        out->callee = expr.callee;
        out->args = std::move(args);
        return out;
    }

    S1ExprPtr normalizeStaticSliceRead(const Expr& expr, std::vector<S1ExprPtr> args) {
        if (expr.callee == "__bit") {
            if (args.size() < 2 || expr.args.size() < 2) {
                fail("Int bit read is missing a bit index", expr.debug_loc);
            }
            auto bit = literalInt(expr.args[1]);
            if (!bit) fail("Int bit read requires a static bit index", expr.debug_loc);
            return makeBitSelect(std::move(args[0]), *bit);
        }
        if (args.size() < 3 || expr.args.size() < 3) {
            fail("Int slice read is missing hi/lo indices", expr.debug_loc);
        }
        auto hi = literalInt(expr.args[1]);
        auto lo = literalInt(expr.args[2]);
        if (!hi || !lo) fail("Int slice read requires static hi/lo indices", expr.debug_loc);
        return makeSlice(std::move(args[0]), *hi, *lo, expr.type);
    }
};

} // namespace

const char* hardwareOpName(S1HardwareOp op) {
    switch (op) {
    case S1HardwareOp::ZExt: return "ZExt";
    case S1HardwareOp::SExt: return "SExt";
    case S1HardwareOp::Trunc: return "Trunc";
    case S1HardwareOp::Slice: return "Slice";
    case S1HardwareOp::BitSelect: return "BitSelect";
    case S1HardwareOp::DynamicSlice: return "DynamicSlice";
    case S1HardwareOp::DynamicBitSelect: return "DynamicBitSelect";
    case S1HardwareOp::WriteSlice: return "WriteSlice";
    case S1HardwareOp::WriteBit: return "WriteBit";
    case S1HardwareOp::DynamicWriteSlice: return "DynamicWriteSlice";
    case S1HardwareOp::DynamicWriteBit: return "DynamicWriteBit";
    case S1HardwareOp::Concat: return "Concat";
    case S1HardwareOp::Repeat: return "Repeat";
    case S1HardwareOp::ReduceOr: return "ReduceOr";
    case S1HardwareOp::ReduceAnd: return "ReduceAnd";
    case S1HardwareOp::ReduceXor: return "ReduceXor";
    }
    return "<unknown>";
}

std::string debugPrint(const S1FunctionAST&,
                       const std::vector<APINormSummary>& summaries) {
    std::ostringstream os;
    os << "s1apinorm\n";
    for (const auto& summary : summaries) {
        os << "function " << summary.function_name
           << " normalized_calls=" << summary.normalized_calls
           << " normalized_writes=" << summary.normalized_writes << "\n";
    }
    return os.str();
}

APINormResult normalizeAPIs(const FunctionAST& function,
                            const APINormOptions& options) {
    try {
        APINormResult result;
        Normalizer normalizer;
        result.function = normalizer.run(function, result.summaries);
        if (options.debug_print) {
            result.debug_text = debugPrint(result.function.value(), result.summaries);
        }
        return result;
    } catch (const RTLZZException& ex) {
        APINormResult result;
        APINormError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        APINormResult result;
        APINormError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

S1FunctionAST normalizeAPIsOrThrow(const FunctionAST& function,
                                   const APINormOptions& options) {
    auto result = normalizeAPIs(function, options);
    if (!result.ok()) {
        throw RTLZZException(result.error->context, result.error->message);
    }
    return std::move(result.function.value());
}

} // namespace pred::s1apinorm
