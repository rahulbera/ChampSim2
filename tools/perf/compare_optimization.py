#!/usr/bin/env python3
"""Paired legacy-only optimization measurements with exact result comparisons."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
from types import SimpleNamespace
import tomllib

from benchmark_ptw import build_info, measure, sha256


def fingerprint(record):
    with (Path(record['cwd']) / 'stats.toml').open('rb') as stream:
        data = tomllib.load(stream)
    return (record['warmup_instructions'], record['roi_instructions'],
            record['warmup_cycles'], record['roi_cycles'],
            hashlib.sha256(json.dumps(data['phase'], sort_keys=True).encode()).hexdigest(),
            hashlib.sha256(json.dumps(data['config'], sort_keys=True).encode()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--traces', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, action='append', default=[])
    parser.add_argument('--cpu', type=int, default=8)
    parser.add_argument('--warmup', type=int, default=1_000_000)
    parser.add_argument('--instructions', type=int, default=3_000_000)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=900, help='wall-clock seconds allowed per simulation')
    parser.add_argument('--regression', action='store_true', help='short paired controls, both PTW modes and stress configurations; not timing evidence')
    args = parser.parse_args()
    if min(args.warmup, args.instructions, args.repetitions, args.timeout) <= 0:
        parser.error('instruction counts, repetitions and timeout must be positive')
    args.output = args.output.resolve()
    args.config = [p.resolve(strict=True) for p in args.config]
    binaries = {label: p.resolve(strict=True) for label, p in (('before', args.before), ('after', args.after))}
    traces = json.loads(args.traces.read_text())
    if not isinstance(traces, list) or not traces:
        parser.error('trace manifest must contain a non-empty list of traces')
    for trace in traces:
        trace['path'] = str(Path(trace['path']).resolve(strict=True))
        if sha256(trace['path']) != trace['sha256']:
            raise RuntimeError('Trace hash mismatch: ' + trace['path'])
    args.output.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, {args.cpu})
    manifest = {'arguments': {k: str(v) for k, v in vars(args).items()}, 'traces': traces,
                'binaries': {label: {'path': str(p), 'sha256': sha256(p), 'build_info': build_info(p)} for label, p in binaries.items()},
                'configs': [{'path': str(p), 'sha256': sha256(p), 'text': p.read_text()} for p in args.config],
                'cpu_affinity': sorted(os.sched_getaffinity(0)), 'regression_only': args.regression}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    cases = [(t, 'detailed', []) for t in traces]
    if args.regression:
        cases += [(t, 'fixed', []) for t in traces]
        for trace in (traces[0], traces[-1]):
            for mode in ('detailed', 'fixed'):
                cases += [(trace, mode, ['vmem.randomization=42', 'ooo_cpu.cpu0.frequency=3500',
                                         'ptw.cpu0_ptw.frequency=2000', 'cache.cpu0_l2c.frequency=2500', 'sim.deadlock_cycle=100000'])]
                cases += [(trace, mode, ['vmem.randomization=false', 'pmem.channels=2', 'pmem.ranks=2',
                                         'pmem.bankgroups=4', 'pmem.banks=4', 'pmem.bank_rows=4096', 'cache.cpu0_l1d.prefetcher=next_line'])]
    records, groups = [], {}
    for repetition in range(args.repetitions):
        for index, (trace, mode, settings) in enumerate(cases):
            case = f'{index:02d}-{trace["name"]}-{mode}'
            pair = []
            for label in (('before', 'after') if (index + repetition) % 2 == 0 else ('after', 'before')):
                parent = args.output / f'{case}-{label}'
                parent.mkdir(exist_ok=True)
                run_args = SimpleNamespace(champsim=binaries[label], baseline=None, config=args.config,
                                           settings=settings, warmup=args.warmup, instructions=args.instructions,
                                           fixed_latency=200, output=parent, timeout=args.timeout)
                result = measure(run_args, mode, trace, repetition)
                result.update(label=label, case=case)
                pair.append(result)
                groups.setdefault(case, {}).setdefault(label, []).append(result)
                records.append(result)
                (args.output / 'runs.json').write_text(json.dumps(records, indent=2) + '\n')
            if fingerprint(pair[0]) != fingerprint(pair[1]):
                raise RuntimeError(f'NON-INERT optimization: {case}; inspect the paired stats.toml files')
    rows = []
    for case, labels in groups.items():
        if len({fingerprint(r) for runs in labels.values() for r in runs}) != 1:
            raise RuntimeError(f'Non-deterministic results across repetitions: {case}')
        values = {label: statistics.median(r['kips'] for r in runs) for label, runs in labels.items()}
        rows.append({'case': case, 'before_kips': values['before'], 'after_kips': values['after'],
                     'improvement_percent': 100 * (values['after'] / values['before'] - 1),
                     'before_min': min(r['kips'] for r in labels['before']), 'before_max': max(r['kips'] for r in labels['before']),
                     'after_min': min(r['kips'] for r in labels['after']), 'after_max': max(r['kips'] for r in labels['after']),
                     'repetitions': args.repetitions, 'inert': True})
    with (args.output / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    for trace in traces:
        if sha256(trace['path']) != trace['sha256']:
            raise RuntimeError('Trace changed during comparison: ' + trace['path'])
    print(f'PASS: {len(cases)} cases, {len(records)} runs; complete phase/configuration parity', flush=True)


if __name__ == '__main__':
    main()
