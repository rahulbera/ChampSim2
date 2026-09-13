#!/usr/bin/env python3
"""Prepare stable Make stamps and a verified, pinned pure C++ native library.

No native dependency is inspected in disabled mode. This helper is called before
Make reads dependency files, so changed mode/compiler/provenance reaches both
preprocessing and compilation. Output files change only when their bytes change.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

REVISION = '72427a1bba3771564c4fb0e494ba02242fd1eaa7'
DEPENDENCIES = {'fmt': 'e69e5f977d458f2650bb346dadf2ad30c5320281',
                'yaml-cpp': '56e3bb550c91fd7005566f19c079cb7a503223cf'}


def write_changed(path, text):
    if path.exists() and path.read_text() == text:
        return
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(text)
    tmp.replace(path)


def output(args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def fingerprint(data):
    # Match ChampSim's historical config_id basis, not the published FNV basis.
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('0', '1'), required=True)
    parser.add_argument('--root', default='')
    parser.add_argument('--obj', required=True)
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--flags', default='')
    args = parser.parse_args()
    obj = Path(args.obj).resolve()
    obj.mkdir(parents=True, exist_ok=True)
    command = shlex.split(args.cxx)
    compiler = shutil.which(command[0])
    if not compiler:
        raise RuntimeError(f'compiler not found: {args.cxx}')
    identity = {'command': command, 'path': str(Path(compiler).resolve()),
                'version': output(command + ['--version']), 'flags': args.flags}
    write_changed(obj / 'compiler.stamp', json.dumps(identity, sort_keys=True))
    if args.mode == '0':
        write_changed(obj / 'ramulator2_build.h', '#define CHAMPSIM_WITH_RAMULATOR2 0\n')
        return
    if len(command) != 1:
        raise RuntimeError('Ramulator CMake requires CXX to name one compiler executable')
    if not args.root:
        raise RuntimeError('WITH_RAMULATOR2=1 requires RAMULATOR2_ROOT=/path/to/ramulator2')
    root = Path(args.root).resolve(strict=True)
    revision = output(['git', '-C', str(root), 'rev-parse', 'HEAD'])
    if revision != REVISION:
        raise RuntimeError(f'unsupported native revision {revision}; expected {REVISION}')
    if output(['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=no']):
        raise RuntimeError('native tracked source is modified; use the pinned clean checkout')
    native = obj / 'ramulator2-native'
    native.mkdir(exist_ok=True)
    manifest_path = native / 'manifest.json'
    library = root / 'libramulator.so'
    inputs = {'root': str(root), 'revision': revision, 'compiler': identity,
              'helper': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    prior = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    digest = hashlib.sha256(library.read_bytes()).hexdigest() if library.exists() else None
    valid = prior.get('inputs') == inputs and prior.get('sha256') == digest and digest is not None
    for dep, commit in prior.get('dependencies', {}).items():
        path = root / 'ext' / dep
        valid = valid and output(['git', '-C', str(path), 'rev-parse', 'HEAD']) == commit
        valid = valid and not output(['git', '-C', str(path), 'status', '--porcelain', '--untracked-files=no'])
    for name, expected in DEPENDENCIES.items():
        dep_path = root / 'ext' / name
        if (dep_path / '.git').exists() and output(['git', '-C', str(dep_path), 'rev-parse', 'HEAD']) != expected:
            raise RuntimeError(f'unsupported {name} dependency revision; expected {expected}')
    env = {k: v for k, v in os.environ.items() if k not in ('CXXFLAGS', 'CPPFLAGS', 'CFLAGS', 'LDFLAGS')}
    if not valid:
        build = native / ('build-' + hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()[:16])
        with (native / 'build.log').open('w') as log:
            subprocess.run(['cmake', '-S', str(root), '-B', str(build), '-DRAMULATOR_PYTHON_BINDINGS=OFF',
                            '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_CXX_COMPILER={compiler}',
                            '-DCMAKE_CXX_FLAGS=', '-DCMAKE_SHARED_LINKER_FLAGS=', '-DCMAKE_EXE_LINKER_FLAGS='],
                           stdout=log, stderr=subprocess.STDOUT, env=env, check=True)
            # Unknown/replaced libraries cannot acquire provenance from an up-to-date no-op.
            subprocess.run(['cmake', '--build', str(build), '--target', 'ramulator', '--clean-first', '-j6'],
                           stdout=log, stderr=subprocess.STDOUT, env=env, check=True)
        deps = {name: output(['git', '-C', str(root / 'ext' / name), 'rev-parse', 'HEAD']) for name in ('fmt', 'yaml-cpp')}
        if deps != DEPENDENCIES:
            raise RuntimeError(f'native dependency revisions differ from the pinned build: {deps}')
        for name in deps:
            if output(['git', '-C', str(root / 'ext' / name), 'status', '--porcelain', '--untracked-files=no']):
                raise RuntimeError(f'native dependency {name} has modified tracked files')
        prior = {'inputs': inputs, 'dependencies': deps, 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                 'fingerprint': fingerprint(library.read_bytes())}
        write_changed(manifest_path, json.dumps(prior, sort_keys=True, indent=2) + '\n')
    # Definitions are pure Python; they do not import the native extension.
    env.update(PYTHONPATH=str(root / 'python'), PYTHONDONTWRITEBYTECODE='1')
    code = '''import json, ramulator
from ramulator.dram.spec import DRAMStandard
print(json.dumps([[c.name,len(c.levels),len(c.commands),len(c.timing_params),c.internal_prefetch_size,
list(c.levels).index('Column'),c.timing_params.index('tCK_ps')] for c in DRAMStandard._registry.values()]))'''
    models = json.loads(output([sys.executable, '-c', code], env=env))
    lines = ['#define CHAMPSIM_WITH_RAMULATOR2 1', '#include <array>', 'namespace champsim::native_build {',
             f'inline constexpr auto revision = {json.dumps(revision)};',
             f'inline constexpr auto fingerprint = {json.dumps(prior["fingerprint"])};',
             f'inline constexpr auto build = {json.dumps(json.dumps({"compiler": identity, "dependencies": prior["dependencies"], "mode": "Release C++20 Python=OFF"}, sort_keys=True))};',
             'struct model { const char* name; int levels, commands, timings, prefetch, column, clock; };',
             f'inline constexpr std::array<model, {len(models)}> models = {{{{']
    lines += ['{' + json.dumps(row[0]) + ',' + ','.join(map(str, row[1:])) + '},' for row in models]
    lines += ['}};', '}']
    # Root/compiler/library changes must rebuild even when the binary happens to hash identically.
    lines += ['// ' + json.dumps(inputs, sort_keys=True)]
    write_changed(obj / 'ramulator2_build.h', '\n'.join(lines) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Ramulator build: {error}', file=sys.stderr)
        sys.exit(1)
