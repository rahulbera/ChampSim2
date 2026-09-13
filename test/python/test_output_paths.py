"""Run the CLI against disposable inputs: output validation must never erase them."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()


@unittest.skipUnless(BINARY.is_file(), "build bin/champsim to exercise output path validation")
class OutputPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run([str(BINARY), "--knobs"], text=True, capture_output=True, check=True, timeout=30)
        cls.cores = len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.frequency\s*=", result.stdout, re.MULTILINE))
        if not cls.cores:
            raise AssertionError("--knobs did not identify any configured cores")

    def test_optional_output_cannot_truncate_a_consumed_trace_argument(self):
        for option in ("--toml", "--toml="):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as tmp:
                trace = Path(tmp) / "trace.input"
                trace.write_bytes(b"a disposable trace that must survive")
                result = subprocess.run([str(BINARY), option, str(trace)], capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(trace.read_bytes(), b"a disposable trace that must survive")
                self.assertIn("trace(s)", result.stderr)

    def test_output_cannot_alias_a_trace_by_name_symlink_or_hardlink(self):
        for alias in ("name", "relative", "symlink", "hardlink"):
            with self.subTest(alias=alias), tempfile.TemporaryDirectory() as tmp:
                trace = Path(tmp) / "trace.input"
                trace.write_bytes(b"a disposable trace that must survive")
                output = Path(tmp) / "output.toml"
                if alias == "name":
                    output = trace
                elif alias == "relative":
                    output = Path("trace.input")
                elif alias == "symlink":
                    output.symlink_to(trace)
                else:
                    output.hardlink_to(trace)
                argv = [str(BINARY), "--toml", str(output), "--", *([str(trace)] * self.cores)]
                result = subprocess.run(argv, cwd=tmp, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(trace.read_bytes(), b"a disposable trace that must survive")
                self.assertIn("aliases an input trace", result.stderr)
