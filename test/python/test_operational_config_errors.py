"""CLI coverage for always-active construction-time configuration errors."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()


@unittest.skipUnless(BINARY.is_file(), "set CHAMPSIM_BINARY to exercise configuration errors")
class OperationalConfigErrorTests(unittest.TestCase):
    def run_knobs(self, *settings):
        command = [str(BINARY), "--knobs"]
        for setting in settings:
            command.extend(["--set", setting])
        return subprocess.run(command, text=True, capture_output=True, timeout=30)

    @classmethod
    def setUpClass(cls):
        result = subprocess.run([str(BINARY), "--knobs"], text=True, capture_output=True, check=True, timeout=30)
        cls.cores = len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.frequency\s*=", result.stdout, re.MULTILINE))
        if not cls.cores:
            raise AssertionError("--knobs did not identify any configured cores")

    @staticmethod
    def placeholder(directory, name):
        """A file the CLI can check exists but the run must never open."""
        path = Path(directory) / name
        path.write_bytes(b"a placeholder trace that is never opened")
        return path

    def run_with_traces(self, traces, *settings):
        return subprocess.run([str(BINARY), "-w", "0", "-i", "1000", *settings, "--", *map(str, traces)],
                              capture_output=True, text=True, timeout=30)

    def test_unrepresentable_aggregate_dram_capacity_is_a_cli_error(self):
        result = self.run_knobs("dram-model=legacy", "pmem.bank_rows=4611686018427387904")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("ERROR:", result.stderr)
        self.assertIn("DRAM address mapping capacity is not representable", result.stderr)

    def test_binding_boundaries_are_valid(self):
        result = self.run_knobs(
            "dram-model=legacy",
            "vmem.pte_page_size=2048",
            "ooo_cpu.cpu0.register_file_size=32767",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("vmem.pte_page_size = 2048", result.stdout)
        self.assertIn("ooo_cpu.cpu0.register_file_size = 32767", result.stdout)

    def test_misspelt_num_contexts_is_an_unknown_key_not_a_trace_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            traces = [self.placeholder(tmp, f"trace{index}") for index in range(self.cores + 1)]
            result = self.run_with_traces(traces, "--set", "ooo_cpu.cpu0.num_context=2")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("nothing consumed configuration key 'ooo_cpu.cpu0.num_context'", result.stderr)
            self.assertNotIn("trace(s)", result.stderr)

    def test_trace_count_follows_num_contexts(self):
        with tempfile.TemporaryDirectory() as tmp:
            traces = [self.placeholder(tmp, f"trace{index}") for index in range(self.cores)]
            result = self.run_with_traces(traces, "--set", "ooo_cpu.cpu0.num_contexts=2")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(f"ERROR: expected {self.cores + 1} trace(s), one per hardware context", result.stderr)


if __name__ == "__main__":
    unittest.main()
