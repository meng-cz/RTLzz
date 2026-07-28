#pragma once

#include "s0clang18/s009reachability.hpp"

#include <clang/AST/TemplateBase.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

struct TemplateValueBinding {
    std::string parameter_name;
    long long value = 0;
    DebugLoc loc;
};

struct TemplateSpecializationInfo {
    SemanticEntityId function_id = -1;
    const clang::FunctionDecl* specialization_decl = nullptr;
    const clang::TemplateDecl* primary_template_decl = nullptr;
    std::vector<TemplateValueBinding> value_bindings;
    DebugLoc call_loc;
};

struct TemplateSpecializationTable {
    std::vector<TemplateSpecializationInfo> specializations;
    std::unordered_map<SemanticEntityId, std::size_t> specialization_by_function;
};

StepResult<TemplateSpecializationTable> resolveTemplateSpecializations(
    const Clang18Session& session,
    const FunctionReachabilityGraph& reachability,
    const ConstEvalContext& const_eval,
    const SourceLocPolicy& loc_policy = {});

const TemplateSpecializationInfo* findTemplateSpecialization(
    const TemplateSpecializationTable& table,
    SemanticEntityId function_id);

} // namespace pred::s0clang18

