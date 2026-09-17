#!/usr/bin/env python3
"""Check loop-unrolled pick writes structurally and against the C++ oracle.

Returns nonzero until both static-slice specialization and write semantics pass.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source = ROOT / 'testv2/fixtures/int_misc.logic.cpp'
    with tempfile.TemporaryDirectory(prefix='rtlzz_unrolled_pick_') as temp:
        output = Path(temp) / 'program.beir'
        subprocess.run([str(build / 'predicate-expand'), str(source), '--top',
                        'hls_main', '--format', 'beir', '-o', str(output)], check=True)
        text = output.read_text()
        # Walk only the new output cones: int_misc also contains intentional
        # runtime reads unrelated to loop-unrolled slicing.
        nodes = {}
        names = {}
        for block in re.split(r'(?=^  signal #)', text, flags=re.MULTILINE):
            match = re.match(r'  signal #(\d+) (\w+)', block)
            if not match:
                continue
            node, name = int(match[1]), match[2]
            kind = re.search(r'^    driver (\w+)', block, re.MULTILINE)
            deps = re.findall(r'^      operand\d+ = symbol\(#(\d+)', block, re.MULTILINE)
            nodes[node] = (kind[1] if kind else '', list(map(int, deps)))
            names[name] = node

        def operations(name):
            pending = [names[name]]
            seen = set()
            kinds = []
            while pending:
                node = pending.pop()
                if node in seen:
                    continue
                seen.add(node)
                kind, deps = nodes[node]
                kinds.append(kind)
                pending.extend(deps)
            return kinds

        structure_ok = True
        for name in ('pick_constant', 'pick_static', 'pick_ordered',
                     'pick_overlap', 'pick_edges', 'pick_full'):
            ok = not any(kind.startswith('dynamic_') for kind in operations(name))
            print(f"{'PASS' if ok else 'FAIL'} static output: {name}", flush=True)
            structure_ok = structure_ok and ok
        for name, kind in (('pick_dynamic', 'dynamic_write_slice'),
                           ('pick_bit', 'dynamic_write_bit')):
            ok = operations(name).count(kind) == 1
            print(f"{'PASS' if ok else 'FAIL'} runtime control: {name}", flush=True)
            structure_ok = structure_ok and ok
        rtl = Path(temp) / 'program.sv'
        subprocess.run([str(build / 'predicate-expand'), str(source), '--top',
                        'hls_main', '--format', 'rtl', '-o', str(rtl)], check=True)
        code = re.sub(r'//[^\n]*', '', rtl.read_text())
        ok = "assign pick_constant = 128'h3000000020000000100000000;" in code
        print(f"{'PASS' if ok else 'FAIL'} folded 128-bit constant", flush=True)
        structure_ok = structure_ok and ok
    diff = subprocess.run([sys.executable, str(ROOT / 'scripts/differential_rtl.py'),
                           str(source), '--top', 'hls_main', '--build-dir', str(build),
                           '--cases', '100', '--keep'], cwd=ROOT)
    return 0 if structure_ok and diff.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
