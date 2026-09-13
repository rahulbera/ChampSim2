"""Behavioral checks for dependency-free mode and build identity invalidation."""
import json
import os
import time
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[2] / 'config' / 'ramulator2_build.py'


class RamulatorBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = shutil.which('g++') or shutil.which('clang++')
        if not self.compiler:
            self.skipTest('a C++ compiler is required for build stamp checks')

    def run_helper(self, *args):
        return subprocess.run([sys.executable, str(HELPER), '--obj', str(self.root / 'objects'),
                               '--cxx', self.compiler, *args], text=True, capture_output=True)

    def test_disabled_mode_never_needs_a_native_checkout(self):
        result = self.run_helper('--mode=0', '--root=/nonexistent/native/checkout')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / 'objects' / 'ramulator2-native').exists())
        stamp = self.root / 'objects' / 'compiler.stamp'
        before = stamp.stat().st_mtime_ns
        self.assertEqual(self.run_helper('--mode=0').returncode, 0)
        self.assertEqual(stamp.stat().st_mtime_ns, before)
        self.assertEqual(self.run_helper('--mode=0', '--flags=-O1').returncode, 0)
        self.assertNotEqual(stamp.stat().st_mtime_ns, before)
        self.assertEqual(json.loads(stamp.read_text())['flags'], '-O1')

    def test_make_dry_run_does_not_prepare_build_artifacts(self):
        objects = self.root / 'dry-run-objects'
        result = subprocess.run(['make', '-n', 'ramulator2', 'WITH_RAMULATOR2=0',
                                 f'OBJ_ROOT={objects}', f'CXX={self.compiler}'],
                                cwd=HELPER.parent.parent, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(objects.exists(), 'dry-run must not create compiler/native stamps')

    def test_fresh_enabled_dry_run_prints_missing_dependencies_without_building(self):
        objects = self.root / 'enabled-dry-run'
        result = subprocess.run(['make', '-n', 'all', 'WITH_RAMULATOR2=1',
                                 f'RAMULATOR2_ROOT={self.root / "missing-native"}',
                                 f'OBJ_ROOT={objects}', f'CXX={self.compiler}'],
                                cwd=HELPER.parent.parent, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(objects.exists())

    def test_long_make_options_do_not_disable_normal_preparation(self):
        objects = self.root / 'normal-objects'
        result = subprocess.run(['make', '--no-print-directory', 'ramulator2',
                                 'WITH_RAMULATOR2=0', f'OBJ_ROOT={objects}', f'CXX={self.compiler}'],
                                cwd=HELPER.parent.parent, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)  # target explains enablement
        self.assertTrue((objects / 'compiler.stamp').exists())

    def test_effective_abi_changes_are_rejected_in_flags_response_and_include_files(self):
        forced = self.root / 'forced.h'
        forced.write_text('#define _GLIBCXX_DEBUG 1\n')
        inner = self.root / 'inner.options'
        inner.write_text('-D_GLIBCXX_DEBUG\n')
        outer = self.root / 'outer.options'
        outer.write_text(f'@{inner}\n')
        for flags in ('-D_GLIBCXX_DEBUG', '-D_GLIBCXX_USE_CXX11_ABI=0',
                      f'@{outer}', f'-include {forced}', '-fpack-struct=1'):
            with self.subTest(flags=flags):
                result = self.run_helper('--mode=1', '--check-abi', f'--flags={flags}')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('incompatible C++ ABI', result.stderr)
        compatible = self.run_helper('--mode=1', '--check-abi', '--flags=-O1 -DUNRELATED_BUILD_OPTION=1')
        self.assertEqual(compatible.returncode, 0, compatible.stderr)

    def test_no_execute_modes_keep_newer_header_dependencies_without_remaking_them(self):
        workspace = self.root / 'dependency-edges'
        (workspace / 'src').mkdir(parents=True)
        (workspace / '.csconfig').mkdir()
        shutil.copyfile(HELPER.parent.parent / 'Makefile', workspace / 'Makefile')
        files = {'_configuration.mk': 'executable_name :=\n', 'global.options': '',
                 'absolute.options': '', '.csconfig/compiler.stamp': '',
                 'src/probe.cc': '', 'changed-header.h': '', '.csconfig/probe.o': '',
                 '.csconfig/probe.d': '.csconfig/probe.o .csconfig/probe.d: changed-header.h\n'}
        base = time.time() - 100
        for name, content in files.items():
            path = workspace / name
            path.write_text(content)
            os.utime(path, (base, base))
        obj = workspace / '.csconfig/probe.o'
        dep = workspace / '.csconfig/probe.d'
        os.utime(obj, (base + 10, base + 10))
        os.utime(workspace / 'changed-header.h', (base + 20, base + 20))
        dep_time = dep.stat().st_mtime_ns
        for flag, expected in (('-q', 1), ('-n', 0), ('-t', 0)):
            with self.subTest(flag=flag):
                result = subprocess.run(['make', flag, '.csconfig/probe.o', f'CXX={self.compiler}'],
                                        cwd=workspace, text=True, capture_output=True)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertEqual(dep.stat().st_mtime_ns, dep_time, 'dependency file must not be remade')
                if flag == '-n':
                    self.assertIn(' -c ', result.stdout)
                if flag == '-t':
                    self.assertGreater(obj.stat().st_mtime, base + 20)

    def test_enabled_missing_root_fails_before_building(self):
        result = self.run_helper('--mode=1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('RAMULATOR2_ROOT', result.stderr)
        self.assertFalse((self.root / 'objects' / 'ramulator2-native').exists())

    def test_unpinned_revision_cannot_be_certified_from_an_existing_library(self):
        native = self.root / 'native'
        native.mkdir()
        subprocess.run(['git', 'init', '-q', str(native)], check=True)
        (native / 'libramulator.so').write_bytes(b'unverified existing binary')
        subprocess.run(['git', '-C', str(native), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(native), '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                        'commit', '-qm', 'fixture'], check=True)
        result = self.run_helper('--mode=1', '--root', str(native))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsupported native revision', result.stderr)
        self.assertFalse((self.root / 'objects' / 'ramulator2-native').exists())


if __name__ == '__main__':
    unittest.main()
