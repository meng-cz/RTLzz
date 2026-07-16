#!/usr/bin/env python3
"""Differentially test generated SystemVerilog RTL against the original C++.

The script:
  1. runs predicate-expand to produce pipelinev2 port metadata and RTL,
  2. builds a tiny C++ oracle harness that includes and calls the source top,
  3. builds a Verilator C++ testbench around the generated RTL,
  4. compares RTL outputs against the C++ oracle for random inputs.

It reuses differential_listjson.py harness helpers where the compact port
metadata schema intentionally matches the old listjson port subset.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import differential_listjson as lj


ROOT = Path(__file__).resolve().parents[1]


def sanitize_identifier(text: str) -> str:
    out = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in text)
    if not out or out[0].isdigit():
        out = "_" + out
    keywords = {
        "module", "endmodule", "input", "output", "inout", "logic", "wire",
        "assign", "always", "if", "else", "case", "endcase", "function",
        "endfunction", "signed", "unsigned", "localparam", "bit", "byte",
        "shortint", "int", "longint", "reg", "integer", "time", "local",
    }
    if out in keywords:
        out += "_"
    return out


def verilator_class_name(top: str) -> str:
    return "V" + sanitize_identifier(top)


def verilator_member_name(name: str) -> str:
    # Verilator escapes double underscores in public C++ member names.
    return sanitize_identifier(name).replace("__", "___05F")


def rtl_cpp_value_expr(expr: str, t: dict[str, Any]) -> str:
    width = lj.type_width(t)
    if width <= 64:
        return f"static_cast<unsigned long long>({expr})"
    return f"hex_from_words({expr}, {width})"


def rtl_cpp_assign_from_argv(expr: str, t: dict[str, Any], argv_index: int) -> str:
    width = lj.type_width(t)
    if width <= 64:
        return f"{expr} = std::strtoull(argv[{argv_index}], nullptr, 0);"
    words = (width + 31) // 32
    return (
        f"auto words_{argv_index} = parse_words(argv[{argv_index}], {width}); "
        f"for (int i = 0; i < {words}; ++i) {expr}[i] = words_{argv_index}[i];"
    )


def generate_verilator_harness(top: str, program: dict[str, Any], path: Path) -> list[str]:
    class_name = verilator_class_name(top)
    input_order: list[str] = []
    lines = [
        "#include <cstdlib>",
        "#include <cstdint>",
        "#include <iomanip>",
        "#include <iostream>",
        "#include <memory>",
        "#include <sstream>",
        "#include <string>",
        "#include <vector>",
        "#include <verilated.h>",
        f'#include "{class_name}.h"',
        "",
        "static std::vector<uint32_t> parse_words(const char* text, int width) {",
        "  int nwords = (width + 31) / 32;",
        "  std::vector<uint32_t> words(nwords, 0);",
        "  for (const char* p = text; *p; ++p) {",
        "    if (*p < '0' || *p > '9') continue;",
        "    uint64_t carry = static_cast<uint64_t>(*p - '0');",
        "    for (int i = 0; i < nwords; ++i) {",
        "      uint64_t next = static_cast<uint64_t>(words[i]) * 10ULL + carry;",
        "      words[i] = static_cast<uint32_t>(next & 0xffffffffULL);",
        "      carry = next >> 32;",
        "    }",
        "  }",
        "  int top_bits = width % 32;",
        "  if (top_bits != 0 && nwords > 0) words[nwords - 1] &= ((uint32_t{1} << top_bits) - 1U);",
        "  return words;",
        "}",
        "",
        "static std::string hex_from_words(const uint32_t* words, int width) {",
        "  int nwords = (width + 31) / 32;",
        "  std::vector<uint32_t> copy(words, words + nwords);",
        "  int top_bits = width % 32;",
        "  if (top_bits != 0 && nwords > 0) copy[nwords - 1] &= ((uint32_t{1} << top_bits) - 1U);",
        "  int top = nwords - 1;",
        "  while (top > 0 && copy[top] == 0) --top;",
        "  std::ostringstream os;",
        "  os << \"0x\" << std::hex << copy[top];",
        "  for (int i = top - 1; i >= 0; --i) {",
        "    os << std::setw(8) << std::setfill('0') << copy[i];",
        "  }",
        "  return os.str();",
        "}",
        "",
        "int main(int argc, char** argv) {",
        f"  auto top = std::make_unique<{class_name}>();",
    ]

    arg_index = 1
    argc_check_index = len(lines)
    for port in program["ports"]:
        if port["direction"] != "Input":
            continue
        is_array = bool(port["type"].get("is_array"))
        for flat, element in enumerate(port["element_symbols"]):
            input_order.append(element)
            if is_array:
                expr = f"top->{verilator_member_name(port['name'])}[{flat}]"
            else:
                expr = f"top->{verilator_member_name(port['name'])}"
            lines.append("  " + rtl_cpp_assign_from_argv(expr, port["type"], arg_index))
            arg_index += 1

    lines.insert(argc_check_index, f"  if (argc != {arg_index}) return 97;")
    lines.append("  top->eval();")

    for port in program["ports"]:
        if port["direction"] != "Output":
            continue
        is_array = bool(port["type"].get("is_array"))
        for flat, element in enumerate(port["element_symbols"]):
            if is_array:
                expr = f"top->{verilator_member_name(port['name'])}[{flat}]"
            else:
                expr = f"top->{verilator_member_name(port['name'])}"
            lines.append(
                f'  std::cout << "{element}=" << {rtl_cpp_value_expr(expr, port["type"])} << "\\n";'
            )

    lines += [
        "  top->final();",
        "  return 0;",
        "}",
    ]
    path.write_text("\n".join(lines) + "\n")
    return input_order


def parse_key_values(stdout: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in stdout.splitlines():
        if not line.strip():
            continue
        name, value = line.split("=", 1)
        values[name] = int(value, 0)
    return values


def build_verilator(verilator: str, top: str, rtl: Path, harness: Path, work: Path) -> Path:
    mdir = work / "obj_dir"
    cmd = [
        verilator,
        "--cc",
        "--exe",
        "--build",
        "--sv",
        "--top-module",
        sanitize_identifier(top),
        "--Mdir",
        str(mdir),
        str(rtl),
        str(harness),
        "-CFLAGS",
        "-std=c++20",
    ]
    try:
        lj.run(cmd, cwd=ROOT)
    except subprocess.CalledProcessError as exc:
        if exc.stdout:
            print(exc.stdout, file=sys.stderr, end="")
        if exc.stderr:
            print(exc.stderr, file=sys.stderr, end="")
        raise
    exe = mdir / verilator_class_name(top)
    if not exe.exists():
        raise FileNotFoundError(f"Verilator executable not found: {exe}")
    return exe


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("--top", required=True, help="Top function name or '*' wildcard pattern")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build")
    ap.add_argument("--cases", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cxx", default=lj.default_cxx())
    ap.add_argument("--verilator", default=shutil.which("verilator") or "verilator")
    ap.add_argument("--beopt", action="append", default=[],
                    help="BEIR optimization option to pass to predicate-expand, e.g. none")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    source = args.source.resolve()
    build_dir = args.build_dir.resolve()
    predicate = lj.find_exe(build_dir)
    work = Path(tempfile.mkdtemp(prefix="rtlzz_rtl_diff_", dir="/tmp"))
    try:
        portmeta = work / "program.portmeta.json"
        rtl = work / "program.sv"
        common_args = [
            str(predicate), str(source), "--top", args.top,
            "--unroll-limit", "4096",
            "--vullib", str(ROOT / "third_party/vulsim/vullib"),
            "--clang-arg", f"-I{ROOT}",
            "--clang-arg", "-std=c++20",
        ]
        for opt in args.beopt:
            common_args += ["--beopt", opt]
        lj.run(common_args + ["--format", "portmeta", "-o", str(portmeta)], cwd=ROOT)
        lj.run(common_args + ["--format", "rtl", "-o", str(rtl)], cwd=ROOT)
        program = json.loads(portmeta.read_text())
        resolved_top = program.get("function", args.top)

        oracle_cpp = work / "oracle.cpp"
        oracle_input_order = lj.generate_harness(source, resolved_top, program, oracle_cpp)
        oracle_exe = work / "oracle"
        lj.run([
            args.cxx, "-std=c++20", str(oracle_cpp),
            f"-I{ROOT}", f"-I{ROOT / 'third_party/vulsim/vullib'}",
            "-o", str(oracle_exe),
        ], cwd=ROOT)

        rtl_tb_cpp = work / "rtl_tb.cpp"
        rtl_input_order = generate_verilator_harness(resolved_top, program, rtl_tb_cpp)
        if rtl_input_order != oracle_input_order:
            raise RuntimeError(
                f"input order mismatch: rtl={rtl_input_order} oracle={oracle_input_order}"
            )
        rtl_exe = build_verilator(args.verilator, resolved_top, rtl, rtl_tb_cpp, work)

        rng = random.Random(args.seed)
        for case in range(args.cases):
            inputs = lj.random_inputs(program, rng)
            argv = [str(inputs[name]) for name in rtl_input_order]
            expected = parse_key_values(lj.run([str(oracle_exe)] + argv, cwd=ROOT).stdout)
            actual = parse_key_values(lj.run([str(rtl_exe)] + argv, cwd=ROOT).stdout)
            for name, exp in expected.items():
                got = actual.get(name)
                width = lj.type_width(next(
                    p["type"] for p in program["ports"]
                    if name in p.get("element_symbols", [])
                ))
                exp &= lj.mask(width)
                if got != exp:
                    print(f"Mismatch case={case} output={name}: rtl={got} cpp={exp}", file=sys.stderr)
                    print(f"inputs={inputs}", file=sys.stderr)
                    if args.keep:
                        print(f"workdir={work}", file=sys.stderr)
                    else:
                        print("rerun with --keep to inspect temporary artifacts", file=sys.stderr)
                    return 1
        print(f"PASS {args.cases} RTL cases for {source}")
        return 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
