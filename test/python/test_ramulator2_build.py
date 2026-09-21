"""Behavioral checks for dependency-free mode and build identity invalidation."""
import hashlib
import importlib.util
import json
import os
import re
import shlex
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
              'RAMULATOR2_SANITIZE', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CFLAGS',
              'LDLIBS', 'LOADLIBES', 'BUILD_MODE', 'X86_ISA', 'OBJ_ROOT', 'DEP_ROOT', 'BIN_ROOT'}
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
        self.env = nested_make_environment()
        self.compiler = shutil.which('g++') or shutil.which('clang++')
        if not self.compiler:
            self.skipTest('a C++ compiler is required for build stamp checks')

    def stub_dependencies(self, workspace):
        target = subprocess.check_output([self.compiler, '-dumpmachine'], text=True)
        arch = 'arm64' if target.startswith(('aarch64-', 'arm64-')) else 'x64'
        triplet = arch + ('-osx' if 'darwin' in target or 'apple' in target else '-linux')
        for directory in ('include', 'lib'):
            (workspace / 'vcpkg_installed' / triplet / directory).mkdir(parents=True)

    def fresh_checkout(self, name):
        """The real Makefile and build rules beside a stub fragment and source.

        Goals such as `all` and `ramulator2` hard-include _configuration.mk, which
        only config.sh writes, so a fresh checkout (the hosted python job) cannot
        dry-run the repository root.
        """
        workspace = self.root / name
        (workspace / 'src').mkdir(parents=True)
        shutil.copyfile(HELPER.parent.parent / 'Makefile', workspace / 'Makefile')
        (workspace / 'config').symlink_to(HELPER.parent, target_is_directory=True)
        (workspace / 'inc').symlink_to(HELPER.parent.parent / 'inc', target_is_directory=True)  # the policy's compiler query
        for file_name, content in {'_configuration.mk': 'executable_name := bin/champsim\n',
                                   'global.options': '', 'module.options': '', 'src/ramulator2_driver.cc': ''}.items():
            (workspace / file_name).write_text(content)
        self.stub_dependencies(workspace)
        return workspace

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
        workspace = self.fresh_checkout('dry-run-checkout')
        before = tree(workspace)
        objects = self.root / 'dry-run-objects'
        result = subprocess.run(['make', '-n', 'ramulator2', 'WITH_RAMULATOR2=0', f'OBJ_ROOT={objects}',
                                 f'CXX={self.compiler}', 'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES='],
                                cwd=workspace, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(objects.exists(), 'dry-run must not create compiler/native stamps')
        self.assertEqual(tree(workspace), before, 'dry-run must not create files')

    def test_fresh_enabled_dry_run_prints_missing_dependencies_without_building(self):
        workspace = self.fresh_checkout('fresh-checkout')
        before = tree(workspace)
        native = self.root / 'missing-native'
        objects = self.root / 'enabled-dry-run'
        result = subprocess.run(['make', '-n', 'all', 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}',
                                 f'OBJ_ROOT={objects}', f'CXX={self.compiler}', 'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES='],
                                cwd=workspace, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{HELPER.name} --mode='1' --root='{native}'", result.stdout)
        self.assertIn('--abi-flags=', result.stdout)
        self.assertRegex(result.stdout, rf'-std=c\+\+20 -c -o {re.escape(str(objects))}/[^\s]+/ramulator2_driver.o')
        self.assertIn(str(native / 'libramulator.so'), result.stdout)
        self.assertFalse(objects.exists())
        self.assertFalse(native.exists())
        self.assertEqual(tree(workspace), before, 'dry-run must not create files')

    def test_long_make_options_do_not_disable_normal_preparation(self):
        workspace = self.fresh_checkout('normal-checkout')
        objects = self.root / 'normal-objects'
        result = subprocess.run(['make', '--no-print-directory', 'ramulator2', 'WITH_RAMULATOR2=0', f'OBJ_ROOT={objects}',
                                 f'CXX={self.compiler}', 'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES='],
                                cwd=workspace, env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Use WITH_RAMULATOR2=1', result.stdout, result.stderr)  # the target explains enablement
        self.assertEqual(len(list(objects.rglob('compiler.stamp'))), 1)

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

    def test_native_root_product_requires_exact_known_identity(self):
        from config import ramulator2_build as helper
        self.assertTrue(hasattr(helper, 'verify_native_product'), 'root-owned native provenance validation is missing')
        library = self.root / 'libramulator.so'
        manifest = self.root / 'manifest.json'
        inputs = {'compiler': 'fixture', 'architecture': ['-march=x86-64-v2']}
        self.assertIsNone(helper.verify_native_product(manifest, library, inputs))
        library.write_bytes(b'compiled native library')
        with self.assertRaisesRegex(RuntimeError, 'fresh isolated'):
            helper.verify_native_product(manifest, library, inputs)
        import hashlib
        record = {'inputs': inputs, 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                  'dependencies': helper.DEPENDENCIES, 'fingerprint': helper.fingerprint(library.read_bytes())}
        manifest.write_text(json.dumps(record))
        self.assertEqual(helper.verify_native_product(manifest, library, inputs), record)
        with self.assertRaisesRegex(RuntimeError, 'fresh isolated'):
            helper.verify_native_product(manifest, library, dict(inputs, architecture=['-march=x86-64']))
        library.write_bytes(b'replaced native library')
        with self.assertRaisesRegex(RuntimeError, 'fresh isolated'):
            helper.verify_native_product(manifest, library, inputs)

    def test_native_root_lock_serializes_distinct_flavor_writers(self):
        from config import ramulator2_build as helper
        self.assertTrue(hasattr(helper, 'native_lock'), 'source-root preparation lock is missing')
        code = """import sys,time
from pathlib import Path
from config.ramulator2_build import native_lock
root=Path(sys.argv[1])
with native_lock(root):
    count=root/'build-count'
    previous=int(count.read_text()) if count.exists() else 0
    time.sleep(.05)
    count.write_text(str(previous+1))
"""
        processes = [subprocess.Popen([sys.executable, '-c', code, str(self.root)], cwd=HELPER.parent.parent,
                                      env=self.env) for flavor in ('sim', 'test')]
        for process in processes:
            self.assertEqual(process.wait(timeout=10), 0)
        self.assertEqual((self.root / 'build-count').read_text(), '2')

    def test_enabled_missing_root_fails_before_building(self):
        result = self.run_helper('--mode=1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('RAMULATOR2_ROOT', result.stderr)
        self.assertFalse((self.root / 'objects' / 'ramulator2-native').exists())

    def sanitizer_workspace(self):
        """The real Makefile and helper beside stub simulator and test sources."""
        workspace = self.root / 'stub-checkout'
        for directory in ('src', 'test/cpp/src', 'inc', '.csconfig'):
            (workspace / directory).mkdir(parents=True)
        shutil.copyfile(HELPER.parent.parent / 'Makefile', workspace / 'Makefile')
        (workspace / 'config').symlink_to(HELPER.parent, target_is_directory=True)
        for name in ('src/ramulator2_driver.cc', 'src/probe.cc', 'test/cpp/src/000-test-main.cc',
                     'test/cpp/src/001-probe.cc', 'global.options', 'module.options'):
            (workspace / name).write_text('')
        for name in ('champsim_assert.h', 'trace_instruction.h'):
            shutil.copyfile(HELPER.parent.parent / 'inc' / name, workspace / 'inc' / name)
        shutil.copytree(HELPER.parent.parent / 'inc/util', workspace / 'inc/util')
        self.stub_dependencies(workspace)
        (workspace / '_configuration.mk').write_text('executable_name := bin/champsim\n')
        return workspace

    def dry_run(self, workspace, *variables):
        return subprocess.run(['make', '-n', 'all', 'test/bin/000-test-main', f'OBJ_ROOT={self.root / "dry-objects"}',
                               f'CXX={self.compiler}', 'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES=', *variables], cwd=workspace, text=True, capture_output=True,
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

    def test_sanitized_native_commands_preserve_the_architecture_floor(self):
        helper = load_helper()
        configure, _ = helper.cmake_commands(Path('/native'), Path('/build'), self.compiler,
                                             helper.native_settings(True), ['-march=x86-64-v2'])
        for name in ('CXX_FLAGS', 'SHARED_LINKER_FLAGS', 'EXE_LINKER_FLAGS'):
            flags = next(word.split('=', 1)[1] for word in configure if word.startswith('-DCMAKE_' + name + '='))
            self.assertEqual(shlex.split(flags), ['-march=x86-64-v2', *shlex.split(SANITIZER_OPTIONS)])

    def test_flipping_sanitizers_refuses_native_replacement_and_isolates_host_objects(self):
        helper = load_helper()
        library, manifest = self.root / 'libramulator.so', self.root / 'manifest.json'
        library.write_bytes(b'known library')
        inputs = [{'native': helper.native_settings(sanitize)} for sanitize in (False, True)]
        for prior, current in (inputs, inputs[::-1]):
            with self.subTest(prior=prior['native']['build_type']):
                record = {'inputs': prior, 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                          'dependencies': helper.DEPENDENCIES, 'fingerprint': helper.fingerprint(library.read_bytes())}
                manifest.write_text(json.dumps(record))
                self.assertEqual(helper.verify_native_product(manifest, library, prior), record)
                with self.assertRaisesRegex(RuntimeError, 'fresh isolated'):
                    helper.verify_native_product(manifest, library, current)
                self.assertEqual(library.read_bytes(), b'known library')
                self.assertEqual(json.loads(manifest.read_text()), record)

        # Exercise the actual Make policy selector, including sanitizer injection.
        workspace = self.sanitizer_workspace()
        def paths(sanitize):
            result = subprocess.run(['make', '-s', 'print-build-paths', f'CXX={self.compiler}',
                                     'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES=',
                                     'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={self.root / "native"}',
                                     'RAMULATOR2_SANITIZE=' + str(int(sanitize))],
                                    cwd=workspace, env=self.env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        plain, instrumented = paths(False), paths(True)
        for key in ('obj', 'dep', 'binary'):
            self.assertNotEqual(plain[key], instrumented[key])
        self.assertEqual(paths(False), plain)
        self.assertFalse((self.root / 'native').exists())

    def test_make_instruments_every_host_compile_and_link_with_the_native_library(self):
        workspace = self.sanitizer_workspace()
        native = self.root / 'missing-native'
        result = self.dry_run(workspace, 'WITH_RAMULATOR2=1', f'RAMULATOR2_ROOT={native}', 'RAMULATOR2_SANITIZE=1')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        compiles = [line for line in lines if ' -c -o ' in line]
        links = [line for line in lines if ' -o ' in line and ' -c -o ' not in line and line.startswith(self.compiler)]
        # Probe/driver objects are isolated for sim and test, plus two test sources.
        self.assertEqual(len(compiles), 6, result.stdout)
        self.assertEqual(len(links), 2, result.stdout)
        prepares = {line for line in lines if 'build_config.py prepare ' in line}
        self.assertTrue(prepares)
        for line in prepares:
            subprocess.run(shlex.split(line), cwd=workspace, env=nested_make_environment(), check=True,
                           capture_output=True, text=True)
        policies = list((self.root / 'dry-objects').rglob('build-policy.json'))
        self.assertEqual(len(policies), 2)
        for path in policies:
            policy = json.loads(path.read_text())
            self.assertEqual(policy['native']['mode'], 'RelWithDebInfo C++20 Python=OFF Sanitizers=address,undefined')
            self.assertEqual(policy['assertions'], 1)
        def expanded(line):
            words = shlex.split(line)
            return [word for token in words for word in
                    (shlex.split(Path(token[1:]).read_text()) if token.startswith('@') else [token])]
        for line in compiles + links:
            words = expanded(line)
            self.assertIn('-fsanitize=address,undefined', words)
            self.assertIn('-fno-omit-frame-pointer', words)
            if line in compiles:
                self.assertIn('-g3', words)
                self.assertIn('-DCHAMPSIM_ENABLE_ASSERTIONS=1', words)
                self.assertNotIn('-DNDEBUG', words)
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
