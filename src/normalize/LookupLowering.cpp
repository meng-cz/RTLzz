#include "normalize/NormalizeUtils.h"

#include <utility>

namespace pred {

ExprPtr makeLookupExpr(const std::string& table_name, const TypeInfo& table_type, ExprPtr index) {
    auto out = std::make_shared<Expr>();
    out->kind = ExprKind::Call;
    out->callee = "lookup";
    out->args.push_back(make_literal(table_name, table_type));
    out->args.push_back(std::move(index));
    out->type = scalarTypeFromArray(table_type);
    return out;
}

} // namespace pred
