#!/usr/bin/env bash

# A subshell also permits `source regression.sh` without changing caller state.
(
    set -u -o pipefail
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" || exit 1
    cmake -S . -B build || exit 1
    cmake --build build --target predicate-expand -j2 || exit 1

    passed=0
    failed=0

    run_case() {
        local file=$1
        shift
        echo "=== Running: $file $* ==="
        if python3 scripts/differential_rtl.py "$file" --top hls_main --cases 100 "$@"; then
            echo "[PASS] $file $*"
            passed=$((passed + 1))
        else
            echo "[FAIL] $file $*"
            failed=$((failed + 1))
        fi
        echo
    }

    # Unit fixtures live separately; pipeline fixtures have their own sweep.
    while IFS= read -r -d '' file; do
        run_case "$file"
        run_case "$file" --circt
    done < <(find testv2/fixtures -path testv2/fixtures/pipeline_misc -prune \
        -o -type f -name '*.logic.cpp' -print0 | sort -z)

    while IFS= read -r -d '' file; do
        for stages in {1..8}; do
            run_case "$file" --latency "$stages"
        done
    done < <(find testv2/fixtures/pipeline_misc -type f -name '*.logic.cpp' -print0 | sort -z)

    echo "=============================="
    echo "Passed: $passed"
    echo "Failed: $failed"
    echo "Total:  $((passed + failed))"
    ((failed == 0))
)
