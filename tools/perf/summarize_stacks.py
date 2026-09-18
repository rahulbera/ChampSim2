#!/usr/bin/env python3
"""Summarize raw GDB stacks into disjoint current-ChampSim component shares.

Allocator and bandwidth shares overlap those components and must not be added
to them. Sampling percentages are approximate; no CPU seconds are inferred.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def component(names, hermes=False):
    if hermes:
        # Hermes has no O3_CPU::operate wrapper. Classify its caller frames;
        # residual main-loop work cannot be split from startup this way.
        if any(n.startswith('va_to_pa(') for n in names):
            return 'ptw'
        if 'O3_CPU::read_from_trace()' in names:
            return 'trace'
        for prefix, label in (('MEMORY_CONTROLLER::', 'dram'), ('CACHE::', 'cache'), ('O3_CPU::', 'core')):
            if any(n.startswith(prefix) for n in names):
                return label
        return 'startup_finish_other'
    if any('champsim::bulk_tracereader<' in n for n in names):
        return 'trace'
    for prefix, label in (('O3_CPU::operate()', 'core'), ('CACHE::operate()', 'cache'),
                          ('PageTableWalker::operate()', 'ptw'), ('DRAM_CHANNEL::operate()', 'dram')):
        if prefix in names:
            return label
    if any('champsim::do_phase(' in n for n in names):
        return 'framework'
    return 'startup_finish_other'


def allocator(name):
    return (name.startswith(('operator new(', 'operator new[](', 'operator delete(', 'operator delete[](', '_int_malloc', '_int_free', 'sysmalloc', 'malloc_consolidate'))
            or any(name.startswith(prefix) for prefix in ('__GI___libc_malloc', '__GI___libc_free', '__GI___libc_realloc', '__libc_malloc', '__libc_free', '__libc_realloc'))
            or name in ('malloc', 'free', 'calloc', 'realloc', 'cfree'))


def summarize(path):
    data = json.loads(path.read_text())
    samples = data['samples']
    totals = collections.Counter()
    for sample in samples:
        names = [frame['function'] for frame in sample['stack']]
        totals[component(names, path.parent.name.endswith('-hermes'))] += 1
        totals['allocator_union'] += any(allocator(n) for n in names)
        totals['bandwidth_exclusive'] += bool(names and names[0].startswith('champsim::bandwidth::'))
        totals['rob_scan_union'] += any(n in ('O3_CPU::schedule_instruction()', 'O3_CPU::execute_instruction()', 'O3_CPU::complete_inflight_instruction()') for n in names)
    row = {'run': path.parent.name, 'samples': len(samples)}
    row.update({key: 100 * totals[key] / len(samples) for key in (
        'core', 'cache', 'ptw', 'dram', 'trace', 'framework', 'startup_finish_other',
        'allocator_union', 'bandwidth_exclusive', 'rob_scan_union')})
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = [summarize(p) for p in sorted(args.directory.glob('*/stacks.json'))]
    if not rows:
        parser.error('no stack collections found')
    with args.output.open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


if __name__ == '__main__':
    main()
