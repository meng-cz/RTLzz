"""Regression tests for observable write-data pairing in the RTL differential harness."""
import copy
import unittest

from differential_rtl import cpp_value_expr, write_data_guards


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


if __name__ == '__main__':
    unittest.main()
