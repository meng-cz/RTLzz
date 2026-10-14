#pragma once

#include "rtlzz.hpp"
#include "backend/beir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pred::pipeline {

// Internal scheduling configuration; the public API exposes its fields
// directly in CompileOptions. rtlzz.cpp translates between the two layers.
struct Options {
    int register_stages = 0;
    std::string clock_name = "clk";
    std::map<rtlzz::PipelineOperation, double> operation_delay_weights;
    double timing_relaxation_ratio = 0.05;
    bool reset_enabled = false;
    std::string reset_name = "rstn";
    bool reset_active_low = true;
    std::map<std::string, std::vector<std::uint64_t>> input_reset_values;
};

using Report = rtlzz::PipelineReport;

struct Register {
    beir::NodeId node;
    beir::NodeId source;
    int stage;
    std::vector<std::uint64_t> reset_values;
};

// Sequential semantics live here, never in combinational BEOPT. Drivers on
// register nodes in rtl_program describe D inputs; the emitter MUST use the
// registers table to emit nonblocking clocked assignments for these nodes.
struct Program {
    beir::Program combinational;
    beir::Program rtl_program;
    Options options;
    std::vector<int> stages; // Original NodeId -> computation stage.
    std::vector<Register> registers;
    Report report;
};

void initializeResetValues(Program& program);
void validateOptions(const Options& options);
Program build(const beir::Program& program, const Options& options);
// Check stage, source identity, type, and register-storage invariants.
void validateProgram(const Program& program);
std::string emitReport(const Program& program);
std::string emitText(const Program& program);

} // namespace pred::pipeline
