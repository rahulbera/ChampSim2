import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

from compare_optimization import fingerprint


class ParityTests(unittest.TestCase):
    def test_counter_change_cannot_hide_behind_a_stale_cached_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'stats.toml'
            record = {'cwd': directory, 'warmup_instructions': 10, 'roi_instructions': 20,
                      'warmup_cycles': 12, 'roi_cycles': 40, 'phase_sha256': 'unchanged-stale-record'}
            text = '[config]\ndram-model = "legacy"\n[phase.Simulation.roi.cpu0]\nmisses = {}\n'
            path.write_text(text.format(7))
            before = fingerprint(record)
            path.write_text(text.format(8))
            self.assertNotEqual(before, fingerprint(record))


class InputValidationTests(unittest.TestCase):
    def test_empty_or_non_list_manifest_is_rejected_before_output_creation(self):
        script = Path(__file__).with_name('compare_optimization.py')
        cpu = str(min(os.sched_getaffinity(0)))
        for manifest_value in ([], {}):
            for regression in (False, True):
                with self.subTest(manifest=manifest_value, regression=regression):
                    with tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        manifest = root / 'traces.json'
                        output = root / 'output'
                        manifest.write_text(json.dumps(manifest_value))
                        command = [sys.executable, str(script), '--before', sys.executable,
                                   '--after', sys.executable, '--traces', str(manifest),
                                   '--output', str(output), '--cpu', cpu]
                        if regression:
                            command.append('--regression')
                        result = subprocess.run(command, cwd=script.parent,
                                                capture_output=True, text=True)
                        self.assertEqual(result.returncode, 2)
                        self.assertIn('trace manifest must contain a non-empty list of traces',
                                      result.stderr)
                        self.assertNotIn('Traceback', result.stderr)
                        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
