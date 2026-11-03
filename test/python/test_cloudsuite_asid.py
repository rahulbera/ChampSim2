"""A CloudSuite trace runs in its core's single address space, not each record's asid.

CloudSuite records carry their own asid, but Stage 2 decides a record's asid is
not an address-space identity: the trace runs in the address space its core
stamps on it, {cpu, cpu}, exactly as v1 and v2 traces do. This test feeds the
same instruction stream under four asid patterns and under a constant {0,0}
asid; both runs must retire identically because the record asid is ignored.
"""
import os
import struct
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('CHAMPSIM_BINARY', ROOT / 'bin' / 'champsim'))
CLOUDSUITE_RECORD = struct.Struct('<QBB4B4B6x4Q4Q2B6x')  # inc/trace_instruction.h: cloudsuite_instr, 96 bytes


def page(i):
    return 0x10000000 + 4096 * i


def loop_trace(path, asid_of, load_of, store_of):
    """2000 iterations of an 8-instruction loop, each record tagged asid_of(i)."""
    with open(path, 'wb') as trace:
        for i in range(2000):
            for k in range(8):
                ip = 0x400000 + 4 * k
                is_branch = 0
                branch_taken = 0
                destination_registers = [0] * 4
                source_registers = [0] * 4
                destination_memory = [0] * 4
                source_memory = [0] * 4
                if k == 1:  # load: reads load_of(i) into register 3, base register 4
                    destination_registers[0] = 3
                    source_registers[0] = 4
                    source_memory[0] = load_of(i)
                elif k == 6:  # store: writes register 3 to store_of(i)
                    source_registers[0] = 3
                    destination_memory[0] = store_of(i)
                elif k == 7:  # branch back to the loop head, taken unless i % 16 == 15
                    is_branch = 1
                    branch_taken = 1 if i % 16 != 15 else 0
                    destination_registers[0] = 26
                    source_registers[0] = 26
                    source_registers[1] = 25
                else:  # k in {0, 2, 3, 4, 5}: plain ALU
                    destination_registers[0] = 5 + k
                    source_registers[0] = 2
                asid = asid_of(i)
                trace.write(CLOUDSUITE_RECORD.pack(
                    ip, is_branch, branch_taken,
                    *destination_registers, *source_registers,
                    *destination_memory, *source_memory,
                    asid[0], asid[1]))


# Each case names a triple of functions of the iteration: (asid, load address, store address).
CASES = {
    'high asid byte': (lambda i: (0, 16), lambda i: page(i), lambda i: page(i) + 0x40),
    'store and load across asids': (lambda i: (1 + (i // 8) % 2, 0), lambda i: 0x20000000, lambda i: 0x20000000),
    'asids sharing pages': (lambda i: (1 + i % 2, 0), lambda i: page(i // 2), lambda i: page(i // 2) + 0x40),
    'asids on disjoint pages': (lambda i: (1 + (i // 8) % 2, 0), lambda i: page(i), lambda i: page(i) + 0x40),
}


@unittest.skipUnless(BINARY.is_file(), 'build bin/champsim or set CHAMPSIM_BINARY to exercise the core')
class CloudSuiteAsidTests(unittest.TestCase):
    def run_cloudsuite(self, trace, output):
        return subprocess.run([str(BINARY), '-c', '-w', '5000', '-i', '20000', '--toml', str(output), '--', str(trace)],
                              capture_output=True, text=True, timeout=120)

    @staticmethod
    def comparable(document):
        """The two runs differ only in the trace's asid pattern, so their documents must agree
        everywhere except the volatile per-run fields: [meta] and each phase's trace names."""
        document.pop('meta', None)
        for phase in document.get('phase', {}).values():
            phase.pop('trace', None)
        return document

    def test_record_asid_is_ignored(self):
        for name, (asid_of, load_of, store_of) in CASES.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as scratch:
                trace = Path(scratch) / f'{name}.champsimtrace'
                zero_trace = Path(scratch) / f'{name}-zero.champsimtrace'
                loop_trace(trace, asid_of, load_of, store_of)
                loop_trace(zero_trace, lambda i: (0, 0), load_of, store_of)

                out = Path(scratch) / f'{name}.toml'
                zero_out = Path(scratch) / f'{name}-zero.toml'
                result = self.run_cloudsuite(trace, out)
                zero_result = self.run_cloudsuite(zero_trace, zero_out)
                self.assertEqual(result.returncode, 0, result.stderr[-2000:])
                self.assertEqual(zero_result.returncode, 0, zero_result.stderr[-2000:])

                with open(out) as document, open(zero_out) as zero_document:
                    self.assertEqual(self.comparable(tomllib.loads(document.read(), parse_float=str)),
                                     self.comparable(tomllib.loads(zero_document.read(), parse_float=str)))


if __name__ == '__main__':
    unittest.main()
