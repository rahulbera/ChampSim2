"""Execute the real Make rules with small actual compiler fixtures."""
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class BuildModeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='champsim-modes-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.compiler = '/usr/bin/g++'
        if not Path(self.compiler).exists():
            self.skipTest('host g++ required')
        for name in ('Makefile', 'global.options', 'module.options'):
            shutil.copy(REPO / name, self.root / name)
        shutil.copytree(REPO / 'config', self.root / 'config')
        for name in ('src', 'inc', 'test/cpp/src', 'branch/probe', '.csconfig',
                     'vcpkg_installed/x64-linux/include', 'vcpkg_installed/x64-linux/lib'):
            (self.root / name).mkdir(parents=True)
        shutil.copy(REPO / 'inc/champsim_assert.h', self.root / 'inc')
        shutil.copy(REPO / 'inc/trace_instruction.h', self.root / 'inc')
        shutil.copytree(REPO / 'inc/util', self.root / 'inc/util')
        (self.root / '_configuration.mk').write_text('configured_bindir := bin\nregistry_dir := .csconfig\nexecutable_name += $(BIN_ROOT)/champsim\n')
        (self.root / '.csconfig/registry.inc').write_text('// configured discovery input\n')
        probe = '''#include "champsim_assert.h"
#include "registry.inc"
#include <cstdio>
extern int core(); extern int module();
int main() {
#ifdef __OPTIMIZE__
 std::puts("optimized");
#else
 std::puts("unoptimized");
#endif
 std::printf("assertions=%d core=%d module=%d\\n", CHAMPSIM_ENABLE_ASSERTIONS, core(), module());
#ifdef __SSE4_2__
 std::puts("v2");
#else
 std::puts("v1");
#endif
#ifdef __AVX__
#error unexpected AVX
#endif
}
'''
        (self.root / 'src/main.cc').write_text('#ifndef CHAMPSIM_TEST_BUILD\n' + probe + '\n#endif\n')
        (self.root / 'test/cpp/src/000-test-main.cc').write_text(probe)
        for name, function in [('src/core.cc', 'core'), ('branch/probe/probe.cc', 'module')]:
            (self.root / name).write_text(f'int {function}() {{\n#ifdef CHAMPSIM_TEST_BUILD\nreturn 1;\n#else\nreturn 0;\n#endif\n}}\n')
        self.env = {k: v for k, v in os.environ.items() if k not in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'MAKEFLAGS', 'MFLAGS', 'BUILD_MODE', 'X86_ISA', 'WITH_RAMULATOR2', 'RAMULATOR2_ROOT')}

    def make(self, *args, ok=True):
        result = subprocess.run(['make', '--no-print-directory', f'CXX={self.compiler}',
                                 'WITH_RAMULATOR2=0', 'CHAMPSIM_LIBRARIES=', 'CHAMPSIM_TEST_LIBRARIES=', *args],
                                cwd=self.root, env=self.env, text=True, capture_output=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def paths(self, *args):
        return json.loads(self.make('print-build-paths', *args).stdout)

    def execute(self, path):
        return subprocess.check_output([str(path)], text=True, cwd=self.root)

    def test_named_modes_compile_actual_policy_and_coexist(self):
        self.make('-j4', 'debug', 'release', 'fast')
        for mode, optimized, assertions in [('debug', 'unoptimized', 1), ('release', 'optimized', 1), ('fast', 'optimized', 0)]:
            paths = self.paths('BUILD_MODE=' + mode)
            binary = Path(paths['binary'])
            self.assertEqual(self.execute(binary), f'{optimized}\nassertions={assertions} core=0 module=0\nv2\n')
            before = binary.stat().st_mtime_ns
            self.make(mode)
            self.assertEqual(binary.stat().st_mtime_ns, before)
        self.assertFalse((self.root / 'bin/champsim').exists())

    def test_sim_test_flavors_and_alias_override(self):
        self.make('-j4', 'all', 'test', 'BUILD_MODE=fast', 'test_main_name=custom/tests')
        self.assertIn('core=0 module=0', self.execute(self.root / 'bin/champsim'))
        self.assertIn('core=1 module=1', self.execute(self.root / 'custom/tests'))
        sim = self.paths('BUILD_MODE=fast')
        tests = self.paths('BUILD_MODE=fast', 'BUILD_FLAVOR=test')
        self.assertNotEqual(sim['obj'], tests['obj'])

    def test_v1_flags_and_response_contents_invalidate(self):
        response = self.root / 'nested.options'
        response.write_text('-DFIXTURE=1\n')
        (self.root / 'outer.options').write_text('@nested.options\n')
        args = ['X86_ISA=x86-64', 'CXXFLAGS=@outer.options']
        self.make('release', *args)
        first = self.paths(*args)
        self.assertTrue(self.execute(first['binary']).endswith('v1\n'))
        response.write_text('-DFIXTURE=2\n')
        second = self.paths(*args)
        self.assertNotEqual(first['obj'], second['obj'])
        self.make('release', *args)
        self.assertTrue(Path(first['binary']).exists())

    def test_rejects_conflicting_policy(self):
        for argument in ['BUILD_MODE=', 'BUILD_MODE=nope', 'CXXFLAGS=-Og', 'CPPFLAGS=-DNDEBUG',
                         'CXXFLAGS=-march=native', 'LDFLAGS=-mavx2', 'CXXFLAGS=-D CHAMPSIM_ENABLE_ASSERTIONS=0',
                         'X86_ISA=x86-64-v3', 'CXXFLAGS=-m32', 'VCPKG_TARGET_TRIPLET=arm64-linux']:
            with self.subTest(argument=argument):
                result = self.make('release', argument, ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotEqual(self.make('fast', 'test', ok=False).returncode, 0)

    def test_distinct_parallel_invocations(self):
        with concurrent.futures.ThreadPoolExecutor() as pool:
            list(pool.map(lambda mode: self.make(mode), ['debug', 'fast']))
        for mode in ['debug', 'fast']:
            self.assertTrue(Path(self.paths('BUILD_MODE=' + mode)['binary']).exists())

    def test_no_execute_and_known_header_dependencies(self):
        args = ['OBJ_ROOT=objects', 'DEP_ROOT=deps']
        self.make('-n', 'release', *args)
        self.assertFalse((self.root / 'objects').exists())
        self.assertEqual(self.make('-q', 'release', *args, ok=False).returncode, 1)
        self.assertFalse((self.root / 'objects').exists())
        self.make('release', *args)
        paths = self.paths(*args)
        self.assertEqual(self.make('-q', 'release', *args, ok=False).returncode, 0)
        dep = Path(paths['dep']) / 'SIM_main.d'
        stamp = dep.stat().st_mtime_ns
        obj = Path(paths['obj']) / 'SIM_main.o'
        header = self.root / 'inc/champsim_assert.h'
        os.utime(header, ns=(obj.stat().st_mtime_ns + 1000000, obj.stat().st_mtime_ns + 1000000))
        self.assertEqual(self.make('-q', 'release', *args, ok=False).returncode, 1)
        self.assertIn(' -c ', self.make('-n', 'release', *args).stdout)
        self.make('-t', 'release', *args)
        self.assertEqual(dep.stat().st_mtime_ns, stamp)
        self.assertGreaterEqual(obj.stat().st_mtime_ns, header.stat().st_mtime_ns)

    def test_alias_switch_back_to_existing_mode(self):
        self.make('all')
        self.make('all', 'BUILD_MODE=fast')
        self.assertIn('assertions=0', self.execute(self.root / 'bin/champsim'))
        self.make('all')
        self.assertIn('assertions=1', self.execute(self.root / 'bin/champsim'))

    def test_wrapper_quoted_options_and_compile_database(self):
        wrapper = self.root / 'compiler wrapper'
        wrapper.write_text('#!/usr/bin/env python3\nimport json,sys,os\nwith open("argv.jsonl", "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\nos.execv("/usr/bin/g++", ["/usr/bin/g++", *sys.argv[1:]])\n')
        wrapper.chmod(0o755)
        (self.root / 'compiler.options').write_text('-DWRAPPER_OPTION=1\n')
        self.compiler = '"' + str(wrapper) + '" @compiler.options'
        args = ["CPPFLAGS=-DFLAG_TEXT=\"a'b\"", 'BUILD_MODE=debug']
        self.make('all', *args)
        paths = self.paths(*args)
        self.make('compile_commands', *args)
        database = json.loads((Path(paths['obj']) / 'compile_commands.json').read_text())
        actual = [json.loads(line) for line in (self.root / 'argv.jsonl').read_text().splitlines()]
        for entry in database:
            self.assertIn(entry['arguments'][1:], actual)
        wrapper.write_text(wrapper.read_text() + '# new wrapper revision\n')
        self.assertNotEqual(paths['obj'], self.paths(*args)['obj'])

    def test_conditional_include_dependencies_and_link_fingerprint(self):
        (self.root / 'inc/conditional.h').write_text('#define RESULT 17\n')
        (self.root / 'src/core.cc').write_text('#ifdef CONDITIONAL\n#include "conditional.h"\n#else\n#define RESULT 0\n#endif\nint core() { return RESULT; }\n')
        args = ['CXXFLAGS=-DCONDITIONAL', 'OBJ_ROOT=objects', 'DEP_ROOT=dependencies']
        self.make('release', *args)
        paths = self.paths(*args)
        obj = Path(paths['obj']) / 'core.o'
        before = obj.stat().st_mtime_ns
        (self.root / 'inc/conditional.h').write_text('#define RESULT 23\n')
        self.make('release', *args)
        self.assertGreater(obj.stat().st_mtime_ns, before)
        self.assertIn('core=23', self.execute(paths['binary']))
        self.assertNotEqual(paths['obj'], self.paths(*args, 'LDFLAGS=-Wl,--as-needed,--discard-none,--no-strip-discarded')['obj'])

    def test_target_routing_and_unsupported_target(self):
        wrapper = self.root / 'target-wrapper'
        for target, triplet in [('aarch64-linux-gnu', 'arm64-linux'), ('arm64-apple-darwin23', 'arm64-osx'), ('x86_64-apple-darwin23', 'x64-osx')]:
            (self.root / 'vcpkg_installed' / triplet / 'include').mkdir(parents=True)
            (self.root / 'vcpkg_installed' / triplet / 'lib').mkdir()
            wrapper.write_text('#!/bin/sh\ncase " $* " in *" -dumpmachine "*) echo '+target+';; *) exec /usr/bin/g++ "$@";; esac\n')
            wrapper.chmod(0o755)
            self.compiler = str(wrapper)
            self.assertIn(target, self.paths()['obj'])
            if target.startswith(('aarch64', 'arm64')):
                self.assertNotEqual(self.make('print-build-paths', 'X86_ISA=x86-64', ok=False).returncode, 0)
        wrapper.write_text('#!/bin/sh\ncase " $* " in *" -dumpmachine "*) echo riscv64-linux-gnu;; *) exec /usr/bin/g++ "$@";; esac\n')
        self.assertNotEqual(self.make('print-build-paths', ok=False).returncode, 0)

    def test_forced_policy_macros_and_nested_conflicts_rejected(self):
        forced = self.root / 'forced.h'
        forced.write_text('#undef CHAMPSIM_ENABLE_ASSERTIONS\n#define CHAMPSIM_ENABLE_ASSERTIONS 0\n')
        result = self.make('release', 'CPPFLAGS=-include forced.h', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('effective assertion', result.stderr)
        (self.root / 'inner.options').write_text('-mavx2\n')
        (self.root / 'outer.options').write_text('@inner.options\n')
        self.assertNotEqual(self.make('release', 'CXXFLAGS=@outer.options', ok=False).returncode, 0)

    def test_available_clang_compiles_policy(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('clang++ unavailable')
        self.compiler = compiler
        self.make('release')
        self.assertTrue(self.execute(self.paths()['binary']).endswith('v2\n'))

    def test_real_discovery_prefix_and_external_module(self):
        shutil.copy(REPO / 'config.sh', self.root)
        external = self.root / 'external/predictor'
        external.mkdir(parents=True)
        (external / 'extra.cc').write_text('int external() { return 9; }\n')
        (self.root / 'src/core.cc').write_text('extern int external(); int core() { return external(); }\n')
        # Only declaration dependencies of the real generated registry are needed.
        (self.root / 'inc/cache.h').write_text('#pragma once\nstruct CACHE { struct prefetcher_module_concept {}; struct replacement_module_concept {}; };\n')
        (self.root / 'inc/ooo_cpu.h').write_text('#pragma once\nstruct O3_CPU { struct branch_module_concept {}; struct btb_module_concept {}; };\n')
        (self.root / 'inc/environment.h').write_text('#pragma once\nnamespace champsim { namespace configured { struct module_registry; } }\n')
        (self.root / '_configuration.mk').unlink()
        subprocess.run(['python3', 'config.sh', '--prefix', 'configured', '--bindir', 'configured/executables',
                        '--makedir', 'generated-make', '--branch-dir', str(external.parent)],
                       cwd=self.root, env=self.env, check=True, capture_output=True)
        args = ['-Igenerated-make', 'OBJ_ROOT=override-objects', 'DEP_ROOT=override-deps', 'BIN_ROOT=override-bin']
        self.make('release', *args)
        paths = self.paths(*args)
        self.assertIn('/override-bin/', paths['binary'])
        self.assertIn('core=9', self.execute(paths['binary']))
        self.assertFalse((self.root / 'configured/executables/champsim').exists())

    def test_unknown_dependency_isa_and_library_identity(self):
        library = self.root / 'vcpkg_installed/x64-linux/lib/libfixture.a'
        library.write_bytes(b'first external library')
        args = ['CHAMPSIM_LIBRARIES=-lfixture']
        before = self.paths(*args)
        library.write_bytes(b'replaced external library')
        self.assertNotEqual(before['obj'], self.paths(*args)['obj'])
        self.make('release')
        policy = json.loads((Path(self.paths()['obj']) / 'build-policy.json').read_text())
        self.assertTrue(policy['dependencies']['isa_provenance'].startswith('unknown'))

    def test_payload_policy_and_rejected_graph_paths(self):
        self.make('release', 'CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1')
        paths = self.paths('CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1')
        policy = json.loads((Path(paths['obj']) / 'build-policy.json').read_text())
        self.assertEqual(policy['trace_memory_values'], 1)
        self.assertNotEqual(paths['obj'], self.paths()['obj'])
        result = self.make('print-build-paths', 'OBJ_ROOT=path with space', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsupported Make graph', result.stderr)

    def test_gcc_legacy_named_v2_fallback(self):
        wrapper = self.root / 'old-gcc'
        wrapper.write_text('#!/bin/sh\ncase " $* " in *" -march=x86-64-v2 "*) echo "unknown architecture" >&2; exit 1;; *) exec /usr/bin/g++ "$@";; esac\n')
        wrapper.chmod(0o755)
        self.compiler = str(wrapper)
        self.make('release')
        paths = self.paths()
        self.assertTrue(self.execute(paths['binary']).endswith('v2\n'))
        policy = json.loads((Path(paths['obj']) / 'build-policy.json').read_text())
        self.assertIn('-msse4.2', policy['architecture_options'])

    def test_explicit_test_binary_target_rechecks_changed_sources(self):
        self.make('test/bin/000-test-main')
        alias = self.root / 'test/bin/000-test-main'
        before = alias.stat().st_mtime_ns
        (self.root / 'src/core.cc').write_text('int core() { return 42; }\n')
        self.make('test/bin/000-test-main')
        self.assertGreater(alias.stat().st_mtime_ns, before)
        self.assertIn('core=42', self.execute(alias))

    def test_link_search_override_records_actual_library(self):
        installed = self.root / 'vcpkg_installed/x64-linux/lib'
        alternate = self.root / 'alternate-libs'
        alternate.mkdir()
        for directory, value in [(installed, 5), (alternate, 7)]:
            source = directory / 'fixture.cc'
            source.write_text(f'int fixture_value() {{ return {value}; }}\n')
            subprocess.run(['/usr/bin/g++', '-c', source, '-o', directory / 'fixture.o'], check=True)
            subprocess.run(['ar', 'rcs', directory / 'libfixture.a', directory / 'fixture.o'], check=True)
        (self.root / 'src/core.cc').write_text('extern int fixture_value(); int core() { return fixture_value(); }\n')
        args = ['CHAMPSIM_LIBRARIES=-lfixture', 'LDFLAGS=-Lalternate-libs']
        self.make('release', *args)
        paths = self.paths(*args)
        self.assertIn('core=7', self.execute(paths['binary']))
        policy = json.loads((Path(paths['obj']) / 'build-policy.json').read_text())
        self.assertEqual(policy['dependencies']['linked_libraries']['-lfixture'], str(alternate / 'libfixture.a'))

    def test_symbol_stripping_is_rejected_across_option_channels(self):
        (self.root / 'keep-symbols.list').write_text('main\n')
        flags = ['-s', '-S', '-Wl,-s', '-Wl,-S', '-Wl,--strip-all',
                 '-Wl,--strip-debug', '-Wl,-strip-all', '-Wl,-strip-debug',
                 '-Wl,--strip-a', '-Wl,--strip-d', '-Wl,-x', '-Wl,-X',
                 '-Wl,--discard-all', '-Wl,--discard-locals', '-Wl,--discard-a',
                 '-Wl,--retain-symbols-file=keep-symbols.list', '-Wl,-retain-symbols-file,keep-symbols.list',
                 '-Wl,--ret=keep-symbols.list', '-Wl,-non_global_symbols_strip_list,keep-symbols.list',
                 '-Wl,-non_global_symbols_no_strip_list,keep-symbols.list',
                 '-Xlinker -s', '--for-linker=-s', '--for-linker=-S', '--for-l -s',
                 '--for-assembler=--strip-local-absolute']
        for flag in flags:
            with self.subTest(flag=flag):
                result = self.make('release', 'LDFLAGS=' + flag, ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('policy option', result.stderr)
        for channel in ['CXX', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LDLIBS',
                        'LOADLIBES', 'CHAMPSIM_LIBRARIES', 'CHAMPSIM_TEST_LIBRARIES',
                        'global.options', 'module.options']:
            with self.subTest(channel=channel):
                (self.root / 'strip-inner.options').write_text('-Wl,--strip-debug\n')
                (self.root / 'strip-outer.options').write_text('@strip-inner.options\n')
                args = ['test']
                if channel.endswith('.options'):
                    path = self.root / channel
                    original = path.read_text()
                    path.write_text(original + '\n@strip-outer.options\n')
                    self.addCleanup(path.write_text, original)
                else:
                    value = '@strip-outer.options'
                    if channel == 'CXX':
                        value = self.compiler + ' ' + value
                    args.append(channel + '=' + value)
                result = self.make(*args, ok=False)
                if channel.endswith('.options'):
                    path.write_text(original)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('policy option', result.stderr)

    def test_empty_macro_definitions_cannot_swallow_following_values(self):
        wrapper = self.root / 'empty-macro-driver'
        wrapper.write_text('''#!/usr/bin/env python3
import re, subprocess, sys
result = subprocess.run(['/usr/bin/g++', *sys.argv[1:]], capture_output=True, text=True)
output = result.stdout
if '-dM' in sys.argv:
    output = re.sub(r'^(#define CHAMPSIM_ENABLE_ASSERTIONS .*)$',
                    r'#define EMPTY_ASSERT \\n\\1', output, flags=re.M)
    output = re.sub(r'^(#define CHAMPSIM_TRACE_MEMORY_VALUES .*)$',
                    r'#define EMPTY_PAYLOAD\\n\\1', output, flags=re.M)
sys.stdout.write(output)
sys.stderr.write(result.stderr)
sys.exit(result.returncode)
''')
        wrapper.chmod(0o755)
        self.compiler = str(wrapper)
        args = ['CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1']
        self.make('-j4', 'debug', 'release', 'fast', *args)
        for mode in ['debug', 'release', 'fast']:
            paths = self.paths('BUILD_MODE=' + mode, *args)
            policy = json.loads((Path(paths['obj']) / 'build-policy.json').read_text())
            self.assertEqual(policy['trace_memory_values'], 1)
            self.assertIn('assertions=' + str(int(mode != 'fast')), self.execute(paths['binary']))

    def test_debug_level_is_owned_by_the_mode(self):
        for flag in ['-g0', '-g1', '-ggdb0', '-gdwarf-2', '-gsplit-dwarf', '-gtoggle',
                     '-gline-tables-only', '-gline-directives-only', '-gmlt',
                     '--debug=0', '--debug=1', '--debug=toggle', '--debug', '--deb', '--debu']:
            with self.subTest(flag=flag):
                (self.root / 'module.options').write_text(flag + '\n')
                result = self.make('release', ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('policy option', result.stderr)
        (self.root / 'module.options').write_text('')
        for channel in ['CXX', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LDLIBS',
                        'LOADLIBES', 'CHAMPSIM_LIBRARIES', 'CHAMPSIM_TEST_LIBRARIES']:
            with self.subTest(channel=channel):
                (self.root / 'debug-inner.options').write_text('--debug=0\n')
                (self.root / 'debug-outer.options').write_text('@debug-inner.options\n')
                value = '@debug-outer.options'
                if channel == 'CXX':
                    value = self.compiler + ' ' + value
                result = self.make('test', channel + '=' + value, ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('policy option', result.stderr)

    def test_opaque_linker_scripts_are_rejected(self):
        (self.root / 'strip-debug.ld').write_text(
            'SECTIONS { /DISCARD/ : { *(.debug*) } } INSERT AFTER .text;\n')
        for flags in ['-Tstrip-debug.ld', '-T strip-debug.ld', '-dTstrip-debug.ld',
                      '--script=strip-debug.ld', '--script strip-debug.ld',
                      '--default-script=strip-debug.ld', '--default-script strip-debug.ld',
                      '-Wl,-T,strip-debug.ld', '-Wl,-Tstrip-debug.ld', '-Wl,-dT,strip-debug.ld',
                      '-Wl,--script=strip-debug.ld', '-Wl,-script,strip-debug.ld',
                      '-Wl,--scr=strip-debug.ld', '-Wl,--default-sc=strip-debug.ld',
                      '--for-linker=-Tstrip-debug.ld']:
            with self.subTest(flags=flags):
                (self.root / 'script-inner.options').write_text(flags + '\n')
                (self.root / 'script-outer.options').write_text('@script-inner.options\n')
                result = self.make('release', 'LDFLAGS=@script-outer.options', ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn('policy option', result.stderr)

    def test_named_modes_retain_debug_and_symbol_sections(self):
        if not shutil.which('readelf'):
            self.skipTest('readelf required for ELF symbol inspection')
        for mode in ['debug', 'release', 'fast']:
            with self.subTest(mode=mode):
                args = ['BUILD_MODE=' + mode, 'LDFLAGS=-Wl,--as-needed,--discard-none,--no-strip-discarded']
                self.make(mode, *args)
                binary = self.paths(*args)['binary']
                sections = subprocess.check_output(['readelf', '-SW', binary], text=True)
                self.assertIn('.debug_info', sections)
                self.assertIn('.symtab', sections)
                module = Path(self.paths(*args)['obj']) / 'modules/branch/probe/probe.o'
                sections = subprocess.check_output(['readelf', '-SW', module], text=True)
                self.assertIn('.debug_info', sections)

    def test_unsupported_link_selection_is_rejected(self):
        for flags in ['-static', '-Wl,-Bstatic', '-Wl,-Bdynamic', '-Wl,--push-state',
                      '-l:libfmt.a', '-l :libfmt.a', '--library=:libfmt.a', '--library :libfmt.a',
                      '-Wl,-static', '-Wl,--static', '-Wl,-B,static', '-Wl,-dn', '-Wl,-non_shared',
                      '-Wl,-a,archive', '-Wl,-l,:libfmt.a', '-Wl,--library,:libfmt.a',
                      '-Wl,-Lelsewhere', '-Wl,-lhidden', '-Wl,@linker.options']:
            with self.subTest(flags=flags):
                result = self.make('-n', 'release', 'LDFLAGS=' + flags, ok=False)
                self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
