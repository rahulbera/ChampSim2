"""A register file too small for a trace's architectural footprint is a named error.

Each architectural register a trace uses keeps a committed physical register for
the rest of the run, so a file that cannot hold them all plus one instruction's
renames stops the core for good. The run must say so, not spin until the generic
deadlock guard aborts it.
"""
import os
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('CHAMPSIM_BINARY', ROOT / 'bin' / 'champsim'))
V1_RECORD = struct.Struct('<QBB2B4B2Q4Q')  # inc/trace_instruction.h: input_instr, 64 bytes
FIRST_REGISTER = 32  # clear of the stack pointer (6), flags (25) and instruction pointer (26), which the core treats specially


def straight_line_trace(path, architectural_registers, length):
    """ALU instructions writing `architectural_registers` distinct registers in turn."""
    with open(path, 'wb') as trace:
        for i in range(length):
            register = FIRST_REGISTER + i % architectural_registers
            trace.write(V1_RECORD.pack(0x400000 + 4 * i, 0, 0, register, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))


@unittest.skipUnless(BINARY.is_file(), 'build bin/champsim or set CHAMPSIM_BINARY to exercise the core')
class RegisterFileFootprintTests(unittest.TestCase):
    def run_with_register_file(self, size):
        with tempfile.TemporaryDirectory() as scratch:
            trace = Path(scratch) / 'footprint.champsimtrace'
            straight_line_trace(trace, architectural_registers=20, length=4000)
            return subprocess.run([str(BINARY), '--set', f'ooo_cpu.cpu0.register_file_size={size}', '-w', '0', '-i', '2000', '--', str(trace)],
                                  capture_output=True, text=True, timeout=120)

    def test_too_small_for_the_footprint_is_a_named_error(self):
        result = self.run_with_register_file(16)
        self.assertEqual(result.returncode, 1, result.stderr[-2000:])
        self.assertIn('ERROR: runtime config: ooo_cpu.cpu0.register_file_size = 16 is too small for this trace', result.stderr)
        self.assertNotIn('DEADLOCK', result.stdout)

    def test_room_for_the_footprint_completes(self):
        result = self.run_with_register_file(24)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        retired = re.search(r'Simulation complete CPU 0 instructions: (\d+)', result.stdout)
        self.assertIsNotNone(retired, result.stdout[-2000:])
        self.assertGreaterEqual(int(retired.group(1)), 2000)


if __name__ == '__main__':
    unittest.main()
