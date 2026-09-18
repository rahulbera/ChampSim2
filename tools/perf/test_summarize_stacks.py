import json
from pathlib import Path
import tempfile
import unittest

from summarize_stacks import summarize


class StackAccountingTests(unittest.TestCase):
    def test_component_partition_does_not_double_count_allocator_children(self):
        stacks = [
            ['__GI___libc_malloc', 'operator new(unsigned long)', 'CACHE::operate()', 'champsim::do_phase(example)'],
            ['champsim::bandwidth::has_remaining() const', 'O3_CPU::execute_instruction()', 'O3_CPU::operate()', 'champsim::do_phase(example)'],
            ['champsim::static_environment::cpu_view()', 'champsim::do_phase(example)'],
            ['loader'],
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'stacks.json'
            path.write_text(json.dumps({'samples': [{'stack': [{'function': name} for name in stack]} for stack in stacks]}))
            result = summarize(path)
        self.assertEqual(result['samples'], 4)
        for key in ('core', 'cache', 'framework', 'startup_finish_other', 'allocator_union', 'bandwidth_exclusive', 'rob_scan_union'):
            self.assertEqual(result[key], 25, key)
        self.assertEqual(sum(result[k] for k in ('core', 'cache', 'ptw', 'dram', 'trace', 'framework', 'startup_finish_other')), 100)


if __name__ == '__main__':
    unittest.main()
