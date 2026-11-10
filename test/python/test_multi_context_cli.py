"""CLI coverage for the num_contexts key: knobs, range refusal, and two-context runs."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()

sys.path.insert(0, str(ROOT / "test" / "ramulator2"))
from generate_trace import generate  # noqa: E402


@unittest.skipUnless(BINARY.is_file(), "set CHAMPSIM_BINARY to exercise num_contexts")
class MultiContextCliTests(unittest.TestCase):
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

    def test_knobs_lists_num_contexts_one_per_core(self):
        listing = self.run_knobs().stdout
        self.assertEqual(
            len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.num_contexts = 1$", listing, re.MULTILINE)),
            self.cores,
        )

    def test_num_contexts_out_of_range_is_refused(self):
        for value in ("0", "3"):
            with self.subTest(value=value):
                result = self.run_knobs(f"ooo_cpu.cpu0.num_contexts={value}")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("ERROR: runtime config: ooo_cpu.cpu0.num_contexts", result.stderr)

    def test_cpu0_two_contexts_runs_with_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            traces = []
            for c in range(2):
                path = Path(tmp) / f"trace{c}.champsim2"
                generate(path, 4096)
                traces.append(path)

            toml_path = Path(tmp) / "stats.toml"
            command = [
                str(BINARY), "--trace-version", "2", "-w", "0", "-i", "1000",
                "--set", "ooo_cpu.cpu0.num_contexts=2",
                "--toml", str(toml_path),
                "--", *map(str, traces),
            ]
            result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = tomllib.loads(toml_path.read_text())
            self.assertEqual(doc["config_override"], {"ooo_cpu.cpu0.num_contexts": 2})

    def test_every_core_two_contexts_runs(self):
        if self.cores < 2:
            self.skipTest("needs more than one core to distinguish an every-core run from cpu0")

        with tempfile.TemporaryDirectory() as tmp:
            traces = []
            settings = []
            for cpu in range(self.cores):
                for c in range(2):
                    path = Path(tmp) / f"trace{cpu}c{c}.champsim2"
                    generate(path, 4096)
                    traces.append(path)
                settings.append(f"ooo_cpu.cpu{cpu}.num_contexts=2")

            toml_path = Path(tmp) / "stats.toml"
            command = [str(BINARY), "--trace-version", "2", "-w", "0", "-i", "1000", "--toml", str(toml_path)]
            for setting in settings:
                command.extend(["--set", setting])
            command.extend(["--", *map(str, traces)])

            result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = tomllib.loads(toml_path.read_text())
            self.assertEqual(doc["config_override"], {f"ooo_cpu.cpu{cpu}.num_contexts": 2 for cpu in range(self.cores)})


if __name__ == "__main__":
    unittest.main()
