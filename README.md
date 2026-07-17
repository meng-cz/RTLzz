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
