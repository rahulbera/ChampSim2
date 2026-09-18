#!/usr/bin/env python3
"""Profile completed benchmark runs without changing their release binaries."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import tomllib

from benchmark_ptw import parse_counts, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--campaign', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--traces', nargs='+', default=['sqlite', 'gcc', 'mcf'])
    parser.add_argument('--variants', nargs='+', choices=['detailed', 'fixed', 'hermes'], default=['detailed', 'fixed', 'hermes'])
    parser.add_argument('--kind', choices=['stacks', 'cpu', 'heap'], default='stacks')
    parser.add_argument('--interval-ms', type=float, default=20)
    parser.add_argument('--allow-heap-timer-warning', action='store_true',
                        help='accept only timer warnings in heap event counts, after independently validating allocation counts')
    args = parser.parse_args()
    if args.interval_ms <= 0:
        parser.error('interval must be positive')
    args.campaign = args.campaign.resolve(strict=True)
    manifest = json.loads((args.campaign / 'manifest.json').read_text())
    runs = json.loads((args.campaign / 'runs.json').read_text())
    references = {(r['trace'], r['variant']): r for r in runs if r['repetition'] == 0}
    for binary in manifest['binaries'].values():
        if sha256(binary['path']) != binary['sha256']:
            raise RuntimeError(f"The benchmarked binary changed: {binary['path']}")
    for item in manifest['configs'] + [t for t in manifest['traces'] if t['name'] in args.traces]:
        if sha256(item['path']) != item['sha256']:
            raise RuntimeError(f"The benchmarked input changed: {item['path']}")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, set(manifest['cpu_affinity']))
    for trace in args.traces:
        for variant in args.variants:
            reference = references[(trace, variant)]
            directory = args.output / f'{trace}-{variant}'
            directory.mkdir()
            argv = list(reference['argv'])
            if variant != 'hermes':
                argv[argv.index('--toml') + 1] = str(directory / 'stats.toml')
            experiment = directory / 'profile.er'
            env = os.environ.copy()
            if args.kind == 'stacks':
                collect = ['gdb', '-q', '-nx', '--batch', '-x', str(Path(__file__).with_name('gdb_sample.py').resolve()), '--args'] + argv
                env['CHAMPSIM_STACK_OUTPUT'] = str(directory / 'stacks.json')
                env['CHAMPSIM_STACK_INTERVAL_MS'] = str(args.interval_ms)
                env.pop('PYTHONHOME', None)
                env.pop('PYTHONPATH', None)
            else:
                collect = ['gprofng', 'collect', 'app', '-F', 'off', '-a', 'on', '-o', str(experiment)]
                collect += ['-H', 'on', '-p', 'off'] if args.kind == 'heap' else ['-p', str(args.interval_ms)]
                collect += argv
            record = {'command': collect, 'cwd': str(directory), 'cpu_affinity': sorted(os.sched_getaffinity(0)),
                      'reference': reference, 'kind': args.kind}
            (directory / 'command.json').write_text(json.dumps(record, indent=2) + '\n')
            start = time.perf_counter()
            with (directory / 'stdout.txt').open('w') as out, (directory / 'stderr.txt').open('w') as err:
                result = subprocess.run(collect, cwd=directory, stdout=out, stderr=err, timeout=900, env=env)
            record['profile_wall_seconds'] = time.perf_counter() - start
            record['returncode'] = result.returncode
            if result.returncode:
                raise RuntimeError(f'{directory}: profiler exited {result.returncode}; see stderr.txt')
            counts = parse_counts(variant, (directory / 'stdout.txt').read_text())
            record.update(counts)
            for key, value in counts.items():
                if value != reference[key]:
                    raise RuntimeError(f'{directory}: profiling changed {key}')
            if variant != 'hermes':
                with (directory / 'stats.toml').open('rb') as stream:
                    stats = tomllib.load(stream)
                if stats['config']['dram-model'] != 'legacy':
                    raise RuntimeError('Profiling must use legacy DRAM')
                record['phase_sha256'] = hashlib.sha256(json.dumps(stats['phase'], sort_keys=True).encode()).hexdigest()
                if record['phase_sha256'] != reference['phase_sha256']:
                    raise RuntimeError(f'{directory}: profiling changed phase statistics')
            (directory / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
            if args.kind == 'stacks':
                stacks = json.loads((directory / 'stacks.json').read_text())
                if stacks['exit_code'] != 0 or not stacks['samples']:
                    raise RuntimeError(f'{directory}: invalid stack collection')
            for display in (() if args.kind == 'stacks' else ('functions', 'calltree', 'metric_list', 'statistics', 'header')):
                with (directory / f'{display}.txt').open('w') as out:
                    subprocess.run(['gprofng', 'display', 'text', '-limit', '100', f'-{display}', str(experiment)],
                                   cwd=directory, stdout=out, stderr=subprocess.STDOUT, timeout=180, check=True)
            if args.kind != 'stacks':
                warnings = [line for line in (directory / 'header.txt').read_text().splitlines() if 'Collector Warning:' in line]
                record['collector_warnings'] = warnings
                (directory / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
                if warnings and not (args.kind == 'heap' and args.allow_heap_timer_warning
                                     and all('Collection interval timer period was changed' in line for line in warnings)):
                    raise RuntimeError(f'{directory}: collector warning; reject this profile (see header.txt)')
            print(f'{trace} {variant} {args.kind}: collected; simulator counts verified ({record["profile_wall_seconds"]:.2f} s under profiler)', flush=True)


if __name__ == '__main__':
    main()
