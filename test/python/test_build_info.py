"""Standalone compiler provenance must be independent of simulation configuration."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BuildInfoTests(unittest.TestCase):
    def setUp(self):
        binary = os.environ.get('CHAMPSIM_TEST_BINARY')
        if not binary:
            self.skipTest('set CHAMPSIM_TEST_BINARY to a named-build candidate')
        self.binary = str(Path(binary).resolve())

    def test_standalone_json_has_compiler_and_policy(self):
        result = subprocess.run([self.binary, '--build-info'], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        info = json.loads(result.stdout)
        self.assertIn(info['mode'], ['debug', 'release', 'fast'])
        self.assertIn('target', info['compiler'])
        self.assertIn('isa_provenance', info['dependencies'])
        self.assertNotIn('build_id', info)
        self.assertEqual(result.stderr, '')

    def test_malformed_config_cannot_substitute_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'malformed.toml'
            config.write_text('[meta\n build_id = "forged"\n')
            for args in [ ['--config', str(config), '--build-info'], ['--build-info', '--config', str(config)],
                         ['--set', 'nonsense=value', '--build-info'] ]:
                result = subprocess.run([self.binary, *args], text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')

    def test_literal_option_value_is_not_build_info_switch(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, '--build-info').write_text('not TOML!')
            result = subprocess.run([self.binary, '--config', '--build-info'], cwd=directory, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            self.assertNotIn('standalone', result.stderr)
            result = subprocess.run([self.binary, '--', '--build-info'], cwd=directory, text=True, capture_output=True)
            self.assertNotIn('standalone', result.stderr)
