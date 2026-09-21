# Nested branch-decision lowering analysis

`fixtures/branch_decision.logic.cpp` models a software-style redirect decision:
an outer `legal && (jal || jalr || branch)` condition, a local Boolean updated by
a six-way `if/else if` chain, signed and unsigned 64-bit comparisons, and a
JALR/PC target selection.

Run the functional check with:

```bash
python3 scripts/differential_rtl.py \
  testv2/fixtures/branch_decision.logic.cpp --top hls_main \
  --build-dir <build> --cases 1000 --input-range branch_op=0:7
```

The generated logic is functionally correct, including signed LT/GE, unsigned
LTU/GEU, JALR low-bit clearing, the outer invalid defaults, and all branch-op
values.

With the predicate-analysis limits removed, the six branch comparisons are
calculated in parallel and the serial `next_taken_v*` Ite chain is replaced by
one native ordered Case node with an explicit default. The optimized BEIR
carries both the `flattened proven-exclusive mux chain into case` and
Boolean-normalization annotations, and RTL emits `always_comb case (1'b1)`.
The Boolean normalization pass also converts the side-effect-free one-bit
short-circuit/phi Ite nodes to AND/OR/NOT. The resulting `control_valid` and
`taken` cones contain no Ite nodes; disabling only this pass leaves both at Ite
depth 4, while the un-parallelized `taken` chain started at depth 12. The
branch-result selection itself is one Case level. The two remaining Ite
levels in `target` are 64-bit data selections and are outside this one-bit
Boolean pass (depth 4 with Boolean normalization disabled).

S10 represents each `else if` condition as a full accumulated path guard.
Proving the later guards mutually exclusive requires tracking the outer control
predicates plus six `branch_op == constant` facts. The previous 8-atom budget
made `isExclusive()` conservatively return Unknown. The relation query now
retains every atom and enumerates assignments with dynamically sized storage,
so it proves the complete chain mutually exclusive.

The complete `taken` cone has a simple BEIR unit-delay depth of 11 after native
Case formation, compared with 17 for the previous mask/OR representation, 23
when Boolean normalization was disabled, and 27 before the structural
optimizations. This includes comparison generation and accumulated path guards;
the final result selection is one Case level.

Predicate proof is now exact for the constructed Boolean abstraction but can
take exponential time in the number of independent atoms. Context propagation
also retains every distinct path. There is no longer an arbitrary cutoff that
returns Unknown or drops context facts.
