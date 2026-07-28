#pragma once

#include "s0clang18/s006type.hpp"

#include <clang/AST/APValue.h>
#include <clang/AST/Expr.h>
#include <clang/AST/TemplateBase.h>
#include <llvm/ADT/APSInt.h>

#include <optional>
#include <string>

namespace pred::s0clang18 {

struct ConstValue {
    enum class Kind {
        Invalid,
        Bool,
        Integer,
    };

    Kind kind = Kind::Invalid;
    llvm::APSInt integer;
    bool boolean = false;
    DebugLoc loc;
};

struct ConstEvalContext {
    const Clang18Session* session = nullptr;
    const TypeLoweringContext* type_context = nullptr;
};

StepResult<ConstValue> evalConstExpr(const ConstEvalContext& context,
                                     const clang::Expr* expr,
                                     DebugLoc loc = {});

StepResult<long long> evalIntegerExpr(const ConstEvalContext& context,
                                      const clang::Expr* expr,
                                      DebugLoc loc = {});

StepResult<bool> evalBoolExpr(const ConstEvalContext& context,
                              const clang::Expr* expr,
                              DebugLoc loc = {});

StepResult<long long> evalTemplateIntegralArgument(
    const ConstEvalContext& context,
    const clang::TemplateArgument& argument,
    DebugLoc loc = {});

} // namespace pred::s0clang18

