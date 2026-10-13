"""Regression tests for observable write-data pairing in the RTL differential harness."""
import copy
import unittest
import tempfile
from pathlib import Path

from differential_rtl import cpp_value_expr, write_data_guards, generate_verilator_harness, parse_stream_results


def port(name, width, dims=(), symbols=None, direction='Output'):
    return {'name': name, 'direction': direction,
            'type': {'width': width, 'array_dims': list(dims)},
            'element_symbols': symbols or [name]}


class WriteDataGuardsTest(unittest.TestCase):
    def test_normalized_integer_output_uses_original_cpp_type(self):
        normalized = {'name': 'Int<32>', 'width': 32, 'hw_kind': 'Int'}
        self.assertNotIn('.template to', cpp_value_expr('result', normalized, 'uint32_t'))
        self.assertIn('.template to', cpp_value_expr('result', normalized, 'Int<32>'))

    def test_scalar_and_array_pairs(self):
        program = {'ports': [port('wdata_r', 32), port('wen_r', 1),
                             port('wdata_a', 8, (2, 2), ['d0', 'd1', 'd2', 'd3']),
                             port('wen_a', 1, (2, 2), ['e0', 'e1', 'e2', 'e3']),
                             port('resetvalue_r', 32)]}
        self.assertEqual(write_data_guards(program),
                         {'wdata_r': 'wen_r', **{f'd{i}': f'e{i}' for i in range(4)}})

    def test_only_exact_output_pairs(self):
        base = {'ports': [port('wdata_r', 32, (2,), ['d0', 'd1']),
                          port('wen_r', 1, (2,), ['e0', 'e1'])]}
        for field, value in [('direction', 'Input'), ('width', 2),
                             ('array_dims', [1, 2]), ('element_symbols', ['e0'])]:
            program = copy.deepcopy(base)
            target = program['ports'][1]
            (target['type'] if field in ('width', 'array_dims') else target)[field] = value
            with self.subTest(field=field):
                self.assertEqual(write_data_guards(program), {})

    def test_other_observers_remain_unconditional(self):
        program = {'ports': [port('wdata_r', 32), port('wen_r', 1),
                             port('query', 32, symbols=['wdata_r'])]}
        self.assertEqual(write_data_guards(program), {})


class PipelineHarnessTest(unittest.TestCase):
    def test_stream_markers_do_not_split_output_names(self):
        self.assertEqual(parse_stream_results("case=0\nshowcase=7\ncase=1\nshowcase=11\n", 2),
                         [{"showcase": 7}, {"showcase": 11}])
        with self.assertRaises(RuntimeError):
            parse_stream_results("case=1\nshowcase=7\n", 1)
        with self.assertRaises(RuntimeError):
            parse_stream_results("case=0\nshowcase=7\n", 2)

    def test_streaming_preserves_array_port_named_argv(self):
        inputs = port("argv", 8, (2,), ["a0", "a1"], direction="Input")
        inputs["type"]["is_array"] = True
        program = {"ports": [inputs, port("result", 8)],
                   "pipeline": {"register_stages": 2, "clock_name": "clk"}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "harness.cpp"
            order = generate_verilator_harness("top", program, path)
            generated = path.read_text()
        self.assertEqual(order, ["a0", "a1"])
        self.assertIn("top->argv[0] = std::strtoull(row_args.data()[1]", generated)
        self.assertIn("top->argv[1] = std::strtoull(row_args.data()[2]", generated)
        self.assertIn("cycle >= 3", generated)
        self.assertLess(generated.index('std::cout << "result="'),
                        generated.index("top->clk = 1; top->eval();"))


if __name__ == '__main__':
    unittest.main()
