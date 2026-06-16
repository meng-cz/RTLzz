#include "predicate/DefaultTotalizationPass.h"

namespace pred {

DefaultTotalizationDecision classifyDefaultTotalization(const std::string& name,
                                                        const TypeInfo& type,
                                                        const std::string& semantic_reason) {
    if (semantic_reason == "write_enable_default_false" ||
        semantic_reason == "valid_default_false" ||
        semantic_reason == "wdata_default_zero_when_wen_false" ||
        semantic_reason == "payload_default_zero_when_valid_false") {
        DefaultTotalizationDecision d;
        d.allowed = true;
        d.reason = semantic_reason;
        d.guard_controlled = semantic_reason == "write_enable_default_false" ||
                             semantic_reason == "wdata_default_zero_when_wen_false" ||
                             semantic_reason == "payload_default_zero_when_valid_false";
        return d;
    }
    if (!semantic_reason.empty()) {
        DefaultTotalizationDecision d;
        d.reason = semantic_reason;
        return d;
    }
    return classifyDefaultTotalization(name, type);
}

DefaultTotalizationDecision classifyDefaultTotalization(const std::string& name,
                                                        const TypeInfo& type) {
    DefaultTotalizationDecision d;
    if (name.rfind("wen", 0) == 0 ||
        name.find("_wen") != std::string::npos ||
        name.find("__wen__") != std::string::npos) {
        d.reason = "write_enable_name_heuristic_requires_semantic_metadata";
        return d;
    }
    if (name.find("__vld__") != std::string::npos ||
        name.find("vld") != std::string::npos ||
        name.find("valid") != std::string::npos) {
        d.reason = "valid_name_heuristic_requires_semantic_metadata";
        return d;
    }
    if (name.find("rdy") != std::string::npos ||
        name.find("ready") != std::string::npos) {
        d.reason = "ready_requires_explicit_assignment";
        return d;
    }
    if (type.width == 1 &&
        (name.find("valid") != std::string::npos || name.find("vld") != std::string::npos)) {
        d.reason = "valid_name_heuristic_requires_semantic_metadata";
        return d;
    }
    d.reason = "ordinary_output_requires_assignment";
    return d;
}

ExprPtr makeDefaultTotalizedValue(const std::string& name, const TypeInfo& type) {
    (void)name;
    if (type.width == 1 || type.hw_kind == "bool") {
        return make_literal("false", TypeInfo{"bool", 1, false, true, "bool"});
    }
    TypeInfo t = type;
    if (t.width <= 0) t = make_hw_type("UInt", 1, false);
    return make_literal("0", t);
}

} // namespace pred
