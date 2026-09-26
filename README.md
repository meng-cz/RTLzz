# RTLzz

Restricted VUL-style C++ to synthesizable SystemVerilog through the V2 pipeline.

## Directories

- `src/`: V2 compiler stages, shared debug support, and BEIR/SystemVerilog backend.
- `main/`: `predicate-expand` command line entry point.
- `testv2/`: stage unit/integration tests and V2 fixture programs.
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
backend. `testv2/regression.sh` runs every fixture with both the native and
CIRCT emitters and records the backend in its per-fixture logs and `summary.tsv`.

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

Run `build/testv2/beopt-structure-test` for structural and BEIR equivalence checks,
and `scripts/differential_rtl.py testv2/fixtures/backend_structure.logic.cpp
--top hls_main --cases 256` for RTL differential validation.
Run `build/testv2/beopt-bit-updates-test` and use
`testv2/fixtures/bit_update_coalescing.logic.cpp` for bit-update structural and
RTL differential validation.
Run `build/testv2/beopt-boolean-test` for exhaustive small-network equivalence
checks of Boolean normalization.
Run `build/testv2/beir-case-test` for Case value-fact, constant-folding, width,
algebraic, text-IR, and RTL-emission checks.

Boolean implication search scales its depth and work allowance with actual DAG
depth; shared constant Eq/Ne decoders use separate typed selector facts.
Boolean normalization runs immediately before and after predicate sinking and
at the end of the final optimizer iteration (only the final call when sinking
is disabled). Exact output pairs `wen_NAME` / `wdata_NAME` declare a write-port
contract: data is observable only when its corresponding enable is true. Array
ports must have matching shapes and pair element by element; enables at other
indices never qualify a write. Predicate sinking models virtual masked observers
without emitting extra ports, and retains all other observable uses. See
`testv2/adaptive_control_analysis.md` for tests and resource semantics.
