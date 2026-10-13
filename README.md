# RTLzz

Restricted VUL-style C++ to synthesizable SystemVerilog through the V2 pipeline.

## Directories

- `src/`: V2 compiler stages, shared debug support, and BEIR/SystemVerilog backend.
- `main/`: `predicate-expand` command line entry point.
- `testv2/`: stage unit/integration tests.
- `testv2/unit_fixtures/`: stage-specific positive/negative fixtures, excluded from RTL differential regression.
- `testv2/fixtures/`: combinational regression sources; `pipeline_misc/` holds named pipeline coverage cases.
- `scripts/`: V2 differential RTL test harness.
- `third_party/vulsim/vullib/`: bundled VUL headers used by parsing and tests.

## Build

```bash
cmake -S . -B build
cmake --build build --target predicate-expand -j2
```

`CMakeLists.txt` also discovers every direct `testv2/*.cpp` file and builds each
one as an executable under `build/testv2/`.

## Run

```bash
build/predicate-expand testv2/fixtures/int_misc.logic.cpp \
  --top hls_main \
  --vullib third_party/vulsim/vullib \
  --clang-arg -std=c++20 \
  --format rtl \
  -o /tmp/int_misc.sv
```

Supported output formats are:

- `rtl`: SystemVerilog emitted from V2 BEIR.
- `beir`: textual backend IR after V2 lowering and BEIR optimization.
- `portmeta`: JSON port metadata used by RTL differential testing.

## Test

```bash
cmake --build build -j2
python3 scripts/differential_rtl.py testv2/fixtures/int_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/flatten_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/controlflow_misc.logic.cpp --top hls_main --cases 100
python3 scripts/differential_rtl.py testv2/fixtures/inline_misc.logic.cpp --top hls_main --cases 100
```

Pass `--circt` to run the same C++/RTL differential test through the CIRCT
backend. `regression.sh` scans combinational fixtures with both native and CIRCT emitters,
then scans `testv2/fixtures/pipeline_misc/` with the native backend at every
internal register count from 1 through 8. Unit fixtures are kept in
`testv2/unit_fixtures/` and are only read by their stage tests. The summary counts
each source/backend/latency combination; any failure gives a nonzero exit status.
Run `bash regression.sh` or `source regression.sh` from this directory. The
differential harness resolves actual RTL port declarations, including CIRCT
keyword renaming, while retaining the original source names in the oracle.

Use `--release` (or `-r`) with RTL output to suppress all debug sidecar files,
including error snapshots. It takes precedence over `--rtl-debug-file`, regardless
of argument order, and requires `--format rtl` (the default). Diagnostics still
appear on stderr. Without this option, debug output behavior is unchanged.

Backend structural optimization is enabled by default. `--beopt no-mux` disables
proven-exclusive mux parallelization; `--beopt no-balance` disables associative
tree balancing; `--beopt no-bit-updates` disables static bit-range update
coalescing; `--beopt no-boolean` disables Boolean control normalization.
`--beopt none` disables them along with the existing passes.
Mux rewriting retains the default branch and requires pairwise proven exclusive
conditions (at most 8 branches). It emits one ordered BEIR `Case` node whose
operands alternate condition and value, followed by the default value; RTL uses
an `always_comb case (1'b1)` block. Tree balancing only expands single-user,
equal-width unsigned AND/OR/XOR or modular-add nodes, and only rewrites when the
estimated arrival depth improves. Predicate sinking and scalar cleanup iterate
up to 4 rounds by default; the C++ `beir::opt::Options` exposes these bounds.
Static `WriteSlice` chains are combined into one flat composition when their
intermediate values have a single live user. Overlapping writes preserve
last-write-wins behavior, uncovered ranges come from the original value, and
the pass is bounded to 32 updates and 64 composed pieces by default.
Boolean control normalization rewrites side-effect-free one-bit `Ite` nodes
created by short-circuit and Boolean phi lowering into AND/OR/NOT form. It then
flattens and balances Boolean trees and applies constant, idempotence,
complement, absorption, and one-complement consensus identities. General
priority muxes whose branches cannot be represented by these identities remain
as `Ite` nodes.

