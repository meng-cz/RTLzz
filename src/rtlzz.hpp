#pragma once

#include "pipelinev2/PipelineV2.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rtlzz {

enum class RtlDebugMode {
    None,
    Structured,
    Text,
};

struct SourceLoc {
    std::string file;
    int line = 0;
    int column = 0;
    int end_line = 0;
    int end_column = 0;

    bool valid() const {
        return !file.empty() || line > 0 || column > 0 ||
               end_line > 0 || end_column > 0;
    }
};

struct RtlDerivedSignalDebugInfo {
    std::uint64_t node_id = 0;
    std::string signal_name;
};

struct RtlSignalDebugInfo {
    std::uint64_t node_id = 0;
    std::string signal_name;
    int rtl_line = 0;
    std::string port_name;
    int port_element_index = -1;
    std::optional<SourceLoc> decl_loc;
    std::vector<SourceLoc> primary_locs;
    std::vector<SourceLoc> related_locs;
    std::vector<std::string> messages;
    std::vector<RtlDerivedSignalDebugInfo> derived_signals;
    std::vector<std::string> derived_names;
};

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
    // Optional RTL debug data. None avoids building API debug payloads.
    RtlDebugMode rtl_debug = RtlDebugMode::None;
};

struct CompileResult {
    std::vector<std::string> output_codelines;
    // Populated only when CompileOptions::rtl_debug == RtlDebugMode::Text.
    std::string debug_text;
    std::vector<std::string> debug_codelines;
    // Populated only when CompileOptions::rtl_debug == RtlDebugMode::Structured.
    std::vector<RtlSignalDebugInfo> debug_signals;
    // Populated on failed compilations when the pipeline has a debug snapshot.
    std::string error_debug_text;
    std::vector<std::string> error_debug_codelines;
    std::vector<RtlSignalDebugInfo> error_debug_signals;
    // Best-effort subset related to the failing source location or signal.
    std::string error_signal_debug_text;
    std::vector<std::string> error_signal_debug_codelines;
    std::vector<RtlSignalDebugInfo> error_signal_debug_signals;
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

inline bool hasLanguageStandardArg(const std::vector<std::string>& clang_args) {
    for (const auto& arg : clang_args) {
        if (arg == "-std" || arg == "--std" ||
            arg.rfind("-std=", 0) == 0 || arg.rfind("--std=", 0) == 0) {
            return true;
        }
    }
    return false;
}

inline std::vector<std::string> buildClangArgs(const CompileOptions& options) {
    std::vector<std::string> args = options.clang_args;
    if (!hasLanguageStandardArg(args)) {
        args.push_back("-std=c++20");
    }
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

inline SourceLoc convertLoc(const pred::DebugLoc& loc) {
    SourceLoc out;
    out.file = loc.file;
    out.line = loc.line;
    out.column = loc.column;
    out.end_line = loc.end_line;
    out.end_column = loc.end_column;
    return out;
}

inline std::optional<SourceLoc> convertLoc(const std::optional<pred::DebugLoc>& loc) {
    if (!loc || !loc->valid()) return std::nullopt;
    return convertLoc(*loc);
}

inline std::vector<SourceLoc> convertLocs(const std::vector<pred::DebugLoc>& locs) {
    std::vector<SourceLoc> out;
    out.reserve(locs.size());
    for (const auto& loc : locs) {
        if (loc.valid()) out.push_back(convertLoc(loc));
    }
    return out;
}

inline std::vector<RtlSignalDebugInfo> convertDebugSignals(
    const std::vector<pred::rtlgen::RtlDebugSignal>& signals) {
    std::vector<RtlSignalDebugInfo> out;
    out.reserve(signals.size());
    for (const auto& signal : signals) {
        RtlSignalDebugInfo converted;
        converted.node_id = signal.id;
        converted.signal_name = signal.signal_name;
        converted.rtl_line = signal.rtl_line;
        converted.port_name = signal.port_name;
        converted.port_element_index = signal.port_element_index;
        converted.decl_loc = convertLoc(signal.decl_loc);
        converted.primary_locs = convertLocs(signal.primary_locs);
        converted.related_locs = convertLocs(signal.related_locs);
        converted.messages = signal.messages;
        for (const auto& node : signal.derived_nodes) {
            converted.derived_signals.push_back(
                RtlDerivedSignalDebugInfo{node.id, node.name});
        }
        converted.derived_names = signal.derived_names;
        out.push_back(std::move(converted));
    }
    return out;
}

inline CompileResult compileSource(const CompileOptions& options, OutputKind output_kind) {
    if (options.source_codelines.empty()) return {{}, "", {}, {}, "", {}, {}, "", {}, {}, "source_codelines must not be empty"};
    if (options.top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {{}, "", {}, {}, "", {}, {}, "", {}, {}, "top_function must not be empty"};
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
    switch (options.rtl_debug) {
    case RtlDebugMode::None:
        config.rtl_debug_output = pred::pipelinev2::RtlDebugOutputKind::None;
        break;
    case RtlDebugMode::Structured:
        config.rtl_debug_output = pred::pipelinev2::RtlDebugOutputKind::Structured;
        break;
    case RtlDebugMode::Text:
        config.rtl_debug_output = pred::pipelinev2::RtlDebugOutputKind::Text;
        break;
    }
    if (output_kind == OutputKind::Beir) {
        config.output_kind = pred::pipelinev2::OutputKind::Beir;
    } else if (output_kind == OutputKind::PortMetadata) {
        config.output_kind = pred::pipelinev2::OutputKind::PortMetadata;
    } else {
        config.output_kind = pred::pipelinev2::OutputKind::Rtl;
    }

    auto result = pred::pipelinev2::compile(config);
    if (!result.ok()) {
        CompileResult out;
        out.error = result.error;
        out.error_debug_text = result.error_debug_text;
        out.error_debug_codelines = result.error_debug_text.empty()
            ? std::vector<std::string>{}
            : splitCodeLines(result.error_debug_text);
        out.error_debug_signals = convertDebugSignals(result.error_rtl_debug_signals);
        out.error_signal_debug_text = result.error_signal_debug_text;
        out.error_signal_debug_codelines = result.error_signal_debug_text.empty()
            ? std::vector<std::string>{}
            : splitCodeLines(result.error_signal_debug_text);
        out.error_signal_debug_signals = convertDebugSignals(result.error_signal_debug_signals);
        return out;
    }
    CompileResult out;
    out.output_codelines = splitCodeLines(result.output_text);
    out.debug_text = result.rtl_debug_text;
    out.debug_codelines = result.rtl_debug_text.empty()
        ? std::vector<std::string>{}
        : splitCodeLines(result.rtl_debug_text);
    out.debug_signals = convertDebugSignals(result.rtl_debug_signals);
    return out;
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
