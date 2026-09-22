#pragma once

#include "backend/beir.hpp"
#include "backend/rtlgen.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pred::pipelinev2 {

enum class OutputKind {
    Beir,
    Rtl,
    PortMetadata,
};

enum class RtlDebugOutputKind {
    None,
    Structured,
    Text,
};

struct PipelineConfig {
    std::string source_name;
    std::optional<std::string> source_text;
    std::string top_function;
    std::vector<std::string> clang_args;
    int unroll_limit = 1024;
    // Zero means no artificial S7 leaf-symbol limit.
    std::size_t max_leaf_symbols = 0;
    std::vector<std::string> beopt_args;
    // Optional status hook. Called before each long-running frontend stage and
    // backend optimization iteration; stages remain silent when it is absent.
    std::function<void(const std::string&)> progress_callback;
    OutputKind output_kind = OutputKind::Rtl;
    RtlDebugOutputKind rtl_debug_output = RtlDebugOutputKind::None;
    bool rtl_module_body = false;
    std::vector<std::pair<std::string, std::string>> rtl_port_bindings;
};

struct PipelineResult {
    std::optional<beir::Program> beir_program;
    std::string output_text;
    std::string rtl_debug_text;
    std::vector<rtlgen::RtlDebugSignal> rtl_debug_signals;
    std::string error_debug_text;
    std::string error_signal_debug_text;
    std::vector<rtlgen::RtlDebugSignal> error_rtl_debug_signals;
    std::vector<rtlgen::RtlDebugSignal> error_signal_debug_signals;
    std::string error;

    bool ok() const { return error.empty(); }
};

PipelineResult compile(const PipelineConfig& config);

} // namespace pred::pipelinev2
