"""Behavioral checks for dependency-free mode and build identity invalidation."""
import importlib.util
import json
import os
import re
import time
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[2] / 'config' / 'ramulator2_build.py'
SANITIZER_OPTIONS = '-fsanitize=address,undefined -fno-omit-frame-pointer'


def tree(path):
    """Every path below `path`, without descending into symbolic links."""
    return sorted(str(Path(parent, name).relative_to(path))
                  for parent, dirs, files in os.walk(path) for name in dirs + files)


def nested_make_environment():
    """The caller's environment without what would configure a nested make.

    `make pytest WITH_RAMULATOR2=1` passes its variables to child makes through
    MAKEFLAGS, and CI sets them in the environment; either would override the
    variables a dry run is meant to control.
    """
    hidden = {'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'MAKEOVERRIDES', 'WITH_RAMULATOR2', 'RAMULATOR2_ROOT',
              'RAMULATOR2_SANITIZE', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS'}
    return {name: value for name, value in os.environ.items() if name not in hidden}


def load_helper():
    spec = importlib.util.spec_from_file_location('ramulator2_build_under_test', HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
        # Goal `all` hard-includes _configuration.mk, which only config.sh writes, so
        # a fresh checkout (the hosted python job) cannot dry-run the repository root.
        # Dry-run the real Makefile and helper beside a stub fragment and source instead.
        workspace = self.root / 'fresh-checkout'
        (workspace / 'src').mkdir(parents=True)
        shutil.copyfile(HELPER.parent.parent / 'Makefile', workspace / 'Makefile')
        (workspace / 'config').symlink_to(HELPER.parent, target_is_directory=True)
        for name, content in {'_configuration.mk': 'executable_name := bin/champsim\n',
                              'global.options': '', 'src/ramulator2_driver.cc': ''}.items():
            (workspace / name).write_text(content)
        before = tree(workspace)
        native = self.root / 'missing-native'
        objects = self.root / 'enabled-dry-run'
        result = subprocess.run(['make', '-n', 'all', 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}',
                                 f'OBJ_ROOT={objects}', f'CXX={self.compiler}'],
                                cwd=workspace, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{HELPER.name} --mode='1' --root='{native}'", result.stdout)
        self.assertIn('--check-abi', result.stdout)
        self.assertIn(f'-std=c++20 -c -o {objects / "ramulator2_driver.o"}', result.stdout)
        self.assertIn(str(native / 'libramulator.so'), result.stdout)
        self.assertFalse(objects.exists())
        self.assertFalse(native.exists())
        self.assertEqual(tree(workspace), before, 'dry-run must not create files')

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

    def sanitizer_workspace(self):
        """The real Makefile and helper beside stub simulator and test sources."""
        workspace = self.root / 'stub-checkout'
        for directory in ('src', 'test/cpp/src'):
            (workspace / directory).mkdir(parents=True)
        shutil.copyfile(HELPER.parent.parent / 'Makefile', workspace / 'Makefile')
        (workspace / 'config').symlink_to(HELPER.parent, target_is_directory=True)
        for name in ('src/ramulator2_driver.cc', 'src/probe.cc', 'test/cpp/src/000-test-main.cc',
                     'test/cpp/src/001-probe.cc', 'global.options'):
            (workspace / name).write_text('')
        (workspace / '_configuration.mk').write_text('executable_name := bin/champsim\n')
        return workspace

    def dry_run(self, workspace, *variables):
        return subprocess.run(['make', '-n', 'all', 'test/bin/000-test-main', f'OBJ_ROOT={self.root / "dry-objects"}',
                               f'CXX={self.compiler}', *variables], cwd=workspace, text=True, capture_output=True,
                              env=nested_make_environment())

    def test_release_native_build_commands_and_mode_are_unchanged(self):
        helper = load_helper()
        release = helper.native_settings(sanitize=False)
        self.assertEqual(helper.cmake_commands(Path('/native'), Path('/build'), '/usr/bin/g++', release), [
            ['cmake', '-S', '/native', '-B', '/build', '-DRAMULATOR_PYTHON_BINDINGS=OFF', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_CXX_FLAGS=', '-DCMAKE_SHARED_LINKER_FLAGS=',
             '-DCMAKE_EXE_LINKER_FLAGS='],
            ['cmake', '--build', '/build', '--target', 'ramulator', '--clean-first', '-j6']])
        self.assertEqual(helper.mode_string(release), 'Release C++20 Python=OFF')
        identity = helper.compiler_identity([self.compiler], '  ', sanitize=False)
        self.assertEqual(sorted(identity), ['command', 'flags', 'path', 'version'])
        record = json.loads(helper.build_record(identity, helper.DEPENDENCIES, release))
        self.assertEqual(record, {'compiler': identity, 'dependencies': helper.DEPENDENCIES,
                                  'mode': 'Release C++20 Python=OFF'})

    def test_sanitized_native_build_is_instrumented_and_recorded(self):
        helper = load_helper()
        sanitized = helper.native_settings(sanitize=True)
        configure, build = helper.cmake_commands(Path('/native'), Path('/build'), '/usr/bin/g++', sanitized)
        self.assertIn('-DCMAKE_BUILD_TYPE=RelWithDebInfo', configure)
        self.assertIn(f'-DCMAKE_CXX_FLAGS={SANITIZER_OPTIONS}', configure)
        self.assertIn(f'-DCMAKE_SHARED_LINKER_FLAGS={SANITIZER_OPTIONS}', configure)
        self.assertNotIn('-DCMAKE_BUILD_TYPE=Release', configure)
        self.assertEqual(build[-3:], ['ramulator', '--clean-first', '-j6'])
        mode = helper.mode_string(sanitized)
        self.assertEqual(mode, 'RelWithDebInfo C++20 Python=OFF Sanitizers=address,undefined')
        identity = helper.compiler_identity([self.compiler], SANITIZER_OPTIONS, sanitize=True)
        self.assertEqual(identity['sanitizers'], 'address,undefined')
        # The build string is what statistics documents report as meta.ramulator2.build.
        record = json.loads(helper.build_record(identity, helper.DEPENDENCIES, sanitized))
        self.assertEqual(record['mode'], mode)
        self.assertEqual(record['compiler']['sanitizers'], 'address,undefined')
        inputs = helper.manifest_inputs(Path('/native'), helper.REVISION, identity, sanitized)
        self.assertEqual(inputs['native'], {'build_type': 'RelWithDebInfo', 'cxx_flags': SANITIZER_OPTIONS,
                                            'sanitizers': 'address,undefined',
                                            'shared_linker_flags': SANITIZER_OPTIONS})

    def test_flipping_sanitizers_invalidates_the_native_library_and_host_objects(self):
        helper = load_helper()
        release, sanitized = helper.native_settings(sanitize=False), helper.native_settings(sanitize=True)
        plain = helper.compiler_identity([self.compiler], '  ', sanitize=False)
        instrumented = helper.compiler_identity([self.compiler], f' {SANITIZER_OPTIONS} -g  {SANITIZER_OPTIONS}',
                                                sanitize=True)
        # Every host object depends on the stamp holding this identity, and the
        # mode alone changes it even if the host flags were spelled identically.
        self.assertNotEqual(plain, instrumented)
        self.assertNotEqual(plain, helper.compiler_identity([self.compiler], '  ', sanitize=True))
        before = helper.manifest_inputs(Path('/native'), helper.REVISION, plain, release)
        after = helper.manifest_inputs(Path('/native'), helper.REVISION, instrumented, sanitized)
        for prior, current in ((before, after), (after, before)):
            with self.subTest(prior=prior['native']['build_type']):
                self.assertTrue(helper.manifest_matches({'inputs': prior, 'sha256': 'digest'}, prior, 'digest'))
                self.assertFalse(helper.manifest_matches({'inputs': prior, 'sha256': 'digest'}, current, 'digest'))
        # Native settings invalidate the library on their own, even with identical host flags.
        self.assertFalse(helper.manifest_matches(
            {'inputs': helper.manifest_inputs(Path('/native'), helper.REVISION, plain, release), 'sha256': 'digest'},
            helper.manifest_inputs(Path('/native'), helper.REVISION, plain, sanitized), 'digest'))
        native = Path('/objects/ramulator2-native')
        self.assertNotEqual(helper.build_directory(native, before), helper.build_directory(native, after))

    def test_make_instruments_every_host_compile_and_link_with_the_native_library(self):
        workspace = self.sanitizer_workspace()
        native = self.root / 'missing-native'
        result = self.dry_run(workspace, 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}', 'RAMULATOR2_SANITIZE=1')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        compiles = [line for line in lines if ' -c -o ' in line]
        links = [line for line in lines if re.search(r' -o (bin/champsim|test/bin/000-test-main) ', line)]
        self.assertEqual(len(compiles), 4, result.stdout)  # shared probe and driver objects, two test sources
        self.assertEqual(len(links), 2, result.stdout)
        for line in compiles:
            self.assertIn(f'{SANITIZER_OPTIONS} -g', line)
        for line in links:
            self.assertIn(SANITIZER_OPTIONS, line)
        helper_lines = [line for line in lines if HELPER.name in line]
        self.assertTrue(helper_lines)
        for line in helper_lines:
            self.assertTrue(line.endswith(' --sanitize'), line)
        # Host flags reach the compiler stamp every object depends on.
        self.assertIn(SANITIZER_OPTIONS, next(line for line in helper_lines if '--check-abi' not in line))
        self.assertFalse(native.exists())

    def test_default_make_commands_do_not_mention_sanitizers(self):
        workspace = self.sanitizer_workspace()
        native = self.root / 'missing-native'
        unset = self.dry_run(workspace, 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}')
        explicit = self.dry_run(workspace, 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}', 'RAMULATOR2_SANITIZE=0')
        self.assertEqual(unset.returncode, 0, unset.stderr)
        self.assertEqual(explicit.returncode, 0, explicit.stderr)
        self.assertEqual(unset.stdout, explicit.stdout)
        self.assertNotIn('-fsanitize', unset.stdout)
        self.assertNotIn('--sanitize', unset.stdout)
        self.assertIn(f"{HELPER.name} --mode='1' --root='{native}'", unset.stdout)

    def test_sanitizers_require_the_native_build(self):
        result = self.run_helper('--mode=0', '--sanitize')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('WITH_RAMULATOR2=1', result.stderr)
        self.assertFalse((self.root / 'objects').exists(), 'rejection must precede every stamp')
        workspace = self.sanitizer_workspace()
        for variables, message in ((['RAMULATOR2_SANITIZE=1'], 'WITH_RAMULATOR2=1'),
                                   (['WITH_RAMULATOR2=0', 'RAMULATOR2_SANITIZE=1'], 'WITH_RAMULATOR2=1'),
                                   (['WITH_RAMULATOR2=1', 'RAMULATOR2_SANITIZE=yes'], 'must be 0 or 1')):
            with self.subTest(variables=variables):
                result = self.dry_run(workspace, *variables)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('RAMULATOR2_SANITIZE', result.stderr)
                self.assertIn(message, result.stderr)
        self.assertFalse((self.root / 'dry-objects').exists())

    def test_sanitized_abi_probe_accepts_instrumented_driver_options(self):
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / 'probe'
            source = Path(directory) / 'probe.cc'
            source.write_text('int main() {}\n')
            built = subprocess.run([self.compiler, *SANITIZER_OPTIONS.split(), str(source), '-o', str(program)],
                                   capture_output=True)
            if built.returncode != 0 or subprocess.run([str(program)], capture_output=True).returncode != 0:
                self.skipTest('this compiler cannot build and run AddressSanitizer programs here')
        result = self.run_helper('--mode=1', '--check-abi', '--sanitize', f'--flags={SANITIZER_OPTIONS} -g -O1')
        self.assertEqual(result.returncode, 0, result.stderr)

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
