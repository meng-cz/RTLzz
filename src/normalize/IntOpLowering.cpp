#include "normalize/NormalizeUtils.h"
#include "semantics/IntSemantics.h"

#include <algorithm>
#include <cctype>

namespace pred {

bool isUIntName(const std::string& name) {
    return name.rfind("UInt<", 0) == 0 || name.rfind("UInt <", 0) == 0 ||
           name.rfind("Int<", 0) == 0 || name.rfind("Int <", 0) == 0 ||
           name.find("uint") != std::string::npos || name.find("int") != std::string::npos ||
           name == "bool";
}

int explicitHwWidthFromName(const std::string& name) {
    auto lt = name.find('<');
    auto gt = name.find('>', lt == std::string::npos ? 0 : lt);
    if (lt == std::string::npos || gt == std::string::npos || gt <= lt + 1) return 0;
    std::string width = name.substr(lt + 1, gt - lt - 1);
    width.erase(std::remove_if(width.begin(), width.end(), [](unsigned char c) {
        return std::isspace(c);
    }), width.end());
    if (width.empty() || !std::all_of(width.begin(), width.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return 0;
    }
    return std::stoi(width);
}

bool isMutableParam(const ParamDecl& p) {
    return p.passing == ParamPassingKind::MutableRef ||
           p.passing == ParamPassingKind::Pointer;
}

bool isOutputParam(const ParamDecl& p) {
    return p.direction == ParamDirection::Output ||
           p.direction == ParamDirection::InOut;
}

bool isInputParam(const ParamDecl& p) {
    if (p.direction == ParamDirection::Input || p.direction == ParamDirection::InOut) return true;
    return false;
}

std::string baseName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::UnaryOp && e->op == "*") return baseName(e->operand);
    if (e->kind == ExprKind::Cast) return baseName(e->cast_expr);
    if (e->kind == ExprKind::FieldAccess) {
        std::string b = baseName(e->struct_base);
        return b.empty() ? e->field_name : b + "_" + e->field_name;
    }
    return "";
}

bool isConstantExpr(const ExprPtr& e) {
    if (!e) return false;
    if (e->kind == ExprKind::Literal) return true;
    if (e->kind == ExprKind::Cast || e->kind == ExprKind::ZExt ||
        e->kind == ExprKind::SExt || e->kind == ExprKind::Trunc) {
        return isConstantExpr(e->cast_expr);
    }
    if (e->kind == ExprKind::UnaryOp) return isConstantExpr(e->operand);
    if (e->kind == ExprKind::BinaryOp) return isConstantExpr(e->left) && isConstantExpr(e->right);
    if (e->kind == ExprKind::Ternary) {
        return isConstantExpr(e->cond) && isConstantExpr(e->then_expr) && isConstantExpr(e->else_expr);
    }
    return false;
}

bool isWidthCastableConstantExpr(const ExprPtr& e) {
    if (!e) return false;
    if (isConstantExpr(e)) return true;
    if (e->kind == ExprKind::Ternary) {
        return isWidthCastableConstantExpr(e->then_expr) &&
               isWidthCastableConstantExpr(e->else_expr);
    }
    if (e->kind == ExprKind::Cast || e->kind == ExprKind::ZExt ||
        e->kind == ExprKind::SExt || e->kind == ExprKind::Trunc) {
        return isWidthCastableConstantExpr(e->cast_expr);
    }
    return false;
}

bool isConstantContextBinaryOp(const std::string& op) {
    return op == "+" || op == "-" || op == "*" ||
           op == "&" || op == "|" || op == "^" ||
           op == "==" || op == "!=" || op == "<" || op == "<=" ||
           op == ">" || op == ">=";
}

bool isBoolType(const TypeInfo& type) {
    return type.hw_kind == "bool" || type.name == "bool";
}

TypeInfo resultTypeForBinary(const std::string& op, const TypeInfo& a, const TypeInfo& b) {
    return IntSemantics::binaryResultType(op, a, b).type;
}

TypeInfo constantContextType(TypeInfo target_type) {
    if (target_type.hw_kind == "signed_view") {
        return make_hw_type("Int", target_type.width, true);
    }
    if (target_type.hw_kind.empty() && target_type.width > 0) {
        target_type.hw_kind = target_type.is_signed ? "Int" : "UInt";
        target_type.name = target_type.hw_kind + "<" + std::to_string(target_type.width) + ">";
        target_type.is_hw_int = true;
    }
    return target_type;
}

ExprPtr castIfWidthChanges(ExprPtr value, const TypeInfo& target_type) {
    if (!value || target_type.width <= 0 || value->type.width <= 0) {
        return value;
    }
    if (target_type.width == value->type.width) {
        const bool has_target_type =
            !target_type.name.empty() || !target_type.hw_kind.empty() || target_type.is_hw_int;
        const bool metadata_differs =
            value->type.name != target_type.name ||
            value->type.hw_kind != target_type.hw_kind ||
            value->type.is_signed != target_type.is_signed ||
            value->type.is_hw_int != target_type.is_hw_int;
        if (has_target_type && metadata_differs) {
            // A same-width signed/unsigned conversion is still semantic. Keep
            // it as an explicit node so SSA symbol typing cannot erase the
            // signed view and make div/rem/compare silently unsigned.
            auto out = std::make_shared<Expr>();
            out->kind = ExprKind::Cast;
            out->cast_expr = std::move(value);
            out->cast_type = target_type;
            out->type = target_type;
            return out;
        }
        return value;
    }
    if (target_type.width > value->type.width) {
        ExprPtr widened;
        if (value->type.hw_kind == "signed_view" || value->type.is_signed) {
            widened = make_sext(value, target_type.width);
        } else {
            widened = make_zext(value, target_type.width);
        }
        widened->type = target_type;
        return widened;
    }
    if (value->type.hw_kind == "signed_view" && target_type.width > 0 && value->type.width > target_type.width) {
        auto narrowed = make_trunc(value, target_type.width, target_type.is_signed);
        narrowed->type = target_type;
        auto sign = make_bit_select(cloneExpr(value), value->type.width - 1);
        return make_write_bit(narrowed, target_type.width - 1, sign, target_type);
    }
    auto narrowed = make_trunc(value, target_type.width, target_type.is_signed);
    narrowed->type = target_type;
    return narrowed;
}

