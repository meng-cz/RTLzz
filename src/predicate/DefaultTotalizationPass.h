#pragma once

#include "ast/AST.h"

#include <string>

namespace pred {

struct DefaultTotalizationDecision {
    bool allowed = false;
    bool guard_controlled = false;
    std::string reason;
};

DefaultTotalizationDecision classifyDefaultTotalization(const std::string& name,
                                                        const TypeInfo& type);
DefaultTotalizationDecision classifyDefaultTotalization(const std::string& name,
                                                        const TypeInfo& type,
                                                        const std::string& semantic_reason);
ExprPtr makeDefaultTotalizedValue(const std::string& name, const TypeInfo& type);

} // namespace pred
