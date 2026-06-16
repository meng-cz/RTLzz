#include "normalize/NormalizeUtils.h"

namespace pred {

std::string directVarName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::Cast) return directVarName(e->cast_expr);
    if (e->kind == ExprKind::UnaryOp && (e->op == "&" || e->op == "*")) {
        return directVarName(e->operand);
    }
    if (e->kind == ExprKind::Call && !e->args.empty()) {
        for (auto& arg : e->args) {
            std::string name = directVarName(arg);
            if (!name.empty()) return name;
        }
    }
    if (e->kind == ExprKind::FieldAccess) return directVarName(e->struct_base);
    return "";
}

bool fieldAccessPath(const ExprPtr& e, std::string& object, std::vector<std::string>& fields) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) {
        object = e->var_name;
        return !object.empty();
    }
    if (e->kind != ExprKind::FieldAccess) return false;
    if (!fieldAccessPath(e->struct_base, object, fields)) return false;
    fields.push_back(e->field_name);
    return true;
}

} // namespace pred
