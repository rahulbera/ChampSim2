import os
from pathlib import Path
import sys
import tempfile
import unittest

import config.pgo


@unittest.skipUnless(sys.version_info >= (3, 11), 'the native check reads TOML with tomllib (Python 3.11+)')
class NativeConfigOverlayTests(unittest.TestCase):
    """pgo-check times each simulation in its own directory, where a relative ramulator2.config
    no longer names the YAML that training read from the source root."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.work = self.root / 'work'
        self.work.mkdir()
        previous = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)

    def write(self, name, text):
        (self.root / name).write_text(text)
        return name

    def test_relative_yaml_is_resolved_against_the_source_root(self):
        core = self.write('core.toml', '[ooo_cpu.cpu0]\nrob_size = 512\n')
        native = self.write('native.toml', 'dram-model = "ramulator2"\n[ramulator2]\nconfig = "yaml/ddr4.yaml"\n')
        overlays = config.pgo.native_config_overlay([core, native], self.work)
        self.assertEqual(len(overlays), 1)
        self.assertEqual(Path(overlays[0]).parent, self.work)
        self.assertEqual(Path(overlays[0]).read_text(), f'[ramulator2]\nconfig = "{self.root / "yaml/ddr4.yaml"}"\n')

    def test_the_last_source_wins_and_absolute_paths_need_nothing(self):
        first = self.write('first.toml', '[ramulator2]\nconfig = "a.yaml"\n')
        second = self.write('second.toml', '[ramulator2]\nconfig = "/abs/b.yaml"\n')
        self.assertEqual(config.pgo.native_config_overlay([first, second], self.work), [])
        overlays = config.pgo.native_config_overlay([second, first], self.work)
        self.assertIn(str(self.root / 'a.yaml'), Path(overlays[0]).read_text())

    def test_statistics_document_and_legacy_sources(self):
        document = self.write('run.toml', '[meta]\nschema_version = 2\n[config.ramulator2]\nconfig = "c.yaml"\n')
        self.assertIn(str(self.root / 'c.yaml'), Path(config.pgo.native_config_overlay([document], self.work)[0]).read_text())
        legacy = self.write('legacy.toml', '[pmem]\nchannels = 2\n')
        self.assertEqual(config.pgo.native_config_overlay([legacy], self.work), [])


if __name__ == '__main__':
    unittest.main()
