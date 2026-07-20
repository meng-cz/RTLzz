#!/usr/bin/env python3
"""Differentially test generated SystemVerilog RTL against the original C++.

The script:
  1. runs predicate-expand to produce pipelinev2 port metadata and RTL,
  2. builds a tiny C++ oracle harness that includes and calls the source top,
  3. builds a Verilator C++ testbench around the generated RTL,
  4. compares RTL outputs against the C++ oracle for random inputs.
"""

from __future__ import annotations

import argparse
import json
import re
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def default_cxx() -> str:
    return shutil.which("clang++") or shutil.which("g++") or "c++"


def find_exe(build_dir: Path) -> Path:
    candidates = [
        build_dir / "predicate-expand",
        build_dir / "Debug" / "predicate-expand",
        build_dir / "Release" / "predicate-expand",
        build_dir / "RelWithDebInfo" / "predicate-expand",
        build_dir / "MinSizeRel" / "predicate-expand",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"predicate-expand executable not found under {build_dir}")


def type_width(t: dict[str, Any]) -> int:
    width = int(t.get("width", 0))
    if width <= 0:
        raise ValueError(f"port type has no positive width: {t}")
    return width


def mask(width: int) -> int:
    return (1 << width) - 1 if width > 0 else 0


def split_top_level_commas(text: str) -> list[str]:
    parts: list[str] = []
    start = 0
    angle = paren = bracket = brace = 0
    for i, ch in enumerate(text):
        if ch == "<":
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}" and brace:
            brace -= 1
        elif ch == "," and not (angle or paren or bracket or brace):
            parts.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def strip_cxx_comments(text: str) -> str:
    text = re.sub(r"//.*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def sanitize_cxx_value_type(text: str) -> str:
    text = text.strip()
    text = re.sub(r"\bconst\b", "", text)
    text = text.replace("&&", " ").replace("&", " ").replace("*", " ")
    text = re.sub(r"\s+", " ", text).strip()
    return text


def source_param_types(source: Path, top: str) -> dict[str, str]:
    text = strip_cxx_comments(source.read_text())
    match = re.search(r"\b" + re.escape(top) + r"\s*\(", text)
    if not match:
        return {}
    open_paren = text.find("(", match.start())
    depth = 0
    close_paren = -1
    for i in range(open_paren, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                close_paren = i
                break
    if close_paren < 0:
        return {}
    params = text[open_paren + 1:close_paren]
    out: dict[str, str] = {}
    for param in split_top_level_commas(params):
        param = param.split("=", 1)[0].strip()
        m = re.search(r"([A-Za-z_][A-Za-z_0-9]*)\s*(?:\[[^\]]*\]\s*)*$", param)
        if not m:
            continue
        name = m.group(1)
        type_part = param[:m.start(1)].strip()
        value_type = sanitize_cxx_value_type(type_part)
        if value_type:
            out[name] = value_type
    return out


def source_global_port_types(source: Path) -> dict[str, str]:
    text = strip_cxx_comments(source.read_text())
    out: dict[str, str] = {}
    pattern = re.compile(
        r"^\s*#\s*pragma\s+(?:input_port|output_port)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*$\s*"
        r"^([^;\n]+?)\s+\1\s*;\s*$",
        flags=re.M,
    )
    for match in pattern.finditer(text):
        out[match.group(1)] = sanitize_cxx_value_type(match.group(2))
    return out


def cxx_scalar_type(t: dict[str, Any], source_type: str | None = None) -> str:
    if source_type:
        if not source_type.startswith("std::array<") and not source_type.startswith("array<"):
            return source_type
    name = str(t.get("name", ""))
    if name == "bool":
        return "bool"
    if name.startswith("Int<"):
        return name
    builtin = {
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "unsigned char", "unsigned short", "unsigned int", "unsigned long",
        "unsigned long long", "char", "short", "int", "long", "long long",
    }
    if name in builtin:
        return name
    hw_kind = str(t.get("hw_kind", ""))
    if hw_kind == "bool":
        return "bool"
    if hw_kind == "Int":
        return f"Int<{type_width(t)}>"
    raise ValueError(f"unsupported port scalar type for C++ harness: {t}")


def int_width_from_scalar(scalar: str) -> int | None:
    match = re.fullmatch(r"Int\s*<\s*(\d+)\s*>", scalar)
    if not match:
        return None
    return int(match.group(1))


def cxx_port_type(t: dict[str, Any], source_type: str | None = None) -> str:
    if source_type and ("std::array<" in source_type or "array<" in source_type):
        return source_type
    result = cxx_scalar_type(t, source_type)
    for dim in reversed(t.get("array_dims", [])):
        result = f"std::array<{result}, {int(dim)}>"
    return result


def access_indices(base: str, indices: list[int]) -> str:
    out = base
    for idx in indices:
        out += f"[{idx}]"
    return out


def element_indices(port: dict[str, Any], flat: int) -> list[int]:
    elements = port.get("elements", [])
    if flat < len(elements):
        return [int(v) for v in elements[flat].get("indices", [])]
    if port["type"].get("is_array"):
        return [flat]
    return []


def assign_from_argv(expr: str, t: dict[str, Any], argv_index: int,
                     source_type: str | None = None) -> str:
    scalar = cxx_scalar_type(t, source_type)
    if scalar == "bool":
        return f"{expr} = std::strtoull(argv[{argv_index}], nullptr, 0) != 0;"
    int_width = int_width_from_scalar(scalar)
    if int_width is not None and int_width > 64:
        return f"{expr} = int_from_decimal<{int_width}>(argv[{argv_index}]);"
    if int_width is not None:
        return f"{expr} = {scalar}(std::strtoull(argv[{argv_index}], nullptr, 0));"
    if scalar.startswith("int") or scalar in {"char", "short", "int", "long", "long long"}:
        return f"{expr} = static_cast<{scalar}>(std::strtoll(argv[{argv_index}], nullptr, 0));"
    return f"{expr} = static_cast<{scalar}>(std::strtoull(argv[{argv_index}], nullptr, 0));"


def cpp_value_expr(expr: str, t: dict[str, Any]) -> str:
    scalar = cxx_scalar_type(t)
    if scalar == "bool":
        return f"({expr} ? 1ULL : 0ULL)"
    int_width = int_width_from_scalar(scalar)
    if int_width is not None and int_width > 64:
        return f"hex_from_int<{int_width}>({expr})"
    if int_width is not None:
        return f"static_cast<unsigned long long>({expr}.template to<unsigned long long>())"
    return f"static_cast<unsigned long long>({expr})"


def random_inputs(program: dict[str, Any], rng: random.Random) -> dict[str, int]:
    values: dict[str, int] = {}
    for port in program["ports"]:
        if port["direction"] != "Input":
            continue
        width = type_width(port["type"])
        max_value = mask(width)
        for element in port["element_symbols"]:
            values[element] = rng.randrange(max_value + 1)
    return values


def generate_harness(source: Path, top: str, program: dict[str, Any], path: Path) -> list[str]:
    input_order: list[str] = []
    source_types = source_global_port_types(source)
    lines = [
        "#include <array>",
        "#include <cstdint>",
        "#include <cstdlib>",
        "#include <iomanip>",
        "#include <iostream>",
        "#include <sstream>",
        "#include <vector>",
        f'#include "{source}"',
        "",
        "static std::vector<uint64_t> parse_words64(const char* text, int width) {",
        "  int nwords = (width + 63) / 64;",
        "  std::vector<uint64_t> words(nwords, 0);",
        "  for (const char* p = text; *p; ++p) {",
        "    if (*p < '0' || *p > '9') continue;",
        "    unsigned __int128 carry = static_cast<unsigned __int128>(*p - '0');",
        "    for (int i = 0; i < nwords; ++i) {",
        "      unsigned __int128 next = static_cast<unsigned __int128>(words[i]) * 10 + carry;",
        "      words[i] = static_cast<uint64_t>(next);",
        "      carry = next >> 64;",
        "    }",
        "  }",
        "  int top_bits = width % 64;",
        "  if (top_bits != 0 && nwords > 0) words[nwords - 1] &= ((uint64_t{1} << top_bits) - 1ULL);",
        "  return words;",
        "}",
        "",
        "template <uint32_t Width, uint32_t Lo>",
        "static void assign_int_words(Int<Width>& value, const std::vector<uint64_t>& words) {",
        "  if constexpr (Lo < Width) {",
        "    constexpr uint32_t chunk = (Width - Lo >= 64) ? 64 : (Width - Lo);",
        "    value.template at<Lo + chunk - 1, Lo>() = Int<chunk>(words[Lo / 64]);",
        "    assign_int_words<Width, Lo + 64>(value, words);",
        "  }",
        "}",
        "",
        "template <uint32_t Width>",
        "static Int<Width> int_from_decimal(const char* text) {",
        "  Int<Width> value(0);",
        "  auto words = parse_words64(text, Width);",
        "  assign_int_words<Width, 0>(value, words);",
        "  return value;",
        "}",
        "",
        "template <uint32_t Width, uint32_t Lo>",
        "static void collect_int_words(const Int<Width>& value, std::vector<uint64_t>& words) {",
        "  if constexpr (Lo < Width) {",
        "    constexpr uint32_t chunk = (Width - Lo >= 64) ? 64 : (Width - Lo);",
        "    auto part = Int<chunk>(value.template at<Lo + chunk - 1, Lo>());",
        "    words.push_back(part.template to<uint64_t>());",
        "    collect_int_words<Width, Lo + 64>(value, words);",
        "  }",
        "}",
        "",
        "template <uint32_t Width>",
        "static std::string hex_from_int(const Int<Width>& value) {",
        "  std::vector<uint64_t> words;",
        "  collect_int_words<Width, 0>(value, words);",
        "  int top = static_cast<int>(words.size()) - 1;",
        "  while (top > 0 && words[top] == 0) --top;",
        "  std::ostringstream os;",
        "  os << \"0x\" << std::hex << words[top];",
        "  for (int i = top - 1; i >= 0; --i) {",
        "    os << std::setw(16) << std::setfill('0') << words[i];",
        "  }",
        "  return os.str();",
        "}",
        "",
        "int main(int argc, char** argv) {",
    ]

    arg_index = 1
    argc_check_index = len(lines)
    for port in program["ports"]:
        var = port["name"]
        source_type = source_types.get(port["name"])
        if port["direction"] != "Input":
            continue
        for flat, element in enumerate(port["element_symbols"]):
            input_order.append(element)
            expr = access_indices(var, element_indices(port, flat))
            lines.append("  " + assign_from_argv(expr, port["type"], arg_index, source_type))
            arg_index += 1

    lines.insert(argc_check_index, f"  if (argc != {arg_index}) return 97;")
    lines.append(f"  {top}();")

    for port in program["ports"]:
        if port["direction"] != "Output":
            continue
        var = port["name"]
        for flat, element in enumerate(port["element_symbols"]):
            expr = access_indices(var, element_indices(port, flat))
            lines.append(
                f'  std::cout << "{element}=" << {cpp_value_expr(expr, port["type"])} << "\\n";'
            )

    lines += [
        "  return 0;",
        "}",
    ]
    path.write_text("\n".join(lines) + "\n")
    return input_order


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
    width = type_width(t)
    if width <= 64:
        return f"static_cast<unsigned long long>({expr})"
    return f"hex_from_words({expr}, {width})"


def rtl_cpp_assign_from_argv(expr: str, t: dict[str, Any], argv_index: int) -> str:
    width = type_width(t)
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
        run(cmd, cwd=ROOT)
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
    ap.add_argument("--cxx", default=default_cxx())
    ap.add_argument("--verilator", default=shutil.which("verilator") or "verilator")
    ap.add_argument("--beopt", action="append", default=[],
                    help="BEIR optimization option to pass to predicate-expand, e.g. none")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    source = args.source.resolve()
    build_dir = args.build_dir.resolve()
    predicate = find_exe(build_dir)
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
        run(common_args + ["--format", "portmeta", "-o", str(portmeta)], cwd=ROOT)
        run(common_args + ["--format", "rtl", "-o", str(rtl)], cwd=ROOT)
        program = json.loads(portmeta.read_text())
        resolved_top = program.get("function", args.top)

        oracle_cpp = work / "oracle.cpp"
        oracle_input_order = generate_harness(source, resolved_top, program, oracle_cpp)
        oracle_exe = work / "oracle"
        run([
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
            inputs = random_inputs(program, rng)
            argv = [str(inputs[name]) for name in rtl_input_order]
            expected = parse_key_values(run([str(oracle_exe)] + argv, cwd=ROOT).stdout)
            actual = parse_key_values(run([str(rtl_exe)] + argv, cwd=ROOT).stdout)
            for name, exp in expected.items():
                got = actual.get(name)
                width = type_width(next(
                    p["type"] for p in program["ports"]
                    if name in p.get("element_symbols", [])
                ))
                exp &= mask(width)
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
