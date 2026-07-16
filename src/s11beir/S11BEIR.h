#pragma once

#include "backend/beir.hpp"
#include "debug/RTLZZException.h"
#include "s10predicate/S10Predicate.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s11beir {

struct BEIROptions {
    bool optimize = false;
    bool debug_print = false;
};

struct BEIRWarning {
    ErrorContext context;
    std::string message;
};

struct BEIRError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct BEIRSummary {
    std::string function_name;
    int input_ports = 0;
    int output_ports = 0;
    int s10_values = 0;
    int beir_signals = 0;
    int definitions = 0;
    int lowered_lookups = 0;
    int generated_lookup_nodes = 0;
};

struct BEIRResult {
    std::optional<beir::Program> program;
    std::optional<BEIRError> error;
    std::vector<BEIRWarning> warnings;
    std::vector<BEIRSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

BEIRResult buildBEIR(const s10predicate::S10PredicateProgram& program,
                     const BEIROptions& options = {});

beir::Program buildBEIROrThrow(const s10predicate::S10PredicateProgram& program,
                               const BEIROptions& options = {});

} // namespace pred::s11beir
