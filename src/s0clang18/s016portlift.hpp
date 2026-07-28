#pragma once

#include "s0clang18/s015calls.hpp"
#include "v2/V2AST.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

struct ImplicitPortParam {
    std::string port_name;
    std::string param_name;
    pred::v2::ParamDecl param;
    DebugLoc loc;
};

struct FunctionPortRequirement {
    SemanticEntityId function_id = -1;
    std::string function_name;
    std::vector<ImplicitPortParam> ports;
};

struct PortLiftPlan {
    std::vector<FunctionPortRequirement> requirements;
    std::unordered_map<SemanticEntityId, std::size_t> requirement_by_function;
};

struct PortLiftResult {
    pred::v2::FunctionAST function;
    PortLiftPlan plan;
    std::vector<Diagnostic> diagnostics;
};

StepResult<PortLiftPlan> analyzeGlobalPortRequirements(
    const pred::v2::FunctionAST& function,
    const PortDeclTable& ports,
    const FunctionReachabilityGraph& reachability);

PortLiftResult liftGlobalPorts(pred::v2::FunctionAST function,
                               const PortDeclTable& ports,
                               const PortLiftPlan& plan);

} // namespace pred::s0clang18
