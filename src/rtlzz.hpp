#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rtlzz {

// Public operation keys; independent of the backend's IR enums. Weights are
// relative combinational costs, not physical time or operator cycle latency.
enum class PipelineOperation {
    Assign, Add, Subtract, Multiply, Divide, Modulo, AddCarry,
    BitAnd, BitOr, BitXor, LogicalAnd, LogicalOr, LogicalNot, BitNot, Negate,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    ShiftLeft, ShiftRight, Mux, Case, Lookup, ArrayAccess, Cast,
    ZeroExtend, SignExtend, Truncate, Slice, BitSelect, WriteSlice, WriteBit,
    Concat, Repeat, ReduceOr, ReduceAnd, ReduceXor,
    DynamicBitSelect, DynamicSlice, DynamicWriteSlice, DynamicWriteBit,
    Aggregate, Call,
};

// Scheduling estimates and actual internal-register storage, excluding external
// boundary registers and combinational area. Available even without RTL debug.
struct PipelineReport {
    // Longest stage cost before register-area optimization.
    double baseline_delay = 0;
    // Hard cap: baseline_delay * (1 + timing_relaxation_ratio).
    double allowed_delay = 0;
    // Longest stage cost in the selected schedule.
    double final_delay = 0;
    // Costs indexed from stage 0 through stage register_stages.
    std::vector<double> stage_delays;
    // Internal register storage before / after register-area optimization.
    std::uint64_t baseline_register_bits = 0;
    std::uint64_t register_bits = 0;
    // Distinct register signals; an array signal counts once, but all its bits
    // are included in register_bits.
    std::size_t register_count = 0;
    // Computed reset output values, keyed by source output-port name. Each
    // scalar uses ceil(width/64) little-endian limbs. Array elements are
    // concatenated in row-major order, with each element independently padded.
    std::map<std::string, std::vector<std::uint64_t>> output_reset_values;
};

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

    bool valid() const;
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
    // Maximum number of scalar symbols produced by S7 flatten. Zero means no
    // artificial limit; memory and the internal SymbolId range remain bounds.
    std::size_t max_leaf_symbols = 0;
    // Extra clang arguments passed through after rtlzz-managed arguments.
    std::vector<std::string> clang_args;
    // Additional include directories translated into clang -I arguments.
    std::vector<std::string> include_dirs;
    // BEIR optimization options. Empty means the default optimization pipeline;
    // use {"none"} to disable all BEIR optimization passes.
    std::vector<std::string> beopt_args;
    // Emit RTL through the external CIRCT HW/Comb bridge.  This is opt-in;
    // failures are reported rather than falling back to the native emitter.
    // Positive register_stages is supported only by the native backend; using
    // CIRCT with a positive stage count reports a compilation error.
    bool use_circt = false;
    // Optional progress hook used by embedding tools. It reports frontend
    // stages and backend optimization iterations; standalone calls are silent.
    std::function<void(const std::string&)> progress_callback;
    // Optional RTL debug data. None avoids building API debug payloads.
    RtlDebugMode rtl_debug = RtlDebugMode::None;
    // Return declarations and statements for an existing module instead of a
    // complete module. Bind RTLZZ port names to containing-module signals.
    bool rtl_module_body = false;
    std::vector<std::pair<std::string, std::string>> rtl_port_bindings;
    // Automatic pipeline: N internal register boundaries, N+1 computation
    // stages. Zero preserves combinational RTL. Inputs and outputs remain wire
    // ports connected to external input-register Q / output-register D.
    // Input launched at edge k settles at output after edge k+N; an external
    // output register captures it at k+N+1. No valid/ready or stalling; reset is opt-in below;
    // Without reset, ignore outputs until the pipeline fills. Operations are indivisible.
    // Must be non-negative.
    int register_stages = 0;
    // Rising-edge clock port added only when register_stages > 0. Must be a
    // non-reserved RTL identifier without data-port/signal collisions. In
    // rtl_module_body mode, include {clock_name, external_clock} in bindings.
    std::string clock_name = "clk";
    // Override relative combinational costs (finite, non-negative), not physical
    // time or operation cycle counts. Unspecified operations cost 1, Assign 0,
    // Multiply 3; input PortRead wiring always costs 0. Constant-only subgraphs
    // require no pipeline storage and contribute no transaction-path delay.
    std::map<PipelineOperation, double> operation_delay_weights;
    // First establish timing baseline T0, then reduce shared register bits under
    // Tmax <= T0 * (1 + ratio). Default allows 5% timing regression; zero allows
    // none. Must be finite and non-negative. This is a deterministic heuristic,
    // not a guarantee of global optimality or physical timing closure.
    double timing_relaxation_ratio = 0.05;
    // Opt in to reset-value evaluation after scheduling. Supply all input data
    // ports in the same limb layout as PipelineReport::output_reset_values.
    // Sizes must match the port shape; unused high padding bits must be zero.
    // Missing/unknown port names are rejected.
    // Reset-enabled internal registers use synchronous reset, held while active.
    // With register_stages=0 this only evaluates outputs and populates a report;
    // it adds no clock/reset ports. Undefined reset computations are errors.
    bool reset_enabled = false;
    std::string reset_name = "rstn";
    bool reset_active_low = true;
    std::map<std::string, std::vector<std::uint64_t>> input_reset_values;
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
    // Available for a pipeline or reset-evaluation compilation regardless of debug mode.
    std::optional<PipelineReport> pipeline_report;

    bool ok() const;
};

// API facade implemented in rtlzz.cpp, linked through predicate-expand-objects.
// Example (after setting source_codelines, top_function and include paths):
//   options.register_stages = 2;
//   options.operation_delay_weights[PipelineOperation::Multiply] = 4.0;
//   options.timing_relaxation_ratio = 0.05;
//   auto result = compileToRtl(options);
//   if (result.ok() && result.pipeline_report) {
//       auto bits = result.pipeline_report->register_bits;
//   }
CompileResult compileToRtl(CompileOptions options);
// Port metadata includes clock/cycle and optional reset information for positive stages;
// pipeline_report is not populated because scheduling is skipped for metadata.
CompileResult compileToPortMetadata(CompileOptions options);
// Positive stages include scheduling and register information in the BEIR text.
CompileResult compileToBeir(CompileOptions options);

} // namespace rtlzz
