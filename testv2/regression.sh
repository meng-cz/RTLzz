#!/usr/bin/env bash

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/testv2/fixtures"
BUILD_DIR="${RTLZZ_REGRESSION_BUILD_DIR:-/tmp/rtlzz-regression-build}"
LOG_DIR="${RTLZZ_REGRESSION_LOG_DIR:-$ROOT_DIR/rtl_test_outputs_regression}"
CASES="${RTLZZ_REGRESSION_CASES:-100}"
SEED="${RTLZZ_REGRESSION_SEED:-20260909}"

usage() {
    cat <<'EOF'
Usage: testv2/regression.sh [--cases N] [--seed N] [--build-dir DIR] [--log-dir DIR]

Runs every testv2/fixtures/**/*.logic.cpp through the C++/RTL differential
harness.  Files named illegal_* or uninitialized_* are expected-negative tests:
rejection is reported as XFAIL, while unexpectedly accepting one is XPASS.

Environment equivalents:
  RTLZZ_REGRESSION_CASES
  RTLZZ_REGRESSION_SEED
  RTLZZ_REGRESSION_BUILD_DIR
  RTLZZ_REGRESSION_LOG_DIR
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --cases)
        CASES="${2:?missing value for --cases}"
        shift 2
        ;;
    --seed)
        SEED="${2:?missing value for --seed}"
        shift 2
        ;;
    --build-dir)
        BUILD_DIR="${2:?missing value for --build-dir}"
        shift 2
        ;;
    --log-dir)
        LOG_DIR="${2:?missing value for --log-dir}"
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if ! [[ "$CASES" =~ ^[1-9][0-9]*$ ]]; then
    echo "--cases must be a positive decimal integer: $CASES" >&2
    exit 2
fi
if ! [[ "$SEED" =~ ^[0-9]+$ ]]; then
    echo "--seed must be a non-negative decimal integer: $SEED" >&2
    exit 2
fi

mkdir -p "$LOG_DIR"
SUMMARY_FILE="$LOG_DIR/summary.tsv"
: >"$SUMMARY_FILE"

echo "Configuring RTLzz regression build: $BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" >"$LOG_DIR/configure.log" 2>&1; then
    echo "CMake configure failed; see $LOG_DIR/configure.log" >&2
    exit 1
fi
if ! cmake --build "$BUILD_DIR" --target predicate-expand -j2 \
    >"$LOG_DIR/build.log" 2>&1; then
    echo "predicate-expand build failed; see $LOG_DIR/build.log" >&2
    exit 1
fi

PREDICATE="$BUILD_DIR/predicate-expand"
if [[ ! -x "$PREDICATE" ]]; then
    for candidate in Debug Release RelWithDebInfo MinSizeRel; do
        if [[ -x "$BUILD_DIR/$candidate/predicate-expand" ]]; then
            PREDICATE="$BUILD_DIR/$candidate/predicate-expand"
            break
        fi
    done
fi
if [[ ! -x "$PREDICATE" ]]; then
    echo "predicate-expand executable not found under $BUILD_DIR" >&2
    exit 1
fi

mapfile -d '' FIXTURES < <(find "$FIXTURE_DIR" -type f -name '*.logic.cpp' -print0 | sort -z)
if [[ ${#FIXTURES[@]} -eq 0 ]]; then
    echo "no .logic.cpp fixtures found under $FIXTURE_DIR" >&2
    exit 1
fi

pass_count=0
xfail_count=0
fail_count=0
xpass_count=0

concise_reason() {
    local log="$1"
    local reason
    reason="$(rg -m1 \
        '^(Error:|Mismatch |.*CHECK failed:|.*error:|.*Error:|.*failed:|.*unsupported |.*Unsupported |.*Missing )' \
        "$log" 2>/dev/null || true)"
    if [[ -z "$reason" ]]; then
        reason="$(tail -n 1 "$log" 2>/dev/null || true)"
    fi
    if [[ -z "$reason" ]]; then
        reason="command failed without diagnostics"
    fi
    printf '%s' "$reason"
}

oracle_crash_reason() {
    local log="$1"
    local signal

    signal="$(sed -n 's/.*died with <Signals\.\(SIG[A-Z]*\):.*/\1/p' "$log" | head -n 1)"
    if [[ -z "$signal" ]]; then
        return 1
    fi

    printf 'C++ oracle crashed with %s before RTL comparison; random input likely violates a fixture input/index precondition' "$signal"
}

echo "Running ${#FIXTURES[@]} fixtures: cases=$CASES seed=$SEED"
for fixture in "${FIXTURES[@]}"; do
    relative="${fixture#"$FIXTURE_DIR"/}"
    safe_name="${relative//\//__}"
    log="$LOG_DIR/${safe_name}.log"
    base="$(basename "$fixture")"
    expected_failure=0
    if [[ "$base" == illegal_* || "$base" == uninitialized_* ]]; then
        expected_failure=1
    fi

    if python3 "$ROOT_DIR/scripts/differential_rtl.py" \
        "$fixture" \
        --top hls_main \
        --build-dir "$BUILD_DIR" \
        --cases "$CASES" \
        --seed "$SEED" >"$log" 2>&1; then
        if [[ $expected_failure -eq 1 ]]; then
            status="XPASS"
            reason="expected an input-validation failure but RTL differential passed"
            xpass_count=$((xpass_count + 1))
        else
            status="PASS"
            reason="$(tail -n 1 "$log")"
            pass_count=$((pass_count + 1))
        fi
    else
        # A crashed direct-C++ oracle means no RTL result was compared.  Keep
        # that distinct from a compiler/lowering failure; in practice these
        # fixtures usually need a constrained dynamic-index input domain.
        oracle_reason="$(oracle_crash_reason "$log" || true)"

        # The differential harness captures predicate-expand stderr.  Re-run
        # the RTL front-end once to preserve its concise stage diagnostic when
        # failure happened before oracle/Verilator construction.
        if [[ -n "$oracle_reason" ]]; then
            reason="$oracle_reason"
        else
            diagnostic="$LOG_DIR/${safe_name}.diagnostic.log"
            "$PREDICATE" "$fixture" \
                --top hls_main \
                --vullib "$ROOT_DIR/third_party/vulsim/vullib" \
                --format rtl \
                --no-rtl-debug \
                -o /tmp/rtlzz_regression_diagnostic.sv \
                >"$diagnostic" 2>&1 || true
            if [[ -s "$diagnostic" ]]; then
                reason="$(concise_reason "$diagnostic")"
            else
                reason="$(concise_reason "$log")"
            fi
        fi
        if [[ $expected_failure -eq 1 ]]; then
            status="XFAIL"
            xfail_count=$((xfail_count + 1))
        else
            status="FAIL"
            fail_count=$((fail_count + 1))
        fi
    fi

    printf '%-5s %-55s %s\n' "$status" "$relative" "$reason"
    printf '%s\t%s\t%s\n' "$status" "$relative" "$reason" >>"$SUMMARY_FILE"
done

echo
echo "Summary: PASS=$pass_count XFAIL=$xfail_count FAIL=$fail_count XPASS=$xpass_count"
echo "Logs: $LOG_DIR"
echo "Machine-readable summary: $SUMMARY_FILE"

if [[ $fail_count -ne 0 || $xpass_count -ne 0 ]]; then
    exit 1
fi
