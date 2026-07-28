#pragma once

#include "s0clang18/s016portlift.hpp"

#include <string>
#include <vector>

namespace pred::s0clang18 {

enum class SurfaceIssueKind {
    UnknownType,
    UnresolvedCallee,
    UnresolvedSymbol,
    IllegalPort,
    IllegalReferenceOrPointer,
    UnsupportedClangNode,
    UnsupportedCppSubset,
};

struct SurfaceValidationIssue {
    SurfaceIssueKind kind = SurfaceIssueKind::UnsupportedCppSubset;
    std::string message;
    DebugLoc loc;
};

struct SurfaceValidationResult {
    std::vector<SurfaceValidationIssue> issues;
    std::vector<Diagnostic> diagnostics;
    bool ok() const { return issues.empty() && diagnostics.empty(); }
};

SurfaceValidationResult validateSurfaceAST(
    const pred::v2::FunctionAST& function,
    const PortDeclTable& ports,
    const RecordMetadataSet& records,
    const FunctionReachabilityGraph& reachability);

} // namespace pred::s0clang18

