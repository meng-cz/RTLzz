#pragma once

#include "debug/RTLZZException.h"
#include "s4cfg/S4CFG.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s5unroll {

struct UnrollWarning {
    ErrorContext context;
    std::string message;
};

struct UnrollError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct UnrollOptions {
    bool debug_print = false;
    int max_iterations_per_loop = 1024;
    int max_total_cloned_blocks = 100000;
    bool allow_while = true;
    bool allow_loop_control = true;
};

struct UnrollSummary {
    std::string function_name;
    s4cfg::LoopRegionId loop_id = -1;
    s4cfg::LoopConditionKind condition_kind = s4cfg::LoopConditionKind::PreTest;
    int iterations = 0;
    int cloned_blocks = 0;
};

struct UnrollResult {
    std::optional<s4cfg::CFGProgram> program;
    std::optional<UnrollError> error;
    std::vector<UnrollWarning> warnings;
    std::vector<UnrollSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

// S5 consumes and produces S4 CFGs with the post-S3 symbol invariant:
// every SymbolId is unique within its FunctionCFG. Any cloned declarations or
// synthetic variables introduced here must allocate fresh function-local ids;
// S5 must not depend on lexical scope metadata for variable identity.
UnrollResult unrollCFGProgram(
    const s4cfg::CFGProgram& program,
    const UnrollOptions& options = {});

s4cfg::CFGProgram unrollCFGProgramOrThrow(
    const s4cfg::CFGProgram& program,
    const UnrollOptions& options = {});

std::string debugPrint(const s4cfg::CFGProgram& program,
                       const std::vector<UnrollSummary>& summaries);

} // namespace pred::s5unroll