ExprPtr castConstantToContext(ExprPtr value, TypeInfo target_type) {
    if (!value || target_type.width <= 0 || isBoolType(target_type)) return value;
    target_type = constantContextType(target_type);
    if (value->type.width <= 0) {
        auto out = cloneExpr(value);
        out->type = target_type;
        return out;
    }
    auto casted = castIfWidthChanges(value, target_type);
    if (!casted) return casted;
    if (casted->type.width == target_type.width &&
        (casted->type.hw_kind != target_type.hw_kind ||
         casted->type.is_signed != target_type.is_signed ||
         casted->type.name != target_type.name)) {
        auto out = cloneExpr(casted);
        out->type = target_type;
        return out;
    }
    return casted;
}

void normalizeConstantOperandsForBinary(const std::string& op, ExprPtr& lhs, ExprPtr& rhs) {
    if (!isConstantContextBinaryOp(op) || !lhs || !rhs) return;
    if (isWidthCastableConstantExpr(lhs) && !isConstantExpr(rhs) &&
        rhs->type.width > 0 && !isBoolType(rhs->type)) {
        lhs = castConstantToContext(lhs, rhs->type);
    }
    if (isWidthCastableConstantExpr(rhs) && !isConstantExpr(lhs) &&
        lhs->type.width > 0 && !isBoolType(lhs->type)) {
        rhs = castConstantToContext(rhs, lhs->type);
    }
}

void normalizeConstantBranchesForTernary(ExprPtr& then_expr, ExprPtr& else_expr) {
    if (!then_expr || !else_expr) return;
    if (isWidthCastableConstantExpr(then_expr) && !isConstantExpr(else_expr) &&
        else_expr->type.width > 0 && !isBoolType(else_expr->type)) {
        then_expr = castConstantToContext(then_expr, else_expr->type);
    }
    if (isWidthCastableConstantExpr(else_expr) && !isConstantExpr(then_expr) &&
        then_expr->type.width > 0 && !isBoolType(then_expr->type)) {
        else_expr = castConstantToContext(else_expr, then_expr->type);
    }
}

ExprPtr cloneExpr(const ExprPtr& e) {
    if (!e) return nullptr;
    auto r = std::make_shared<Expr>(*e);
    if (e->left) r->left = cloneExpr(e->left);
    if (e->right) r->right = cloneExpr(e->right);
    if (e->operand) r->operand = cloneExpr(e->operand);
    if (e->array_base) r->array_base = cloneExpr(e->array_base);
    if (e->index) r->index = cloneExpr(e->index);
    if (e->struct_base) r->struct_base = cloneExpr(e->struct_base);
    if (e->cast_expr) r->cast_expr = cloneExpr(e->cast_expr);
    if (e->cond) r->cond = cloneExpr(e->cond);
    if (e->then_expr) r->then_expr = cloneExpr(e->then_expr);
    if (e->else_expr) r->else_expr = cloneExpr(e->else_expr);
    if (e->base) r->base = cloneExpr(e->base);
    if (e->value) r->value = cloneExpr(e->value);
    r->args.clear();
    for (auto& a : e->args) r->args.push_back(cloneExpr(a));
    r->parts.clear();
    for (auto& p : e->parts) r->parts.push_back(cloneExpr(p));
    return r;
}

int literalIntValue(const ExprPtr& e, int fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::Literal) {
        try { return std::stoi(e->literal_value, nullptr, 0); } catch (...) { return fallback; }
    }
    if (e->kind == ExprKind::Cast || e->kind == ExprKind::ZExt ||
        e->kind == ExprKind::SExt || e->kind == ExprKind::Trunc) {
        return literalIntValue(e->cast_expr, fallback);
    }
    return fallback;
}

bool isSignedViewExpr(const ExprPtr& e) {
    return e && e->type.hw_kind == "signed_view";
}

ExprPtr foldConstantOvershift(const std::string& op, const ExprPtr& lhs, const ExprPtr& rhs) {
    if (op != "<<" && op != ">>") return nullptr;
    if (!lhs || lhs->type.width <= 0) return nullptr;
    int shift = literalIntValue(rhs, -1);
    if (shift < lhs->type.width) return nullptr;

    if (op == ">>" && isSignedViewExpr(lhs)) {
        auto sign = make_bit_select(lhs, lhs->type.width - 1);
        auto out = make_repeat(sign, lhs->type.width);
        out->type = lhs->type;
        return out;
    }
    return make_literal("0", lhs->type);
}

} // namespace pred
