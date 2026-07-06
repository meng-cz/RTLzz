#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/listjson_fixtures}"
TOP="${TOP:-hls_main}"
GENERATOR="${GENERATOR:-Ninja}"

usage() {
  cat <<'EOF'
Usage: scripts/generate_fixture_listjson.sh [options]

Generate listjson outputs for positive fixtures under tests/fixtures.

Options:
  --build-dir DIR   CMake build directory. Default: build
  --out-dir DIR     Output directory. Default: <build-dir>/listjson_fixtures
  --top NAME        Top function name. Default: hls_main
  --no-build        Do not configure/build predicate-expand if it is missing
  -h, --help        Show this help
EOF
}

DO_BUILD=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --top)
      TOP="$2"
      shift 2
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

cd "$ROOT"

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$ROOT/$BUILD_DIR"
fi
if [[ "$OUT_DIR" != /* ]]; then
  OUT_DIR="$ROOT/$OUT_DIR"
fi

EXE="$BUILD_DIR/predicate-expand"
if [[ ! -x "$EXE" && -x "$BUILD_DIR/predicate-expand.exe" ]]; then
  EXE="$BUILD_DIR/predicate-expand.exe"
fi

if [[ ! -x "$EXE" ]]; then
  if [[ "$DO_BUILD" -eq 0 ]]; then
    echo "predicate-expand not found under $BUILD_DIR" >&2
    exit 1
  fi
  cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --target predicate-expand --parallel
  EXE="$BUILD_DIR/predicate-expand"
  if [[ ! -x "$EXE" && -x "$BUILD_DIR/predicate-expand.exe" ]]; then
    EXE="$BUILD_DIR/predicate-expand.exe"
  fi
fi

if [[ ! -x "$EXE" ]]; then
  echo "predicate-expand was not built under $BUILD_DIR" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

shopt -s nullglob
fixtures=("$ROOT"/tests/fixtures/*.logic.cpp)
if [[ "${#fixtures[@]}" -eq 0 ]]; then
  echo "No positive fixtures found under tests/fixtures" >&2
  exit 1
fi

for fixture in "${fixtures[@]}"; do
  base="$(basename "$fixture")"
  name="${base%.logic.cpp}"
  out="$OUT_DIR/$name.listjson"
  echo "Generating $out"
  "$EXE" "$fixture" \
    --top "$TOP" \
    --format listjson \
    --unroll-limit 4096 \
    --vullib "$ROOT/third_party/vulsim/vullib" \
    --clang-arg "-I$ROOT" \
    --clang-arg "-std=c++20" \
    -o "$out"
done

echo "Generated ${#fixtures[@]} listjson files in $OUT_DIR"
