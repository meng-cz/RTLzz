#pragma once

#include "s0ast/S0AST.h"
#include "s0clang18/s017validate.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pred::s0clang18 {

struct Clang18PipelineState {
    Clang18Options options;
    std::optional<Clang18Session> session;
    std::optional<PortDeclTable> ports;
    std::optional<TopFunctionSelection> top;
    std::optional<SemanticIndex> semantic_index;
    std::optional<RecordMetadataSet> records;
    std::optional<FunctionReachabilityGraph> reachability;
    std::optional<TemplateSpecializationTable> template_specializations;
    std::optional<LambdaCaptureTable> lambdas;
    std::optional<pred::v2::FunctionAST> surface_ast;
    std::vector<Diagnostic> diagnostics;
};

struct Clang18BuildResult {
    std::optional<pred::s0ast::S0Program> program;
    std::optional<Diagnostic> error;
    std::vector<Diagnostic> warnings;
    std::string debug_text;

    bool ok() const { return program.has_value() && !error.has_value(); }
};

Clang18BuildResult buildS0ProgramWithClang18(const Clang18Options& options);

pred::s0ast::S0Program bridgeToS0Program(const Clang18PipelineState& state,
                                         pred::v2::FunctionAST surface_ast);

std::string debugPrint(const Clang18PipelineState& state);

} // namespace pred::s0clang18

