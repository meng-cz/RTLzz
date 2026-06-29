#!/usr/bin/env python3
"""Differentially test predicate-expand listjson against the original C++.

The script:
  1. runs predicate-expand on one source file to produce listjson,
  2. interprets the listjson for random inputs,
  3. builds a tiny C++ harness that includes the source and calls the top,
  4. compares the output ports for the same random inputs.

It is intentionally lightweight and dependency-free. It supports the scalar and
static std::array-style ports used by the current positive fixtures.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def default_cxx() -> str:
    return os.environ.get("CXX") or shutil.which("clang++") or "g++"


def mask(width: int) -> int:
    if width <= 0:
        return (1 << 64) - 1
    return (1 << width) - 1


def to_signed(value: int, width: int) -> int:
    value &= mask(width)
    if width > 0 and value & (1 << (width - 1)):
        return value - (1 << width)
    return value


def parse_int_literal(text: str) -> int:
    t = text.strip()
    if t in ("true", "false"):
        return 1 if t == "true" else 0
    t = re.sub(r"(?i)(ull|llu|ul|lu|u|l)+$", "", t)
    return int(t, 0)


def type_width(t: dict[str, Any]) -> int:
    return int(t.get("width") or 64)


def type_signed(t: dict[str, Any]) -> bool:
    return bool(t.get("signed"))


def type_mask(t: dict[str, Any]) -> int:
    return mask(type_width(t))


class ListJsonEvaluator:
    def __init__(self, program: dict[str, Any], inputs: dict[str, int]):
        self.program = program
        self.inputs = inputs
        self.signals = {s["name"]: s for s in program["signals"]}
        self.cache: dict[str, int] = {}
        self.lookup_tables = self._collect_lookup_tables()

    def _collect_lookup_tables(self) -> dict[str, list[int]]:
        tables: dict[str, list[int]] = {}
        for table in self.program.get("lookup_tables", []):
            tables[table["name"]] = [parse_int_literal(str(v)) for v in table.get("values", [])]
        return tables

    def eval_operand(self, operand: dict[str, Any]) -> int | str:
        kind = operand["kind"]
        if kind == "literal":
            text = operand["text"]
            try:
                return parse_int_literal(text) & type_mask(operand["type"])
            except ValueError:
                return text
        if kind == "symbol":
            return self.eval_signal(operand["text"])
        if kind == "port":
            return self.inputs[operand["text"]]
        if kind == "aggregate":
            return operand["text"]
        raise RuntimeError(f"unsupported operand kind: {kind}")

    def eval_signal(self, name: str) -> int:
        if name in self.cache:
            return self.cache[name]
        signal = self.signals.get(name)
        if signal is None:
            # Some SSA initial versions are read as free input-like values.
            if name in self.inputs:
                return self.inputs[name]
            raise RuntimeError(f"unknown signal: {name}")
        driver = signal.get("driver")
        if driver is None:
            base_name = re.sub(r"_\d+$", "", name)
            value = self.inputs.get(name, self.inputs.get(base_name, 0))
        else:
            value = self.eval_driver(driver)
        value &= type_mask(signal["type"])
        self.cache[name] = value
        return value

    def eval_driver(self, driver: dict[str, Any]) -> int:
        kind = driver["kind"]
        ops = [self.eval_operand(o) for o in driver.get("operands", [])]
        width = type_width(driver["type"])
        out_mask = mask(width)

        if kind == "port_read":
            port = driver["operands"][0]["text"]
            if len(driver["operands"]) == 1:
                return self.inputs[port] & out_mask
            index = int(self.eval_operand(driver["operands"][1]))
            return self.inputs[f"{port}_{index}"] & out_mask
        if kind == "assign":
            return int(ops[0]) & out_mask
        if kind == "binary":
            a, b = int(ops[0]), int(ops[1])
            op = driver.get("op", "")
            signed = any(type_signed(o["type"]) for o in driver["operands"][:2])
            sa = to_signed(a, type_width(driver["operands"][0]["type"]))
            sb = to_signed(b, type_width(driver["operands"][1]["type"]))
            if op == "+": return (a + b) & out_mask
            if op == "-": return (a - b) & out_mask
            if op == "*": return (a * b) & out_mask
            if op == "/": return ((int(sa / sb) if signed else a // b) if b else 0) & out_mask
            if op == "%": return ((sa % sb if signed else a % b) if b else 0) & out_mask
            if op == "&": return (a & b) & out_mask
            if op == "|": return (a | b) & out_mask
            if op == "^": return (a ^ b) & out_mask
            if op == "&&": return 1 if a and b else 0
            if op == "||": return 1 if a or b else 0
            if op == "==": return 1 if a == b else 0
            if op == "!=": return 1 if a != b else 0
            if op == "<": return 1 if (sa < sb if signed else a < b) else 0
            if op == "<=": return 1 if (sa <= sb if signed else a <= b) else 0
            if op == ">": return 1 if (sa > sb if signed else a > b) else 0
            if op == ">=": return 1 if (sa >= sb if signed else a >= b) else 0
            if op == "<<": return (a << b) & out_mask
            if op == ">>": return ((sa >> b) if type_signed(driver["operands"][0]["type"]) else (a >> b)) & out_mask
            raise RuntimeError(f"unsupported binary op: {op}")
        if kind == "unary":
            a = int(ops[0])
            op = driver.get("op", "")
            if op == "!": return 0 if a else 1
            if op == "~": return (~a) & out_mask
            if op == "-": return (-a) & out_mask
            raise RuntimeError(f"unsupported unary op: {op}")
        if kind == "ite":
            return int(ops[1] if int(ops[0]) else ops[2]) & out_mask
        if kind in ("zext", "trunc", "cast"):
            return int(ops[0]) & out_mask
        if kind == "sext":
            in_type = driver["operands"][0]["type"]
            return to_signed(int(ops[0]), type_width(in_type)) & out_mask
        if kind == "slice":
            lo = int(driver["lo"])
            hi = int(driver["hi"])
            return (int(ops[0]) >> lo) & mask(hi - lo + 1)
        if kind == "bit_select":
            return (int(ops[0]) >> int(driver["bit"])) & 1
        if kind == "write_slice":
            base, value = int(ops[0]), int(ops[1])
            hi, lo = int(driver["hi"]), int(driver["lo"])
            width = hi - lo + 1
            field_mask = mask(width) << lo
            return ((base & ~field_mask) | ((value & mask(width)) << lo)) & out_mask
        if kind == "write_bit":
            base, value = int(ops[0]), int(ops[1]) & 1
            bit = int(driver["bit"])
            return ((base & ~(1 << bit)) | (value << bit)) & out_mask
        if kind == "dynamic_write_slice":
            base, idx, value = int(ops[0]), int(ops[1]), int(ops[2])
            width = type_width(driver["operands"][2]["type"])
            field_mask = mask(width) << idx
            return ((base & ~field_mask) | ((value & mask(width)) << idx)) & out_mask
        if kind == "dynamic_write_bit":
            base, idx, value = int(ops[0]), int(ops[1]), int(ops[2]) & 1
            return ((base & ~(1 << idx)) | (value << idx)) & out_mask
        if kind == "concat":
            result = 0
            for operand, value in zip(driver["operands"], ops):
                w = type_width(operand["type"])
                result = (result << w) | (int(value) & mask(w))
            return result & out_mask
        if kind == "repeat":
            value = int(ops[0])
            in_w = type_width(driver["operands"][0]["type"])
            result = 0
            for _ in range(int(driver["times"])):
                result = (result << in_w) | (value & mask(in_w))
            return result & out_mask
        if kind == "reduce_or":
            return 1 if int(ops[0]) else 0
        if kind == "reduce_and":
            in_w = type_width(driver["operands"][0]["type"])
            return 1 if (int(ops[0]) & mask(in_w)) == mask(in_w) else 0
        if kind == "reduce_xor":
            return int(ops[0]).bit_count() & 1
        if kind == "dynamic_bit_select":
            return (int(ops[0]) >> int(ops[1])) & 1
        if kind == "dynamic_slice":
            idx = int(ops[1])
            return (int(ops[0]) >> idx) & out_mask
        if kind == "lookup":
            table = str(ops[0])
            index = int(ops[1])
            values = self.lookup_tables.get(table)
            if values is None:
                raise RuntimeError(f"unknown lookup table: {table}")
            if index < 0 or index >= len(values):
                raise RuntimeError(f"lookup index out of range: {table}[{index}]")
            return values[index] & out_mask
        raise RuntimeError(f"unsupported driver kind: {kind}")

    def outputs(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for port in self.program["ports"]:
            if port["direction"] != "Output":
                continue
            for element in port["element_symbols"]:
                out[element] = self.eval_signal(element)
        return out


def cpp_type(t: dict[str, Any]) -> str:
    name = t["name"]
    if name == "bool":
        return "bool"
    return name


def array_suffix(dims: list[int]) -> str:
    return "".join(f"[{d}]" for d in dims)


def nested_std_array(base: str, dims: list[int]) -> str:
    out = base
    for dim in reversed(dims):
        out = f"std::array<{out}, {dim}>"
    return out


def access_expr(name: str, dims: list[int], flat: int) -> str:
    if not dims:
        return name
    indexes: list[int] = []
    remaining = flat
    for i, dim in enumerate(dims):
        stride = 1
        for d in dims[i + 1:]:
            stride *= d
        indexes.append(remaining // stride)
        remaining %= stride
    return name + "".join(f"[{i}]" for i in indexes)


def cxx_value_expr(expr: str, t: dict[str, Any]) -> str:
    if t["name"] == "bool":
        return f"(({expr}) ? 1ULL : 0ULL)"
    if t["name"].startswith(("Int<", "UInt<")):
        return f"static_cast<unsigned long long>(({expr}).to<unsigned long long>())"
    return f"static_cast<unsigned long long>({expr})"


def cxx_assign_from_argv(expr: str, t: dict[str, Any], argv_index: int) -> str:
    name = t["name"]
    if name == "bool":
        return f"{expr} = (std::strtoull(argv[{argv_index}], nullptr, 0) != 0);"
    if name.startswith(("Int<", "UInt<")):
        return f"{expr} = {name}(std::strtoull(argv[{argv_index}], nullptr, 0));"
    return f"{expr} = static_cast<{name}>(std::strtoull(argv[{argv_index}], nullptr, 0));"


def split_cpp_params(params: str) -> list[str]:
    out: list[str] = []
    start = 0
    angle = paren = bracket = brace = 0
    for i, ch in enumerate(params):
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
            out.append(params[start:i].strip())
            start = i + 1
    tail = params[start:].strip()
    if tail:
        out.append(tail)
    return out


def extract_top_param_order(source: Path, top: str, known_ports: set[str]) -> list[str] | None:
    text = source.read_text()
    match = re.search(rf"\b{re.escape(top)}\s*\(", text)
    if not match:
        return None

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
        return None

    order: list[str] = []
    for param in split_cpp_params(text[open_paren + 1:close_paren]):
        if param == "void":
            continue
        param = param.split("=", 1)[0].strip()
        ids = re.findall(r"[A-Za-z_]\w*", param)
        if not ids:
            continue
        name = ids[-1]
        if name not in known_ports:
            return None
        order.append(name)
    return order


def generate_harness(source: Path, top: str, program: dict[str, Any], path: Path) -> list[str]:
    input_elements: list[tuple[str, dict[str, Any]]] = []
    output_elements: list[tuple[str, dict[str, Any]]] = []
    lines: list[str] = [
        "#include <array>",
        "#include <cstdlib>",
        "#include <iostream>",
        f'#include "{source}"',
        "",
        "int main(int argc, char** argv) {",
    ]

    for port in program["ports"]:
        t = port["type"]
        base = cpp_type(t)
        if t.get("is_array"):
            decl_type = nested_std_array(base, list(t.get("array_dims") or [t.get("array_size")]))
        else:
            decl_type = base
        lines.append(f"  {decl_type} {port['name']}{{}};")

    arg_index = 1
    for port in program["ports"]:
        if port["direction"] != "Input":
            continue
        dims = list(port["type"].get("array_dims") or [])
        for flat, element in enumerate(port["element_symbols"]):
            expr = access_expr(port["name"], dims, flat)
            input_elements.append((element, port["type"]))
            arg_index += 1

    lines.append(f"  if (argc != {arg_index}) return 97;")
    arg_index = 1
    for port in program["ports"]:
        if port["direction"] != "Input":
            continue
        dims = list(port["type"].get("array_dims") or [])
        for flat, _element in enumerate(port["element_symbols"]):
            expr = access_expr(port["name"], dims, flat)
            lines.append("  " + cxx_assign_from_argv(expr, port["type"], arg_index))
            arg_index += 1

    port_names = {port["name"] for port in program["ports"]}
    param_order = extract_top_param_order(source, top, port_names)
    if param_order and set(param_order) == port_names and len(param_order) == len(port_names):
        call_args = ", ".join(param_order)
    else:
        call_args = ", ".join(port["name"] for port in program["ports"])
    lines.append(f"  {top}({call_args});")

    for port in program["ports"]:
        if port["direction"] != "Output":
            continue
        dims = list(port["type"].get("array_dims") or [])
        for flat, element in enumerate(port["element_symbols"]):
            expr = access_expr(port["name"], dims, flat)
            lines.append(
                f'  std::cout << "{element}=" << {cxx_value_expr(expr, port["type"])} << "\\n";'
            )
            output_elements.append((element, port["type"]))

    lines.append("  return 0;")
    lines.append("}")
    path.write_text("\n".join(lines) + "\n")
    return [name for name, _ in input_elements]


def random_inputs(program: dict[str, Any], rng: random.Random) -> dict[str, int]:
    values: dict[str, int] = {}
    max_array_index = 0
    for port in program["ports"]:
        if port["type"].get("is_array"):
            max_array_index = max(max_array_index, len(port["element_symbols"]) - 1)
    for port in program["ports"]:
        if port["direction"] != "Input":
            continue
        width = type_width(port["type"])
        limit = mask(width)
        for element in port["element_symbols"]:
            if re.search(r"(idx|index|addr)", element, re.IGNORECASE) and max_array_index > 0:
                value = rng.randrange(max_array_index + 1)
            elif width == 1 or port["type"]["name"] == "bool":
                value = rng.randrange(2)
            else:
                value = rng.getrandbits(min(width, 32)) & limit
            values[element] = value
            if len(port["element_symbols"]) == 1:
                values[port["name"]] = value
    return values


def run(cmd: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, **kwargs)


def find_exe(build_dir: Path) -> Path:
    for name in ("predicate-expand", "predicate-expand.exe"):
        p = build_dir / name
        if p.exists():
            return p
    raise FileNotFoundError(f"predicate-expand not found in {build_dir}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("--top", default="hls_main")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build")
    ap.add_argument("--cases", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cxx", default=default_cxx())
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    source = args.source.resolve()
    build_dir = args.build_dir.resolve()
    predicate = find_exe(build_dir)
    work = Path(tempfile.mkdtemp(prefix="rtlzz_diff_", dir="/tmp"))
    try:
        listjson = work / "program.listjson"
        run([
            str(predicate), str(source), "--top", args.top, "--format", "listjson",
            "--unroll-limit", "4096",
            "--clang-arg", f"-I{ROOT}",
            "--clang-arg", f"-I{ROOT / 'third_party/vulsim/vullib'}",
            "--clang-arg", "-std=c++20",
            "-o", str(listjson),
        ], cwd=ROOT)
        program = json.loads(listjson.read_text())

        harness_cpp = work / "harness.cpp"
        input_order = generate_harness(source, args.top, program, harness_cpp)
        harness_exe = work / "harness"
        run([
            args.cxx, "-std=c++20", str(harness_cpp),
            f"-I{ROOT}", f"-I{ROOT / 'third_party/vulsim/vullib'}",
            "-o", str(harness_exe),
        ], cwd=ROOT)

        rng = random.Random(args.seed)
        for case in range(args.cases):
            inputs = random_inputs(program, rng)
            actual = ListJsonEvaluator(program, inputs).outputs()
            argv = [str(harness_exe)] + [str(inputs[name]) for name in input_order]
            proc = run(argv, cwd=ROOT)
            expected = {}
            for line in proc.stdout.splitlines():
                if not line.strip():
                    continue
                name, value = line.split("=", 1)
                expected[name] = int(value, 0)
            for name, exp in expected.items():
                got = actual.get(name)
                if got != exp:
                    print(f"Mismatch case={case} output={name}: listjson={got} cpp={exp}", file=sys.stderr)
                    print(f"inputs={inputs}", file=sys.stderr)
                    if args.keep:
                        print(f"workdir={work}", file=sys.stderr)
                    else:
                        print("rerun with --keep to inspect temporary artifacts", file=sys.stderr)
                    return 1
        print(f"PASS {args.cases} cases for {source}")
        return 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