## Automatic pipelining (native backend)

All public types, pipeline fields and API declarations are defined in
`src/rtlzz.hpp`; `src/rtlzz.cpp` implements the facade and converts public options
to internal configuration. Link the existing `predicate-expand-objects` library.

Configure pipelining through `rtlzz::CompileOptions`. No source pragma
or explicit registers are required in the C++ function:

```cpp
rtlzz::CompileOptions options;
// Set source_codelines, top_function, vullib_dir as usual.
options.register_stages = 2;
options.clock_name = "clk";
options.operation_delay_weights[rtlzz::PipelineOperation::Multiply] = 4.0;
options.operation_delay_weights[rtlzz::PipelineOperation::Lookup] = 2.0;
options.timing_relaxation_ratio = 0.05;
auto result = rtlzz::compileToRtl(options);
if (result.ok() && result.pipeline_report) {
    auto bits = result.pipeline_report->register_bits;
    auto delay = result.pipeline_report->final_delay;
}
```

`register_stages = N` inserts **N internal register boundaries**, splitting the
optimized combinational graph into N+1 computation stages. Input/output data
ports remain wires: external input-register Q and output-register D are the
caller's responsibility. An input launched by the external register at edge k
reaches the output wire after edge k+N and the last combinational stage settles;
the external output register captures it at edge k+N+1. With N=0, the existing
combinational output and port metadata are unchanged. With N>0, the module adds
a single rising-edge clock input. All outputs share the same transaction delay.
There is no valid/ready, stall, or reset; ignore output during initial filling.

Module-body generation also supports pipelining. Add `{ "clk", "external_clk" }`
(or the configured clock name) to `rtl_port_bindings` alongside the existing data
bindings. It must reference the same clock used by the external boundary registers.

The CLI exposes the basic configuration:

```bash
build/predicate-expand testv2/fixtures/pipeline_misc/arithmetic_mux_lookup.logic.cpp \
  --top hls_main --vullib third_party/vulsim/vullib \
  --latency 2 --clock-name clk --format rtl -o /tmp/pipeline.sv
```

Delay weights use the public `PipelineOperation` enum in `src/rtlzz.hpp`;
unconfigured operations cost 1, Assign costs 0, and Multiply costs 3. PortRead
is always boundary wiring with cost 0. The weights are finite non-negative
relative costs, **not nanoseconds or operation cycle counts**. Constant-only
subgraphs require no registers and contribute no transaction-path delay.
Individual operations are indivisible in this version.

Scheduling first searches a feasible timing baseline T₀, then moves operations
to reduce register storage while keeping the longest estimated stage at most
T₀ × (1 + timing_relaxation_ratio). The default allowance is 5%; set it to 0 for
no timing regression against the baseline. Deterministic local search minimizes
actual shared delay-chain register bits, including conditions and array data.
It does not guarantee globally optimal placement or physical timing closure.
Combinational area and external registers are excluded from the area objective.

`CompileResult::pipeline_report` returns baseline/allowed/final delays, per-stage
costs, register count, and register bits before/after area optimization. RTL text
debug includes this report; `--format beir` prints stages, register D connections
and the materialized graph. In pipeline mode `portmeta` adds a separate
`pipeline` object containing `register_stages`, `external_capture_cycles`, and
`clock_name`; the clock is not added to the C++ data-port list.

CIRCT does not support this sequential lowering yet; combining `--circt` with a
positive `--latency` reports an error. The host vulsim bridge keeps its existing
combinational default until it explicitly passes pipeline configuration.

Run the pipeline tests from this directory:

```bash
bash scripts/pipeline_regression.sh
```

The differential harness's `--latency N` sends changing inputs continuously in a
single Verilator simulation, models input Q updates after each edge and output D
captures before the next edge, skips filling, and drains the pipeline. It checks
N+1 external cycles against the C++ oracle. Omitting `--latency` keeps the existing
single-case combinational harness.
