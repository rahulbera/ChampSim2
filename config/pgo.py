#!/usr/bin/env python3
"""Train and check the profiles that ChampSim's fast build optimizes with (PGO=auto).

  make pgo-train TRACE_ROOT=<dir> [PGO_TRAIN=<plan>]   writes pgo/<dram model>/gcc-<major.minor>/
  make pgo-check TRACE_ROOT=<dir> [PGO_CHECK=<plan>]   PGO against non-PGO fast: parity and speed

Both run from the source root. Build variables given to make (CXX, WITH_RAMULATOR2,
RAMULATOR2_ROOT, VCPKG_INSTALLED_DIR, ...) reach the builds started here through MAKEFLAGS.
Training needs Python 3.10+; checking uses tools/perf/compare_optimization.py (Python 3.11+).

A plan is JSON: {"schema_version": 1, "warmup_instructions": N, "simulation_instructions": N,
"runs": [{"trace": <path under TRACE_ROOT>, "trace_version": 1|2, "configs": [<path>, ...]}]}.
Config paths are relative to the source root.
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 22), b''):
            value.update(block)
    return value.hexdigest()


def make_flags():
    """The first MAKEFLAGS word holds the single-letter flags when there are any."""
    words = os.environ.get('MAKEFLAGS', '').split()
    return words[0] if words and not words[0].startswith('-') and '=' not in words[0] else ''


def make(*args):
    # The recipe's '+' passes the jobserver; without one (make run without -j), build in parallel anyway.
    jobs = [] if '--jobserver' in os.environ.get('MAKEFLAGS', '') else [f'-j{os.cpu_count()}']
    subprocess.run(['make', '--no-print-directory', *jobs, *args], check=True)


def build_paths(*args):
    result = subprocess.run(['make', '--no-print-directory', '-s', 'print-build-paths', 'BUILD_MODE=fast', *args],
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def load_plan(path, trace_root):
    plan = json.loads(Path(path).read_text())
    if plan.get('schema_version') != 1 or not plan.get('runs'):
        raise ValueError(f'{path}: expected schema_version 1 and a non-empty runs list')
    for run in plan['runs']:
        run['path'] = Path(trace_root) / run['trace']
        if not run['path'].is_file():
            raise ValueError(f"{path}: trace not found: {run['path']}")
        if run.get('trace_version') not in (1, 2):
            raise ValueError(f"{path}: {run['trace']}: trace_version must be 1 or 2")
        for config in run.get('configs', []):
            if not Path(config).is_file():
                raise ValueError(f'{path}: config not found: {config}')
    return plan


def stamp():
    return datetime.datetime.now().astimezone().isoformat(timespec='seconds')


def train(args):
    dram = 'ramulator2' if args.native == '1' else 'legacy-dram'
    plan_path = Path(args.plan or f'pgo/train-{dram}.json')
    plan = load_plan(plan_path, args.trace_root)
    work = Path('.csconfig/pgo-train') / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    output = (work / 'profile').resolve()
    output.mkdir(parents=True)
    print(f'pgo-train: {len(plan["runs"])} runs from {plan_path}; work directory {work}', flush=True)
    for run in plan['runs']:
        run['sha256'] = digest(run['path'])

    selection = ('PGO=generate', f'PGO_PROFILE={output}')
    make('fast', *selection)
    paths = build_paths(*selection)
    policy = json.loads((Path(paths['obj']) / 'build-policy.json').read_text())
    version = policy['pgo']['gcc_version']

    # One run after another into one directory: libgcov merges each run's counters at exit.
    # (Separately collected profiles merged with gcov-tool are rejected by GCC 13.)
    backend = 'ramulator2' if dram == 'ramulator2' else 'legacy'
    for index, run in enumerate(plan['runs']):
        command = [paths['binary']] + sum((['--config', c] for c in run.get('configs', [])), [])
        command += ['--set', f'dram-model={backend}', '--trace-version', str(run['trace_version']), '--hide-heartbeat',
                    '-w', str(plan['warmup_instructions']), '-i', str(plan['simulation_instructions']), '--', str(run['path'])]
        log = work / f'run-{index:02d}.log'
        started = datetime.datetime.now()
        with log.open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        run['seconds'] = round((datetime.datetime.now() - started).total_seconds(), 1)
        print(f"pgo-train: [{index + 1}/{len(plan['runs'])}] {run['trace']} rc={result.returncode} {run['seconds']} s", flush=True)
        if result.returncode:
            raise RuntimeError(f"training run failed: {run['trace']}; see {log}")

    # Only this build's objects carry the portable obj#... names; anything else the instrumented
    # binary or a build probe wrote does not belong in the profile.
    files = sorted(output.glob('obj#*.gcda'))
    if not files:
        raise RuntimeError(f'training wrote no profile files to {output}')
    git = lambda *a: subprocess.run(['git', *a], text=True, capture_output=True).stdout.strip()
    manifest = {
        'schema_version': 1, 'dram_model': dram, 'created': stamp(),
        'compiler': {'gcc_version': version, 'version': policy['compiler']['version'].splitlines()[0],
                     'command': policy['compiler']['command']},
        # Tracked files that differ from the commit, so a reader can tell a documentation edit from code.
        'source': {'commit': git('rev-parse', 'HEAD') or None,
                   'modified': sorted(line.split(maxsplit=1)[1] for line in git('status', '--porcelain', '--untracked-files=no').splitlines())},
        'training': {'plan': str(plan_path), 'warmup_instructions': plan['warmup_instructions'],
                     'simulation_instructions': plan['simulation_instructions'],
                     'runs': [{'trace': r['trace'], 'trace_version': r['trace_version'], 'sha256': r['sha256'],
                               'bytes': r['path'].stat().st_size, 'seconds': r['seconds'],
                               'configs': [{'path': c, 'sha256': digest(c)} for c in r.get('configs', [])]} for r in plan['runs']]},
        'profile_files': len(files)}

    destination = Path('pgo') / dram / f'gcc-{version}'
    staging = destination.with_name(destination.name + '.new')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for path in files:
        shutil.copy2(path, staging / path.name)
    (staging / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if destination.exists():
        retired = destination.with_name(destination.name + '.old')
        shutil.rmtree(retired, ignore_errors=True)
        destination.rename(retired)
        staging.rename(destination)
        shutil.rmtree(retired)
    else:
        staging.rename(destination)
    print(f'pgo-train: wrote {destination} ({len(files)} profile files, GCC {version}, {dram}); review and commit it', flush=True)


def check(args):
    dram = 'ramulator2' if args.native == '1' else 'legacy-dram'
    plan_path = Path(args.plan or f'pgo/check-{dram}.json')
    plan = load_plan(plan_path, args.trace_root)
    work = (Path('.csconfig/pgo-check') / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')).resolve()
    work.mkdir(parents=True)
    with_profile = ['PGO=1'] + ([f'PGO_PROFILE={args.profile}'] if args.profile else [])
    make('fast', *with_profile)
    make('fast', 'PGO=0')
    pgo, plain = build_paths(*with_profile)['binary'], build_paths('PGO=0')['binary']
    harness = Path(__file__).resolve().parents[1] / 'tools/perf/compare_optimization.py'
    failures, rows = [], []
    for index, run in enumerate(plan['runs']):
        name = f"t{index:02d}-" + re.sub(r'[^A-Za-z0-9_-]', '-', Path(run['trace']).name)[:48]
        manifest = work / f'{name}.json'
        manifest.write_text(json.dumps([{'name': name, 'version': run['trace_version'], 'path': str(run['path']),
                                         'sha256': digest(run['path'])}]))
        command = [sys.executable, str(harness), '--before', plain, '--after', pgo, '--traces', str(manifest),
                   '--output', str(work / name), '--cpu', str(args.cpu), '--warmup', str(plan['warmup_instructions']),
                   '--instructions', str(plan['simulation_instructions']), '--repetitions', str(args.repetitions)]
        command += sum((['--config', str(Path(c).resolve())] for c in run.get('configs', [])), [])
        if dram == 'ramulator2':
            command += ['--dram-model', 'ramulator2']
        with (work / f'{name}.log').open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            failures.append(f"{run['trace']}: harness failed (parity or run error); see {work / name}.log")
            continue
        row = next(csv.DictReader((work / name / 'summary.csv').open()))
        change = float(row['improvement_percent'])
        rows.append((run['trace'], float(row['before_kips']), float(row['after_kips']), change))
        if change < -args.threshold:
            failures.append(f"{run['trace']}: PGO is {-change:.1f}% slower than non-PGO fast (limit {args.threshold}%)")
    for trace, before, after, change in rows:
        print(f'pgo-check: {trace}: {before:.1f} -> {after:.1f} KIPS ({change:+.1f}%), complete parity')
    for failure in failures:
        print('pgo-check: FAIL ' + failure)
    print(f"pgo-check: {'PASS' if not failures else 'FAIL'} ({len(rows)} of {len(plan['runs'])} traces measured; results in {work})")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('train', 'check'))
    parser.add_argument('--trace-root', required=True)
    parser.add_argument('--native', default='0')
    parser.add_argument('--plan', default='')
    parser.add_argument('--profile', default='', help='check: a profile directory other than the default')
    parser.add_argument('--threshold', type=float, default=3.0, help='check: largest slowdown in percent that still passes')
    parser.add_argument('--repetitions', type=int, default=2)
    parser.add_argument('--cpu', type=int, default=2)
    args = parser.parse_args()
    if 'n' in make_flags():
        print(f'pgo: make -n: would {args.action} with TRACE_ROOT={args.trace_root!r}; nothing done')
        return 0
    if not args.trace_root:
        parser.error('set TRACE_ROOT to the trace catalog the plan refers to')
    if args.action == 'train':
        train(args)
        return 0
    return check(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'pgo: {error}', file=sys.stderr)
        sys.exit(1)
