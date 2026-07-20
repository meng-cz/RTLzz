#pragma once

#include "pipelinev2/PipelineV2.h"

#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rtlzz {

struct CompileOptions {
    // Target C++ source code, split into caller-owned lines or chunks.
    std::vector<std::string> source_codelines;
    // Debug/source filename used when source_codelines are parsed from memory.
    // This name is propagated into clang diagnostics and RTL/BEIR debug locs.
    // Empty preserves the historical default: rtlzz_input.logic.cpp.
    std::string source_name;
    // Directory containing required VUL headers such as fixint.hpp. When set,
    // this is translated into a clang include argument: -I<vullib_dir>.
    std::string vullib_dir;
    // Function name or wildcard pattern selecting the top function.
    std::string top_function;
    // Maximum static loop iterations accepted by the unroll stage.
    int unroll_limit = 1024;
    // Extra clang arguments passed through after rtlzz-managed arguments.
    std::vector<std::string> clang_args;
    // Additional include directories translated into clang -I arguments.
    std::vector<std::string> include_dirs;
    // BEIR optimization options. Empty means the default optimization pipeline;
    // use {"none"} to disable all BEIR optimization passes.
    std::vector<std::string> beopt_args;
};

struct CompileResult {
    std::vector<std::string> output_codelines;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

namespace detail {

enum class OutputKind {
    PortMetadata,
    Beir,
    Rtl,
};

inline const char* outputKindName(OutputKind kind) {
    switch (kind) {
    case OutputKind::PortMetadata:
        return "portmeta";
    case OutputKind::Beir:
        return "beir";
    case OutputKind::Rtl:
        return "rtl";
    }
    return "unknown";
}

inline std::vector<std::string> buildClangArgs(const CompileOptions& options) {
    std::vector<std::string> args = options.clang_args;
    if (!options.vullib_dir.empty()) {
        args.push_back("-I" + options.vullib_dir);
    }
    for (const auto& include_dir : options.include_dirs) {
        if (!include_dir.empty()) args.push_back("-I" + include_dir);
    }
    return args;
}

inline std::string joinCodeLines(const std::vector<std::string>& codelines) {
    std::ostringstream os;
    for (const auto& line : codelines) {
        os << line;
        if (line.empty() || line.back() != '\n') os << '\n';
    }
    return os.str();
}

inline std::vector<std::string> splitCodeLines(const std::string& output) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < output.size()) {
        std::size_t end = output.find('\n', begin);
        if (end == std::string::npos) {
            lines.push_back(output.substr(begin));
            break;
        }
        lines.push_back(output.substr(begin, end - begin + 1));
        begin = end + 1;
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

inline CompileResult compileSource(const CompileOptions& options, OutputKind output_kind) {
    if (options.source_codelines.empty()) return {{}, "source_codelines must not be empty"};
    if (options.top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {{}, "top_function must not be empty"};
    }

    pred::pipelinev2::PipelineConfig config;
    config.source_name = options.source_name.empty()
        ? "rtlzz_input.logic.cpp"
        : options.source_name;
    config.source_text = joinCodeLines(options.source_codelines);
    config.top_function = options.top_function;
    config.clang_args = buildClangArgs(options);
    config.unroll_limit = options.unroll_limit;
    config.beopt_args = options.beopt_args;
    if (output_kind == OutputKind::Beir) {
        config.output_kind = pred::pipelinev2::OutputKind::Beir;
    } else if (output_kind == OutputKind::PortMetadata) {
        config.output_kind = pred::pipelinev2::OutputKind::PortMetadata;
    } else {
        config.output_kind = pred::pipelinev2::OutputKind::Rtl;
    }

    auto result = pred::pipelinev2::compile(config);
    if (!result.ok()) {
        return {{}, result.error};
    }
    return {splitCodeLines(result.output_text), ""};
}

} // namespace detail

inline CompileResult compileToRtl(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::Rtl);
}

inline CompileResult compileToPortMetadata(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::PortMetadata);
}

inline CompileResult compileToBeir(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::Beir);
}

} // namespace rtlzz
