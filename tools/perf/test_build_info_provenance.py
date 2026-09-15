"""Benchmark manifests query build provenance outside timed simulation calls."""
from pathlib import Path
import tempfile
import unittest

from benchmark_ptw import build_info


class BuildProvenanceTests(unittest.TestCase):
    def test_real_binary_query_and_unsupported_predecessor(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'binary'
            binary.write_text('#!/bin/sh\nprintf \'{"compiler":{"version":"fixture"},"mode":"release"}\\n\'\n')
            binary.chmod(0o755)
            info = build_info(binary)
            self.assertTrue(info['available'])
            self.assertEqual(info['provenance']['mode'], 'release')
            binary.write_text('#!/bin/sh\necho "unsupported option" >&2\nexit 109\n')
            info = build_info(binary)
            self.assertFalse(info['available'])
            self.assertEqual(info['returncode'], 109)
            self.assertEqual(info['diagnostic'], 'unsupported option')
