"""Stage 4 statistics naming: one context keeps the flat names, several nest."""

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


@unittest.skipUnless(BINARY.is_file(), "build the ChampSim executable first")
class ContextNamingTests(unittest.TestCase):
    def knobs(self, *settings):
        command = [str(BINARY), "--knobs"]
        for setting in settings:
            command.extend(["--set", setting])
        return subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=30)

    def cores(self):
        listing = self.knobs().stdout
        return len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.frequency\s*=", listing, re.MULTILINE))

    def run_binary(self, traces, settings, heartbeat=False):
        command = [str(BINARY), "--trace-version", "2", "-w", "0", "-i", "1000"]
        command.append("--heartbeat-frequency" if heartbeat else "--hide-heartbeat")
        if heartbeat:
            command.append("500")
        toml_path = Path(traces[0]).parent / "stats.toml"
        command.extend(["--toml", str(toml_path)])
        for setting in settings:
            command.extend(["--set", setting])
        command.extend(["--", *map(str, traces)])
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result, tomllib.loads(toml_path.read_text())

    def test_single_context_keeps_flat_names(self):
        cores = self.cores()
        with tempfile.TemporaryDirectory() as tmp:
            traces = []
            for k in range(cores):
                path = Path(tmp) / f"trace{k}.champsim2"
                generate(path, 4096)
                traces.append(path)

            result, doc = self.run_binary(traces, [])

            trace = doc["phase"]["simulation"]["trace"]
            roi_core = doc["phase"]["simulation"]["roi"]["core"]
            config_core = doc["config"]["ooo_cpu"]
            for k in range(cores):
                key = f"cpu{k}"
                # The trace row is flat: cpuK maps to the path, not a context table.
                self.assertIn(key, trace)
                self.assertIsInstance(trace[key], str)
                self.assertIn(key, roi_core)
                self.assertIn(key, config_core)

            # The plain report keeps the bare CPU K name, never a context suffix.
            self.assertIn("CPU 0", result.stdout)
            self.assertNotIn(" ctx ", result.stdout)

    def test_multi_context_nests_every_core(self):
        cores = self.cores()
        with tempfile.TemporaryDirectory() as tmp:
            traces = []
            settings = []
            for k in range(cores):
                for c in range(2):
                    path = Path(tmp) / f"trace{k}c{c}.champsim2"
                    generate(path, 4096)
                    traces.append(path)
                settings.append(f"ooo_cpu.cpu{k}.num_contexts=2")

            result, doc = self.run_binary(traces, settings, heartbeat=True)

            trace = doc["phase"]["simulation"]["trace"]
            roi_core = doc["phase"]["simulation"]["roi"]["core"]
            config_core = doc["config"]["ooo_cpu"]

            # The core set agrees across the trace, statistics and configuration
            # tables, and holds exactly the configured cores.
            self.assertEqual(set(trace), {f"cpu{k}" for k in range(cores)})
            self.assertEqual(set(trace), set(roi_core))
            self.assertEqual(set(trace), set(config_core))

            # Two contexts per core, keyed ctx0/ctx1 under the core in the trace
            # and in the statistics, context 0 included.
            for k in range(cores):
                key = f"cpu{k}"
                self.assertEqual(set(trace[key]), {"ctx0", "ctx1"})
                self.assertEqual(set(roi_core[key]), {"ctx0", "ctx1"})

            # Every context is named on its own progress line, and the heartbeat
            # labels the multi-context core.
            for k in range(cores):
                for c in range(2):
                    self.assertIn(f"CPU {k} ctx {c}", result.stdout)
            self.assertIn("Heartbeat CPU 0 (2 contexts)", result.stdout)


if __name__ == "__main__":
    unittest.main()
