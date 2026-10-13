# Pipeline regression fixtures

Each `*.logic.cpp` is a complete synthesizable function with top `hls_main` and
explicit global input/output port pragmas. Use filenames that describe the
logic or coverage rather than a generic pipeline name.

`arithmetic_mux_lookup.logic.cpp` retains short paths, signed multiplication,
dynamic lookup, multidimensional array copies and constant outputs, and adds:

- Four serial 32-bit mixing rounds with add/subtract, variable logical shifts,
  multiplication, rotations and data-dependent muxes. Every round consumes the
  preceding result, keeping the optimized graph deep enough for the 1–8 sweep.
- Late merges of that result with fresh inputs, earlier arithmetic/lookup values
  and array reads, plus arithmetic right shift and signed/unsigned comparisons.
- Static slicing, bounded dynamic slice reads/writes, concatenation, repetition,
  logical operators, and power-of-two division/modulo lowering.
- OR/AND reductions over four-bit slices to exercise both outcomes under random
  inputs, and parity over the entire 32-bit result.

With the default cost model, the enriched case's longest-stage estimate is 36
at N=1 and 8 at N=8; all nine computation stages at N=8 carry logic. These are
relative scheduling costs, not physical timing measurements.

`regression.sh` discovers these sources recursively and checks every internal
register count N=1 through N=8 with the native backend. The continuous Verilator
driver launches changing inputs on each external input-register edge and checks
external output-register samples N+1 cycles later, ignoring filling and draining
the final transactions. No CIRCT pipeline sweep is run.

From `rtlzz/`, run one configuration with:

```bash
python3 scripts/differential_rtl.py \
  testv2/fixtures/pipeline_misc/arithmetic_mux_lookup.logic.cpp \
  --top hls_main --cases 100 --latency 3
```

`bash scripts/pipeline_regression.sh` additionally checks N=0, pipeline API tests,
module-body simulation, and the main combinational fixtures at N=2.
