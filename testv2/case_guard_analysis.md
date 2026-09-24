# Case guard simplification and prefix verification

## Implementation

`beopt_case_guards.hpp` runs after the generic BEOPT fixed point. It builds a
shared complemented-edge Boolean DAG, expands small constant decoders into
exact bit constraints, and simplifies each conjunction under its other retained
facts. A deleted condition is never used to prove its own redundancy. Complete
bit decoders are emitted as equality comparisons again.

For ordered Case inputs, arm `i` is qualified by the complement of the balanced
OR of the original preceding conditions. This guarantees mutual exclusion and
preserves first-hit semantics. Redundant exclusions are then removed. Remaining
negative path facts are also emitted as balanced OR/NOT networks, with aligned
power-of-two blocks shared between prefixes. The final cleanup does not rerun
associative Boolean restructuring.

This is a bounded, semantics-preserving structural simplifier, not an assertion
of globally minimal Boolean expressions. Non-decoder arithmetic remains opaque;
unproved exclusions are retained. The existing disabled predicate-sinking pass
is not enabled by this change.

## Unit and differential checks

- Five backend executables passed: `beopt-structure-test`,
  `beopt-predicate-test`, `beopt-bit-updates-test`, `beopt-boolean-test`,
  `beir-case-test`.
- Exhaustive checks of all 256 state encodings and both values of an earlier
  data-dependent branch: a later different-state guard depends only on state
  and is emitted as a single equality comparator.
- 128-arm overlapping priority Case: first-hit behavior and at-most-one active
  condition verified on 256 input patterns; last-arm condition depth <= 10.
- 32 deterministic random Boolean Case graphs, each exhaustively checked on
  all 32 assignments to five inputs: output equivalence and pairwise exclusivity.
- `backend_structure.logic.cpp`: 1000 native C++/RTL differential cases passed.

## FPUArithmetic generation (2026-09-24)

Command, with a 600-second process timeout:

```sh
timeout 600s build/vulrtlgen -t /tmp/vulcore/FPUArithmetic.hpp \
  -o /tmp/fpu-case-guards-final
```

Generation completed successfully in approximately 165 seconds. The output has
63 `unique case (1'b1)` statements. The FMA_SUM1 -> FMA_SUM2 state update is:

```systemverilog
assign __beopt_width_9060 = (rdata_state == 8'h1b);
// Inside the state Case:
__beopt_width_9060: wdata_state____idx_1_v69 = 8'h1c;
```

The same decoder controls `wdata_fma_sum_carry_q___v34`,
`wdata_fma_sum_seg_64___v29`, and `wdata_fma_sum_seg_32___v29`.

Compared with the user-provided `build/rtlout/top.sv` snapshot, whose branch was
`__beopt_width_41414`, this particular condition cone changes as follows:

| Property | Original artifact | New generation |
|---|---|---|
| Dependencies | State, data registers, handshake/control signals | `rdata_state` only |
| Reachable assign nodes | 630 | 1 |
| Expression graph depth | 124 | 1 |

Counts follow assign dependencies, stop at `rdata_*` boundaries, and give aliases
zero depth. These are RTL graph measurements, not synthesis timing or whole-FPU
functional verification. The original artifact is a saved user output, not a
fresh baseline compilation. Generated output and logs remain under `/tmp`;
`build/rtlout` was not overwritten.
