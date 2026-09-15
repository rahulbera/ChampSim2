#!/usr/bin/env python3
"""Resolve and materialize one portable ChampSim compiler policy (Python 3.10+).

Inspection is read-only. Preparation validates the effective compiler macros and
writes immutable, changed-only metadata below the selected Make policy leaf.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

MODES = {'debug': ['-O0', '-g3', '-fno-omit-frame-pointer'],
         'release': ['-O3', '-g3'], 'fast': ['-O3', '-g3']}
V2 = ['-march=x86-64', '-msse3', '-mssse3', '-msse4.1', '-msse4.2', '-mpopcnt', '-mcx16', '-msahf', '-mtune=generic']


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def write_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text() == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        stream.write(text)
    os.replace(stream.name, path)


def expand(tokens, inputs, stack=()):
    result = []
    for token in tokens:
        if token.startswith('@'):
            path = Path(token[1:]).resolve(strict=True)
            if path in stack:
                raise ValueError(f'response file cycle: {path}')
            inputs[str(path)] = digest(path)
            result += expand(shlex.split(path.read_text()), inputs, (*stack, path))
        else:
            result.append(token)
    return result


def run(command, *args, input=None):
    result = subprocess.run(command + list(args), input=input, text=True, capture_output=True)
    if result.returncode:
        raise ValueError(f'compiler query failed: {shlex.join(command + list(args))}\n{result.stderr}')
    return result.stdout.strip()


def validate_flags(tokens):
    # Policy ownership is deliberately strict: matching user assignments are also
    # rejected, making every option channel follow the same rule.
    # GCC's -Wl comma transport is argv for ld. Validate its words using the
    # same static/exact-library policy, including split option arguments.
    for index, token in enumerate(tokens):
        forwarded = token[4:].split(',') if token.startswith('-Wl,') else []
        linker_words = forwarded or [token]
        static_prefixes = ('-static', '--static', '-Bstatic', '-Bdynamic', '-dn', '-dy', '-non_shared', '-call_shared')
        if any(word.startswith(static_prefixes + ('--library', '-l:')) for word in linker_words):
            raise ValueError(f'unsupported linker library selection: {token}')
        if token == '-l' and index + 1 < len(tokens) and tokens[index + 1].startswith(':'):
            raise ValueError('unsupported exact linker library selection: -l ' + tokens[index + 1])
        if any(word.startswith(('-B', '-a', '--push-state', '--pop-state', '-L', '-l', '@')) for word in forwarded):
            raise ValueError(f'unsupported linker library selection: {token}')
        macro = token[2:] if token.startswith(('-D', '-U')) else ''
        if token in ('-D', '-U') and index + 1 < len(tokens):
            macro = tokens[index + 1]
        if macro.split('=')[0] in ('NDEBUG', 'CHAMPSIM_ENABLE_ASSERTIONS', 'CHAMPSIM_TEST_BUILD'):
            raise ValueError(f'policy-owned assertion/flavor macro: {token} {macro}')
        if (token.startswith('-m') and token not in ('-m64', '-mlittle-endian')) or token.startswith(('-o', '-MF', '-MT', '-MQ', '-MJ', '-save-temps', '-Xlinker', '-B', '-fprofile-use', '-fprofile-generate', '-fprofile-instr')):
            raise ValueError(f'conflicting or unsupported policy option: {token}')
        if token.startswith(('-O', '-Xclang', '-Xpreprocessor', '-Xassembler', '-Wa,', '-Wp,',
                             '-specs', '--specs', '-fplugin', '-fomit-frame-pointer')):
            raise ValueError(f'conflicting or unsupported policy option: {token}')
        if any(c in token for c in '\n\r\0'):
            raise ValueError('newlines/NUL are unsupported in build options')


def driver_values(tokens, prefix):
    values = []
    iterator = iter(tokens)
    for token in iterator:
        if token == prefix:
            value = next(iterator, None)
            if value is None:
                raise ValueError(f'missing argument for {prefix}')
            values.append(value)
        elif token.startswith(prefix):
            values.append(token[len(prefix):])
    return values


def target_flags(tokens):
    result = []
    iterator = iter(tokens)
    for token in iterator:
        if token.startswith(('--target=', '-target=', '--sysroot=', '-isysroot=')) or token in ('-m64', '-mlittle-endian'):
            result.append(token)
        elif token in ('--target', '-target', '--sysroot', '-isysroot'):
            value = next(iterator, None)
            if value is None:
                raise ValueError(f'missing argument for {token}')
            result += [token, value]
    return result


def resolve(args):
    for name in ('obj', 'dep', 'binary', 'registry', 'installed'):
        value = getattr(args, name)
        if not value or not re.fullmatch(r'[A-Za-z0-9_./+-]+', value):
            raise ValueError(f'{name} path contains unsupported Make graph characters: {value!r}')
    if args.mode not in MODES:
        raise ValueError('BUILD_MODE must be debug, release, or fast (not empty)')
    if args.flavor not in ('sim', 'test'):
        raise ValueError('BUILD_FLAVOR must be sim or test')
    inputs = {}
    if any(c in args.cxx for c in '$`\n\r;<>|&'):
        raise ValueError('CXX must use transparent argv syntax, not shell programs or substitutions')
    command = expand(shlex.split(args.cxx), inputs)
    if not command or any(t in ('|', '&&', ';', '>', '<') for t in command):
        raise ValueError('CXX must be an executable with transparent argv wrappers, not shell syntax')
    compiler_paths = {}
    for token in command:
        path = shutil.which(token)
        if path and Path(path).is_file():
            compiler_paths[str(Path(path).resolve())] = digest(path)
    if not shutil.which(command[0]):
        raise ValueError(f'compiler not found: {command[0]}')
    options = {name: expand(shlex.split(getattr(args, name)), inputs)
               for name in ('cppflags', 'cxxflags', 'ldflags', 'ldlibs', 'loadlibes')}
    common = expand(['@global.options'], inputs)
    module = expand(['@module.options'], inputs)
    libraries = expand(shlex.split(args.libraries), inputs) + (expand(shlex.split(args.test_libraries), inputs) if args.flavor == 'test' else [])
    all_user = command[1:] + common + module + sum(options.values(), []) + libraries
    validate_flags(all_user)
    for index, token in enumerate(all_user):
        if token in ('-include', '-imacros') and index + 1 < len(all_user):
            path = Path(all_user[index + 1]).resolve(strict=True)
            inputs[str(path)] = digest(path)
    # Query through the selected driver and target flags, never the login host.
    target_options = target_flags(options['cppflags'] + options['cxxflags'])
    target = run(command + target_options, '-dumpmachine')
    link_target_options = target_flags(options['ldflags'] + options['loadlibes'] + options['ldlibs'])
    if link_target_options and run(command + link_target_options, '-dumpmachine') != target:
        raise ValueError('linker-driver target conflicts with the selected compiler target')
    if args.native == '1' and run(command, '-dumpmachine') != target:
        raise ValueError('native ABI probes require an executable native compiler target')
    if not re.fullmatch(r'[A-Za-z0-9_.+-]+', target):
        raise ValueError(f'unsupported compiler target: {target!r}')
    darwin = 'darwin' in target or 'apple' in target
    if not darwin and 'linux' not in target:
        raise ValueError(f'unsupported compiler target: {target}')
    if target.startswith(('x86_64-', 'amd64-')):
        arch = 'x64'
        isa = args.isa if args.isa_explicit else 'x86-64-v2'
        if isa not in ('x86-64', 'x86-64-v2'):
            raise ValueError('X86_ISA must be x86-64 or x86-64-v2')
        architecture = [f'-march={isa}', '-mtune=generic']
        # GCC 9/10 implement the v2 extensions without the later named level.
        probe = subprocess.run(command + target_options + architecture + ['-E', '-x', 'c++', '-'], input='', text=True, capture_output=True)
        if probe.returncode and isa == 'x86-64-v2':
            architecture = V2.copy()
    elif target.startswith(('aarch64-', 'arm64-')):
        arch, isa = 'arm64', 'armv8-a'
        if args.isa_explicit:
            raise ValueError('X86_ISA cannot be supplied for an Arm compiler target')
        architecture = ['-march=armv8-a', '-mtune=generic']
    else:
        raise ValueError(f'unsupported compiler target: {target}')
    expected = arch + ('-osx' if darwin else '-linux')
    triplet = args.triplet or expected
    if triplet != expected:
        raise ValueError(f'VCPKG_TARGET_TRIPLET {triplet!r} is incompatible with {target}; expected {expected}')
    installed = Path(args.installed).resolve()
    dependency = installed / triplet
    if not (dependency / 'include').is_dir() or not (dependency / 'lib').is_dir():
        raise ValueError(f'selected dependency installation is missing: {dependency}')
    files = [Path('vcpkg.json'), installed / 'vcpkg/status']
    files += sorted((dependency / 'share').glob('**/vcpkg_abi_info.txt'))
    files += sorted((dependency / 'share').glob('**/vcpkg.spdx.json'))
    files += sorted(Path('vcpkg/triplets').glob(f'**/{triplet}.cmake'))
    link_tokens = command[1:] + options['ldflags'] + ['-L' + str(dependency / 'lib'), '-L' + str(dependency / 'lib/manual-link')] + options['loadlibes'] + options['ldlibs'] + libraries
    library_dirs = [Path(p).resolve() for p in driver_values(link_tokens, '-L')]
    linked = {}
    for name in driver_values(link_tokens, '-l'):
        token = '-l' + name
        matches = [p for directory in library_dirs for suffix in ('.so', '.dylib', '.a')
                   for p in [directory / ('lib' + name + suffix)] if p.exists()]
        if not matches and token in libraries:
            raise ValueError(f'selected dependency library missing: {token} in {dependency}')
        if matches:
            linked[token] = str(matches[0].resolve())
            files.append(matches[0])
        else:
            linked[token] = 'unknown (compiler/system search path)'
    files += [Path(token) for token in options['ldflags'] + options['ldlibs'] + options['loadlibes'] + libraries if not token.startswith('-') and Path(token).is_file()]
    vcpkg_revision = 'unknown (source archive or unavailable checkout)'
    if Path('vcpkg/.git').exists():
        result = subprocess.run(['git', '-C', 'vcpkg', 'rev-parse', 'HEAD'], text=True, capture_output=True)
        if result.returncode == 0:
            vcpkg_revision = result.stdout.strip()
    packages = []
    status = installed / 'vcpkg/status'
    if status.exists():
        for paragraph in status.read_text().split('\n\n'):
            fields = dict(line.split(': ', 1) for line in paragraph.splitlines() if ': ' in line)
            if fields.get('Architecture') == triplet:
                packages.append({k: v for k, v in fields.items() if k in ('Package', 'Version', 'Architecture', 'Abi', 'Status')})
    dependency_inputs = {str(p.resolve()): digest(p) for p in files if p.is_file()}
    assertions = int(args.mode != 'fast')
    flags = common + options['cppflags'] + options['cxxflags'] + MODES[args.mode] + architecture + [f'-DCHAMPSIM_ENABLE_ASSERTIONS={assertions}']
    if args.flavor == 'test':
        flags += ['-DCHAMPSIM_TEST_BUILD=1']
    policy = {'schema_version': 1, 'mode': args.mode, 'assertions': assertions, 'flavor': args.flavor,
              'isa': isa, 'architecture_options': architecture,
              'compiler': {'command': command, 'target': target, 'version': run(command, '--version'), 'executables': compiler_paths},
              'compile_options': flags, 'module_options': module, 'link_options': target_options + options['ldflags'] + architecture,
              'libraries': options['loadlibes'] + options['ldlibs'] + libraries,
              'option_inputs': inputs, 'dependencies': {'triplet': triplet, 'directory': str(dependency),
                  'build_mode': 'Release', 'vcpkg_revision': vcpkg_revision, 'packages': packages, 'isa_provenance': 'unknown (external installation)',
                  'assertions': 'external dependency policy', 'inputs': dependency_inputs, 'linked_libraries': linked},
              'native': {'enabled': args.native == '1', 'root': str(Path(args.native_root).resolve()) if args.native == '1' else None,
                         'mode': 'Release C++20 Python=OFF', 'architecture_options': architecture,
                         'driver_options': ['-isystem', str(Path(args.native_root).resolve() / 'src'), '-std=c++20'] if args.native == '1' else [],
                         'link_options': ['-Wl,-rpath,' + str(Path(args.native_root).resolve()), '-ldl'] if args.native == '1' else []},
              'registry_directory': str(Path(args.registry).resolve()),
              'build_inputs': {str(p): digest(p) for p in (Path(__file__), Path('Makefile'), Path('config/build_rules.mk'), Path('config/ramulator2_build.py')) if p.is_file()}}
    if args.native not in ('0', '1'):
        raise ValueError('WITH_RAMULATOR2 must be 0 or 1')
    policy['policy_key'] = hashlib.sha256(json.dumps(policy, sort_keys=True).encode()).hexdigest()[:24]
    return policy


def verify(policy, includes):
    flags = policy['compile_options'] + includes
    source = '#include "champsim_assert.h"\n#include "trace_instruction.h"\n'
    macros = run(policy['compiler']['command'] + flags, '-dM', '-E', '-x', 'c++', '-', input=source)
    values = dict(re.findall(r'^#define (\w+)\s+(.*)$', macros, re.M))
    if ('__OPTIMIZE__' in values) != (policy['mode'] != 'debug'):
        raise ValueError('effective compiler optimization conflicts with BUILD_MODE')
    if values.get('CHAMPSIM_ENABLE_ASSERTIONS') != str(policy['assertions']) or 'NDEBUG' in values:
        raise ValueError('effective assertion macros conflict with build policy')
    payload = values.get('CHAMPSIM_TRACE_MEMORY_VALUES', '0')
    if payload not in ('0', '1'):
        raise ValueError('CHAMPSIM_TRACE_MEMORY_VALUES must be 0 or 1')
    if policy['isa'].startswith('x86-64'):
        if '__x86_64__' not in values or '__ILP32__' in values:
            raise ValueError('effective compiler target is not 64-bit x86')
        extra = ('__AVX__', '__AVX2__', '__AVX512F__', '__FMA__', '__BMI__', '__BMI2__', '__AES__', '__PCLMUL__', '__SHA__')
        v2 = ('__SSE3__', '__SSSE3__', '__SSE4_1__', '__SSE4_2__', '__POPCNT__', '__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16')
        if any(k in values for k in extra) or (policy['isa'] == 'x86-64' and any(k in values for k in v2)):
            raise ValueError('effective compiler ISA exceeds selected baseline')
        if policy['isa'] == 'x86-64-v2' and not all(k in values for k in v2):
            raise ValueError('effective compiler ISA lacks required v2 extensions')
    elif '__aarch64__' not in values or '__AARCH64EB__' in values or any(k in values for k in ('__ARM_FEATURE_SVE', '__ARM_FEATURE_CRYPTO', '__ARM_FEATURE_ATOMICS')):
        raise ValueError('effective compiler ISA conflicts with little-endian Armv8-A')
    return int(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inspect', 'prepare', 'paths', 'publish'))
    for name, default in [('cxx', 'g++'), ('mode', 'release'), ('flavor', 'sim'), ('isa', ''), ('triplet', ''),
                          ('installed', 'vcpkg_installed'), ('native', '0'), ('native-root', ''),
                          ('libraries', '-lCLI11 -llzma -lz -lbz2 -lzstd -lfmt'), ('test-libraries', '-lCatch2Main -lCatch2'),
                          ('obj', ''), ('dep', ''), ('binary', ''), ('registry', '.csconfig'), ('alias', '')]:
        parser.add_argument('--' + name, default=default)
    for name in ('cppflags', 'cxxflags', 'ldflags', 'ldlibs', 'loadlibes'):
        parser.add_argument('--' + name, default='')
    parser.add_argument('--isa-explicit', action='store_true')
    args = parser.parse_args()
    if args.action == 'publish':
        alias = Path(args.alias)
        alias.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=alias.parent, delete=False) as stream:
            temporary = Path(stream.name)
        temporary.unlink()
        temporary.symlink_to(Path(args.binary).resolve())
        temporary.replace(alias)
        return
    if args.action == 'paths':
        print(json.dumps({key: str(Path(getattr(args, key)).absolute()) for key in ('obj', 'dep', 'binary')}))
        return
    policy = resolve(args)
    if args.action == 'inspect':
        # Make receives only path-safe identifiers. Compiler flags stay in JSON
        # and response files, avoiding a second shell/Make interpretation.
        print(policy['compiler']['target'], policy['isa'], policy['policy_key'])
        return
    obj = Path(args.obj)
    includes = ['-I' + str(obj), '-I' + args.registry, '-I' + str(Path('inc').resolve()),
                '-isystem', policy['dependencies']['directory'] + '/include']
    policy['trace_memory_values'] = verify(policy, includes)
    verify(dict(policy, compile_options=policy['compile_options'] + policy['module_options']), includes)
    policy['include_options'] = includes
    if policy['native']['enabled']:
        manifest = obj / 'ramulator2-native/manifest.json'
        if manifest.exists():
            policy['native']['manifest'] = json.loads(manifest.read_text())
    write_changed(obj / 'build-policy.json', json.dumps(policy, sort_keys=True, indent=2) + '\n')
    write_changed(obj / 'absolute.options', shlex.join(includes) + '\n')
    write_changed(obj / 'policy.options', shlex.join(policy['compile_options']) + '\n')
    write_changed(obj / 'module-policy.options', shlex.join(policy['module_options']) + '\n')
    write_changed(obj / 'link.options', shlex.join(policy['link_options'] + ['-L' + policy['dependencies']['directory'] + '/lib', '-L' + policy['dependencies']['directory'] + '/lib/manual-link']) + '\n')
    write_changed(obj / 'libraries.options', shlex.join(policy['libraries']) + '\n')
    write_changed(obj / 'compiler.options', shlex.join(policy['compiler']['command'][1:]) + '\n')
    write_changed(obj / 'build_info_generated.h', '#pragma once\ninline constexpr char champsim_build_info[] = ' + json.dumps(json.dumps(policy, sort_keys=True)) + ';\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Build policy: {error}', file=sys.stderr)
        sys.exit(1)
