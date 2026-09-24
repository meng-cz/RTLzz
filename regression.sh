#!/usr/bin/env bash

cmake -S . -B build || exit 1
cmake --build build --target predicate-expand -j2 || exit 1

passed=0
failed=0

while read -r file; do
    echo "=== Running: $file ==="

    if python3 scripts/differential_rtl.py "$file" --top hls_main --cases 100; then
        echo "[PASS] $file"
        ((passed++))
    else
        echo "[FAIL] $file"
        ((failed++))
    fi

    echo
done < <(find testv2/fixtures/ -maxdepth 1 -type f -name '*.logic.cpp')

echo "=============================="
echo "Passed: $passed"
echo "Failed: $failed"
echo "Total:  $((passed + failed))"
