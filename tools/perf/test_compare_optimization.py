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


if __name__ == '__main__':
    unittest.main()
