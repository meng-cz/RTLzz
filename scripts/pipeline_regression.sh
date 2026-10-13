#!/usr/bin/env bash
# Run from rtlzz/ (same convention as regression.sh).
set -euo pipefail
cmake -S . -B build
cmake --build build --target predicate-expand pipeline-test -j2
build/testv2/pipeline-test build/pipeline_body_tb.sv
verilator --binary --timing -Wno-fatal --top-module body_testbench \
    --Mdir build/pipeline_body_obj build/pipeline_body_tb.sv
build/pipeline_body_obj/Vbody_testbench
for stages in 0 1 2 4 8; do
    python3 scripts/differential_rtl.py testv2/fixtures/pipeline_misc.logic.cpp \
        --top hls_main --cases 100 --latency "$stages"
done
for fixture in int_misc flatten_misc controlflow_misc inline_misc; do
    python3 scripts/differential_rtl.py "testv2/fixtures/$fixture.logic.cpp" \
        --top hls_main --cases 100 --latency 2
done
