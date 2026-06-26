#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace pred {

bool collectArrayAccess(const ExprPtr& e, ExprPtr& base, std::vector<ExprPtr>& indices) {
    if (!e) return false;
    if (e->kind == ExprKind::Cast) return collectArrayAccess(e->cast_expr, base, indices);
    if (e->kind == ExprKind::UnaryOp && e->op == "*") return collectArrayAccess(e->operand, base, indices);
    if (e->kind == ExprKind::Call && e->callee == "operator[]" && e->args.size() >= 2) {
        auto receiver = e->args.front();
        ExprPtr receiver_base;
        if (receiver && receiver->kind == ExprKind::FieldAccess &&
            receiver->field_name == "operator[]" && receiver->struct_base) {
            receiver_base = receiver->struct_base;
        } else if (receiver && (!baseName(receiver).empty() ||
                               receiver->kind == ExprKind::ArrayAccess ||
                               receiver->kind == ExprKind::Cast ||
                               receiver->kind == ExprKind::UnaryOp)) {
            receiver_base = receiver;
        }
        if (!receiver_base) {
            return false;
        }
        auto array_access = make_array_access(receiver_base, e->args[1], e->type);
        return collectArrayAccess(array_access, base, indices);
    }
    if (e->kind != ExprKind::ArrayAccess) return false;
    ExprPtr nested_base;
    std::vector<ExprPtr> nested_indices;
    if (collectArrayAccess(e->array_base, nested_base, nested_indices)) {
        base = nested_base;
        indices.insert(indices.end(), nested_indices.begin(), nested_indices.end());
    } else {
        base = e->array_base;
    }
    indices.push_back(e->index);
    return true;
}

TypeInfo scalarTypeFromArray(TypeInfo t) {
    int elems = 1;
    if (!t.array_dims.empty()) {
        for (int d : t.array_dims) elems *= std::max(1, d);
    } else if (t.array_size > 0) {
        elems = t.array_size;
    }
    bool explicit_hw_scalar = t.name.rfind("Int<", 0) == 0 || t.name.rfind("UInt<", 0) == 0 ||
                              t.name.rfind("Int <", 0) == 0 || t.name.rfind("UInt <", 0) == 0;
    if (explicit_hw_scalar) {
        int explicit_width = explicitHwWidthFromName(t.name);
        if (explicit_width > 0) t.width = explicit_width;
    } else if (elems > 1 && t.width > 0 && t.width % elems == 0) {
        int elem_width = t.width / elems;
        if (elem_width == 1 || elem_width == 8 || elem_width == 16 ||
            elem_width == 32 || elem_width == 64) {
            t.width = elem_width;
        }
    }
    t.is_array = false;
    t.array_size = 0;
    t.array_dims.clear();
    return t;
}

std::string joinIndexName(const std::string& base, const std::vector<int>& idxs) {
    std::string out = base;
    for (int i : idxs) out += "_" + std::to_string(i);
    return out;
}

int flatElementCount(const TypeInfo& type) {
    int total = 1;
    if (!type.array_dims.empty()) {
        for (int d : type.array_dims) total *= d;
    } else if (type.array_size > 0) {
        total = type.array_size;
    }
    return total;
}

std::string targetName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::Cast) return targetName(e->cast_expr);
    if (e->kind == ExprKind::UnaryOp && e->op == "*") return targetName(e->operand);
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::FieldAccess) return baseName(e);
    if (e->kind == ExprKind::ArrayAccess) return baseName(e->array_base);
    return "";
}

std::optional<int> literalIndex(const ExprPtr& e) {
    if (!e) return std::nullopt;
    if (e->kind == ExprKind::Cast || e->kind == ExprKind::ZExt ||
        e->kind == ExprKind::SExt || e->kind == ExprKind::Trunc) {
        return literalIndex(e->cast_expr);
    }
    if (e->kind == ExprKind::UnaryOp && e->op == "-") {
        auto value = literalIndex(e->operand);
        if (value) return -*value;
        return std::nullopt;
    }
    if (e->kind == ExprKind::Literal) {
        try {
            return static_cast<int>(std::stoll(e->literal_value, nullptr, 0));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (e->kind == ExprKind::BinaryOp) {
        auto l = literalIndex(e->left);
        auto r = literalIndex(e->right);
        if (!l.has_value() || !r.has_value()) return std::nullopt;
        if (e->op == "+") return *l + *r;
        if (e->op == "-") return *l - *r;
        if (e->op == "*") return *l * *r;
    }
    return std::nullopt;
}

} // namespace pred
