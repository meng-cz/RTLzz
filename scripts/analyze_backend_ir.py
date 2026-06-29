#!/usr/bin/env python3
"""Generate predicate JSON for positive fixtures and audit backend IR parsing."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class SymbolDebug:
    name: str
    flags: set[str]
    type_text: str
    driver: str


@dataclass
class PortDebug:
    name: str
    direction: str
    type_text: str
    elements: list[str]


@dataclass
class BackendDebug:
    ports: dict[str, PortDebug]
    symbols: dict[str, SymbolDebug]


@dataclass
class FixtureResult:
    name: str
    fixture: Path
    json_path: Path
    backend_path: Path
    report_path: Path
    errors: list[str]
    warnings: list[str]
    skipped_backend: bool = False


def run(cmd: list[str], cwd: Path, log_path: Path | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        cmd,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if log_path:
        log_path.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        rendered = " ".join(cmd)
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {rendered}\n{completed.stdout}")
    return completed


def configure_and_build(root: Path, source: Path, build: Path, target: str) -> Path:
    build.mkdir(parents=True, exist_ok=True)
    run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"], cwd=root)
    run(["cmake", "--build", str(build), "--config", "Release", "-j", str(os.cpu_count() or 2)], cwd=root)

    candidates = [
        build / target,
        build / "Release" / f"{target}.exe",
        build / "Debug" / f"{target}.exe",
        build / f"{target}.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise RuntimeError(f"could not find built executable for target {target} under {build}")


def fixture_name(path: Path) -> str:
    return path.name.removesuffix(".logic.cpp")


def positive_fixtures(root: Path) -> list[Path]:
    fixture_dir = root / "tests" / "fixtures"
    return sorted(fixture_dir.glob("*.logic.cpp"))


def strip_line_comments(text: str) -> str:
    return re.sub(r"//.*", "", text)


def split_params(params: str) -> list[str]:
    result: list[str] = []
    current: list[str] = []
    depth_angle = 0
    depth_paren = 0
    depth_bracket = 0
    for ch in params:
        if ch == "<":
            depth_angle += 1
        elif ch == ">" and depth_angle:
            depth_angle -= 1
        elif ch == "(":
            depth_paren += 1
        elif ch == ")" and depth_paren:
            depth_paren -= 1
        elif ch == "[":
            depth_bracket += 1
        elif ch == "]" and depth_bracket:
            depth_bracket -= 1
        if ch == "," and depth_angle == 0 and depth_paren == 0 and depth_bracket == 0:
            item = "".join(current).strip()
            if item:
                result.append(item)
            current = []
        else:
            current.append(ch)
    item = "".join(current).strip()
    if item:
        result.append(item)
    return result


def parse_source_params(source: Path, top: str) -> set[str]:
    text = strip_line_comments(source.read_text(encoding="utf-8"))
    match = re.search(r"\b" + re.escape(top) + r"\s*\((.*?)\)\s*\{", text, re.S)
    if not match:
        return set()
    names: set[str] = set()
    for param in split_params(match.group(1)):
        param = param.split("=")[0].strip()
        match_name = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*$", param)
        if match_name:
            names.add(match_name.group(1))
    return names


def base_signal_name(name: str) -> str:
    return re.sub(r"_\d+$", "", name)


def expr_vars(expr: object) -> Iterable[str]:
    if not isinstance(expr, dict):
        return
    if expr.get("kind") == "var" and isinstance(expr.get("name"), str):
        yield expr["name"]
    for value in expr.values():
        if isinstance(value, dict):
            yield from expr_vars(value)
        elif isinstance(value, list):
            for item in value:
                yield from expr_vars(item)


def parse_backend_debug(text: str) -> BackendDebug:
    ports: dict[str, PortDebug] = {}
    symbols: dict[str, SymbolDebug] = {}
    current: SymbolDebug | None = None
    current_port: PortDebug | None = None
    section = ""
    port_re = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_]*) \[([A-Za-z_][A-Za-z0-9_]*)\] : (.*)$")
    symbol_re = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_]*)(.*?) : (.*)$")
    for line in text.splitlines():
        if line.startswith("ports "):
            section = "ports"
            current = None
            continue
        if line.startswith("aggregates "):
            section = "aggregates"
            current = None
            current_port = None
            continue
        if line.startswith("symbols "):
            section = "symbols"
            current = None
            current_port = None
            continue
        if section == "ports":
            port_match = port_re.match(line)
            if port_match:
                current_port = PortDebug(
                    name=port_match.group(1),
                    direction=port_match.group(2),
                    type_text=port_match.group(3),
                    elements=[],
                )
                ports[current_port.name] = current_port
                continue
            if current_port and line.startswith("    elements ="):
                current_port.elements = line.removeprefix("    elements =").strip().split()
                continue
        if section != "symbols":
            continue
        symbol_match = symbol_re.match(line)
        if symbol_match:
            flags = set(re.findall(r"\[([A-Za-z_][A-Za-z0-9_]*)\]", symbol_match.group(2)))
            current = SymbolDebug(
                name=symbol_match.group(1),
                flags=flags,
                type_text=symbol_match.group(3),
                driver="",
            )
            symbols[current.name] = current
            continue
        if current and line.startswith("    driver = "):
            current.driver = line.removeprefix("    driver = ").strip()
    return BackendDebug(ports=ports, symbols=symbols)


def assignment_targets(ir: dict) -> list[str]:
    targets: list[str] = []
    for assignment in ir.get("assignments", []):
        target_expr = assignment.get("target_expr", {})
        if isinstance(target_expr, dict) and target_expr.get("kind") == "var":
            targets.append(target_expr.get("name", ""))
    return [target for target in targets if target]


def is_initial_port_read(name: str, ports: set[str]) -> bool:
    if name in ports:
        return True
    return name.endswith("_0") and base_signal_name(name) in ports


def forbidden_output_reads(ir: dict) -> list[str]:
    outputs = set(ir.get("outputs", []))
    reads: set[str] = set()
    for assignment in ir.get("assignments", []):
        for expr_key in ("guard_expr", "value_expr"):
            for var_name in expr_vars(assignment.get(expr_key)):
                if is_initial_port_read(var_name, outputs):
                    reads.add(var_name)
    return sorted(reads)


def forbidden_inout_ports(ir: dict) -> list[str]:
    directions = ir.get("param_directions", {})
    if not isinstance(directions, dict):
        return []
    return sorted(name for name, direction in directions.items() if direction == "InOut")


def analyze_fixture(
    root: Path,
    predicate_expand: Path,
    backend: Path,
    fixture: Path,
    top: str,
    output_dir: Path,
) -> FixtureResult:
    name = fixture_name(fixture)
    case_dir = output_dir / name
    case_dir.mkdir(parents=True, exist_ok=True)
    json_path = case_dir / "out.json"
    backend_path = case_dir / "backend.txt"
    report_path = case_dir / "analysis.md"

    include_arg = f"-I{root / 'third_party' / 'vulsim' / 'vullib'}"
    run(
        [
            str(predicate_expand),
            str(fixture),
            "--top",
            top,
            "--format",
            "json",
            "-o",
            str(json_path),
            "--clang-arg",
            include_arg,
            "--clang-arg",
            "-std=c++20",
        ],
        cwd=root,
        log_path=case_dir / "predicate-expand.log",
    )

    ir = json.loads(json_path.read_text(encoding="utf-8"))
    errors: list[str] = []
    warnings: list[str] = []
    inout_ports = forbidden_inout_ports(ir)
    output_reads = forbidden_output_reads(ir)
    if inout_ports:
        errors.append(
            "fixture violates backend port convention: InOut ports are forbidden and should be negative tests: "
            + ", ".join(inout_ports)
        )
    if output_reads:
        errors.append(
            "fixture violates backend port convention: Output ports may not be read: "
            + ", ".join(output_reads)
        )
    if errors:
        report_lines = [
            f"# {name}",
            "",
            f"- source: `{fixture.relative_to(root)}`",
            f"- json: `{json_path.relative_to(root) if json_path.is_relative_to(root) else json_path}`",
            "- backend: `<skipped because fixture violates backend port convention>`",
            "",
            "## Result",
            "",
            "FAIL",
            "",
            "## Errors",
            "",
        ]
        report_lines.extend(f"- {error}" for error in errors)
        report_path.write_text("\n".join(report_lines) + "\n", encoding="utf-8")
        backend_path.write_text("skipped: fixture violates backend port convention\n", encoding="utf-8")
        return FixtureResult(name, fixture, json_path, backend_path, report_path, errors, warnings, True)

    backend_output = run([str(backend), str(json_path)], cwd=root).stdout
    backend_path.write_text(backend_output, encoding="utf-8")

    debug = parse_backend_debug(backend_output)
    ports = debug.ports
    symbols = debug.symbols

    if ir.get("schema_version") != "gpef-predicate-json-v1":
        errors.append(f"unexpected schema_version: {ir.get('schema_version')}")

    source_params = parse_source_params(fixture, top)
    if not source_params:
        warnings.append(f"could not parse source signature for {top}")

    directions = ir.get("param_directions", {})
    json_inputs = {name for name, direction in directions.items() if direction == "Input"}
    json_outputs = {name for name, direction in directions.items() if direction == "Output"}
    json_assignment_targets = assignment_targets(ir)
    vars_in_exprs = set()
    for assignment in ir.get("assignments", []):
        vars_in_exprs.update(expr_vars(assignment.get("guard_expr")))
        vars_in_exprs.update(expr_vars(assignment.get("value_expr")))

    for signal in sorted(json_inputs | json_outputs):
        if source_params and base_signal_name(signal) not in source_params and signal not in source_params:
            warnings.append(f"JSON signal '{signal}' does not map directly to source params {sorted(source_params)}")

    for input_name in sorted(json_inputs):
        port = ports.get(input_name)
        if not port:
            errors.append(f"input port '{input_name}' is missing from backend ports")
            continue
        if port.direction != "Input":
            errors.append(f"port '{input_name}' direction is {port.direction}, expected Input")
        for element in port.elements:
            symbol = symbols.get(element)
            if not symbol:
                errors.append(f"input port element '{element}' is missing from backend symbols")
            elif not symbol.driver.startswith("port_read("):
                errors.append(f"input port element '{element}' is not driven by port_read: {symbol.driver}")

    for output_name in sorted(json_outputs):
        port = ports.get(output_name)
        if not port:
            errors.append(f"output port '{output_name}' is missing from backend ports")
            continue
        if port.direction != "Output":
            errors.append(f"port '{output_name}' direction is {port.direction}, expected Output")
        for element in port.elements:
            symbol = symbols.get(element)
            if not symbol:
                errors.append(f"output port element '{element}' is missing from backend symbols")
            elif symbol.driver == "<none>":
                errors.append(f"output port element '{element}' has no backend driver")

    for target in json_assignment_targets:
        symbol = symbols.get(target)
        if not symbol:
            errors.append(f"assignment target '{target}' is missing from backend symbols")
        elif symbol.driver == "<none>":
            errors.append(f"assignment target '{target}' has no backend driver")

    backend_source_drivers = [
        symbol.name for symbol in symbols.values() if " source#" in symbol.driver
    ]
    if len(backend_source_drivers) != len(json_assignment_targets):
        errors.append(
            "backend source assignment driver count "
            f"{len(backend_source_drivers)} != JSON assignment count {len(json_assignment_targets)}"
        )

    for var_name in sorted(vars_in_exprs):
        if var_name not in symbols:
            errors.append(f"expression variable '{var_name}' is missing from backend symbols")

    for symbol_name, symbol in sorted(symbols.items()):
        if "array_dims=[" in symbol.type_text or "array_size=" in symbol.type_text:
            errors.append(f"scalar symbol table contains array-shaped symbol '{symbol_name}': {symbol.type_text}")

    report_lines = [
        f"# {name}",
        "",
        f"- source: `{fixture.relative_to(root)}`",
        f"- json: `{json_path.relative_to(root) if json_path.is_relative_to(root) else json_path}`",
        f"- backend: `{backend_path.relative_to(root) if backend_path.is_relative_to(root) else backend_path}`",
        f"- source params: {', '.join(sorted(source_params)) if source_params else '<unparsed>'}",
        f"- json inputs: {', '.join(sorted(json_inputs)) or '<none>'}",
        f"- json outputs: {', '.join(sorted(json_outputs)) or '<none>'}",
        f"- backend ports: {len(ports)}",
        f"- json assignments: {len(json_assignment_targets)}",
        f"- backend symbols: {len(symbols)}",
        f"- backend source assignment drivers: {len(backend_source_drivers)}",
        f"- backend skipped: {'yes' if False else 'no'}",
        "",
        "## Result",
        "",
        "PASS" if not errors else "FAIL",
    ]
    if errors:
        report_lines.extend(["", "## Errors", ""])
        report_lines.extend(f"- {error}" for error in errors)
    if warnings:
        report_lines.extend(["", "## Warnings", ""])
        report_lines.extend(f"- {warning}" for warning in warnings)
    report_lines.extend(["", "## Backend Ports", ""])
    for port in ports.values():
        report_lines.append(
            f"- `{port.name}` [{port.direction}]: elements `{', '.join(port.elements)}`"
        )
    report_lines.extend(["", "## Backend Symbols", ""])
    for symbol in symbols.values():
        flags = " ".join(f"[{flag}]" for flag in sorted(symbol.flags))
        report_lines.append(f"- `{symbol.name}` {flags}: `{symbol.driver}`")
    report_path.write_text("\n".join(report_lines) + "\n", encoding="utf-8")

    return FixtureResult(name, fixture, json_path, backend_path, report_path, errors, warnings)


def write_summary(root: Path, output_dir: Path, results: list[FixtureResult]) -> None:
    lines = [
        "# Backend IR Fixture Analysis",
        "",
        f"- fixtures: {len(results)}",
        f"- passed: {sum(1 for result in results if not result.errors)}",
        f"- failed: {sum(1 for result in results if result.errors)}",
        f"- backend skipped: {sum(1 for result in results if result.skipped_backend)}",
        "",
        "| Fixture | Result | Errors | Warnings | Backend | Report |",
        "| --- | --- | ---: | ---: | --- | --- |",
    ]
    for result in results:
        rel_report = result.report_path.relative_to(root) if result.report_path.is_relative_to(root) else result.report_path
        lines.append(
            f"| `{result.name}` | {'PASS' if not result.errors else 'FAIL'} | "
            f"{len(result.errors)} | {len(result.warnings)} | "
            f"{'skipped' if result.skipped_backend else 'parsed'} | `{rel_report}` |"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=None)
    parser.add_argument("--backend-build-dir", type=Path, default=None)
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--top", default="hls_main")
    parser.add_argument("--keep-going", action="store_true", help="continue after a fixture command fails")
    args = parser.parse_args()

    root = args.root.resolve()
    build_dir = (args.build_dir or (root / "build")).resolve()
    backend_build_dir = (args.backend_build_dir or (build_dir / "backend")).resolve()
    output_dir = (args.output_dir or (build_dir / "backend_ir_analysis")).resolve()

    if not shutil.which("cmake"):
        raise RuntimeError("cmake was not found in PATH")

    predicate_expand = configure_and_build(root, root, build_dir, "predicate-expand")
    backend = configure_and_build(root, root / "backend", backend_build_dir, "rtlzz-backend")

    fixtures = positive_fixtures(root)
    if not fixtures:
        raise RuntimeError("no positive fixtures found under tests/fixtures")

    results: list[FixtureResult] = []
    failed_commands = 0
    for fixture in fixtures:
        name = fixture_name(fixture)
        print(f"[fixture] {name}")
        try:
            result = analyze_fixture(root, predicate_expand, backend, fixture, args.top, output_dir)
            results.append(result)
            status = "PASS" if not result.errors else "FAIL"
            print(f"  {status}: {result.report_path}")
        except Exception as exc:
            failed_commands += 1
            print(f"  ERROR: {exc}", file=sys.stderr)
            if not args.keep_going:
                raise

    write_summary(root, output_dir, results)
    print(f"\nsummary: {output_dir / 'summary.md'}")

    failed_analyses = sum(1 for result in results if result.errors)
    if failed_commands or failed_analyses:
        print(f"failed commands: {failed_commands}, failed analyses: {failed_analyses}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
