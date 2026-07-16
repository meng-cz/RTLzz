#pragma once

#include "backend/beir.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pred::pipelinev2 {

enum class OutputKind {
    Beir,
    Rtl,
    PortMetadata,
};

struct PipelineConfig {
    std::string source_name;
    std::optional<std::string> source_text;
    std::string top_function;
    std::vector<std::string> clang_args;
    int unroll_limit = 1024;
    std::vector<std::string> beopt_args;
    OutputKind output_kind = OutputKind::Rtl;
};

struct PipelineResult {
    std::optional<beir::Program> beir_program;
    std::string output_text;
    std::string error;

    bool ok() const { return error.empty(); }
};

PipelineResult compile(const PipelineConfig& config);

} // namespace pred::pipelinev2
