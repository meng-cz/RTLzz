#include "normalize/NormalizeUtils.h"

#include <utility>

namespace pred {

StmtPtr makeAssignStmt(ExprPtr target, ExprPtr value) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Assign;
    s->assign_target = std::move(target);
    s->assign_value = std::move(value);
    return s;
}

} // namespace pred
