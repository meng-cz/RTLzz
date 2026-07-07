#pragma once

#include "backend/beir.hpp"
#include "backend/beopt.hpp"
#include "backend/rtlgen.hpp"
#include "emitter/ListJsonEmitter.h"
#include "pipeline/Stages.h"
#include "predicate/OutputExpressionMap.h"
#include "predicate/PredicateVerifier.h"
#include "transform/Predicate.h"

#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rtlzz {

struct CompileOptions {
    // Target C++ source code, split into caller-owned lines or chunks.
    std::vector<std::string> source_codelines;
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
    ListJson,
    Beir,
    Rtl,
};

inline const char* outputKindName(OutputKind kind) {
    switch (kind) {
    case OutputKind::ListJson:
        return "listjson";
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

inline pred::PredicateProgram buildPredicateProgram(const pred::FunctionAST& function,
                                                    const pred::NormalizeResult& normalized,
                                                    const pred::SSAProgram& ssa) {
    auto program = pred::predicate(ssa);
    program.function_name = function.name;

    for (const auto& [name, type] : normalized.symbols) {
        program.symbols[name] = type;
    }
    program.param_directions = normalized.param_directions;
    for (const auto& param : function.params) {
        if (param.debug_loc.valid()) program.param_debug_locs[param.name] = param.debug_loc;
    }
    program.output_default_reasons = normalized.output_default_reasons;
    program.output_paired_controls = normalized.output_paired_controls;
    program.lookup_tables = normalized.lookup_tables;
    program.outputs = normalized.output_params;
    pred::buildOutputExpressionMap(program);
    return program;
}

inline CompileResult compileSource(const CompileOptions& options, OutputKind output_kind) {
    if (options.source_codelines.empty()) return {{}, "source_codelines must not be empty"};
    if (options.top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {{}, "top_function must not be empty"};
    }

    pred::pipeline::ParseConfig parse_config;
    parse_config.source_name = "rtlzz_input.logic.cpp";
    parse_config.source_text = joinCodeLines(options.source_codelines);
    parse_config.top_function = options.top_function;
    parse_config.clang_args = buildClangArgs(options);

    auto parsed = pred::pipeline::parseSource(parse_config);
    if (!parsed.error.empty()) return {{}, "parse: " + parsed.error};
    if (!parsed.function.has_value()) return {{}, "parse: failed to extract function"};

    pred::UnrollConfig unroll_config;
    unroll_config.max_iterations = options.unroll_limit;
    auto unrolled = pred::pipeline::unrollFunction(std::move(parsed.function.value()),
                                                   unroll_config);
    if (!unrolled.error.empty()) return {{}, "unroll: " + unrolled.error};
    if (!unrolled.function.has_value()) return {{}, "unroll: stage produced no function"};

    auto inlined = pred::pipeline::inlineHelpersAndLambdas(
        std::move(unrolled.function.value()));
    if (!inlined.error.empty()) return {{}, "inline: " + inlined.error};
    if (!inlined.function.has_value()) return {{}, "inline: stage produced no function"};

    const auto& lowered_function = inlined.function.value();
    auto normalized = pred::pipeline::normalizeFunction(lowered_function);
    if (!normalized.error.empty()) return {{}, "normalize/lower: " + normalized.error};

    auto cfg = pred::pipeline::buildControlFlow(normalized);
    if (!cfg.error.empty()) return {{}, "cfg: " + cfg.error};

    auto ssa = pred::pipeline::buildSSAForm(cfg, normalized);
    if (!ssa.error.empty()) return {{}, "ssa: " + ssa.error};

    auto program = buildPredicateProgram(lowered_function, normalized, ssa.program);
    auto verified = pred::verifyPredicateProgram(program);
    if (!verified.ok) return {{}, "predicate verification: " + verified.error};

    try {
        std::string output;
        switch (output_kind) {
        case OutputKind::ListJson:
            output = pred::emitListJson(program);
            break;
        case OutputKind::Beir: {
            auto beir_program = pred::beir::buildProgram(program, false);
            beir_program = pred::beir::opt::optimizeProgram(
                std::move(beir_program),
                pred::beir::opt::parseOptions(options.beopt_args));
            output = pred::beir::emitText(beir_program);
            break;
        }
        case OutputKind::Rtl: {
            auto beir_program = pred::beir::buildProgram(program, false);
            beir_program = pred::beir::opt::optimizeProgram(
                std::move(beir_program),
                pred::beir::opt::parseOptions(options.beopt_args));
            output = pred::rtlgen::emitSystemVerilog(beir_program);
            break;
        }
        }
        return {splitCodeLines(output), ""};
    } catch (const std::exception& ex) {
        return {{}, std::string(outputKindName(output_kind)) + " emission: " + ex.what()};
    }
    return {{}, "unknown output format"};
}

} // namespace detail

inline CompileResult compileToRtl(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::Rtl);
}

inline CompileResult compileToListJson(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::ListJson);
}

inline CompileResult compileToBeir(CompileOptions options) {
    return detail::compileSource(options, detail::OutputKind::Beir);
}

} // namespace rtlzz
