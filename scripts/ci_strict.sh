#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
GENERATOR="${GENERATOR:-Ninja}"
LLVM_DIR_ARG="${LLVM_DIR:+-DLLVM_DIR=${LLVM_DIR}}"
CUSTOMER_DIR="${CUSTOMER_DIR:-}"

cd "$ROOT"
rm -rf "$BUILD_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPREDICATE_EXPAND_REQUIRE_LIBCLANG=ON \
  ${LLVM_DIR_ARG}

cmake --build "$BUILD_DIR" --parallel

test -x "$BUILD_DIR/predicate-expand" || {
  test -x "$BUILD_DIR/predicate-expand.exe" || {
    echo "Strict CI expected predicate-expand to be built" >&2
    exit 1
  }
}
test -x "$BUILD_DIR/predicate-expand-e2e-tests" || {
  test -x "$BUILD_DIR/predicate-expand-e2e-tests.exe" || {
    echo "Strict CI expected predicate-expand-e2e-tests to be built" >&2
    exit 1
  }
}

if command -v pwsh >/dev/null 2>&1; then
  PS_BIN=pwsh
elif command -v powershell >/dev/null 2>&1; then
  PS_BIN=powershell
elif command -v powershell.exe >/dev/null 2>&1; then
  PS_BIN=powershell.exe
else
  echo "Strict CI requires PowerShell to run repository verification scripts" >&2
  exit 1
fi

"$PS_BIN" -ExecutionPolicy Bypass -File "$ROOT/scripts/check_no_unsafe_lowering.ps1"
ctest --test-dir "$BUILD_DIR" --output-on-failure
if [[ -n "$CUSTOMER_DIR" ]]; then
  "$PS_BIN" -ExecutionPolicy Bypass -File "$ROOT/scripts/test_customer_files.ps1" -BuildDir "$BUILD_DIR" -CustomerDir "$CUSTOMER_DIR"
else
  "$PS_BIN" -ExecutionPolicy Bypass -File "$ROOT/scripts/test_customer_files.ps1" -BuildDir "$BUILD_DIR"
fi
"$PS_BIN" -ExecutionPolicy Bypass -File "$ROOT/scripts/test_real_vul_outputs.ps1" -BuildDir "$BUILD_DIR" -OutputDir "$BUILD_DIR/generic_fixtures"

echo "Strict CI passed."
