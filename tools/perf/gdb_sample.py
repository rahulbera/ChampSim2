"""GDB Python command file: sample an owned inferior using jittered SIGINT stops.

Run through profile_ptw.py. This is wall-stack sampling, not a CPU clock or
hardware counter measurement. No signal is delivered to the simulator.
"""
import collections
import json
import os
from pathlib import Path
import signal
import threading
import time

import gdb


def sample():
    output = Path(os.environ['CHAMPSIM_STACK_OUTPUT'])
    interval = float(os.environ.get('CHAMPSIM_STACK_INTERVAL_MS', '20')) / 1000
    if interval <= 0:
        raise RuntimeError('sampling interval must be positive')
    for command in ('set pagination off', 'set confirm off', 'set debuginfod enabled off',
                    'set disable-randomization off', 'set startup-with-shell off',
                    'handle SIGINT noprint nopass stop'):
        gdb.execute(command, to_string=True)
    state = {'exit_code': None, 'exited': False, 'signal': None}

    def on_exit(event):
        state['exited'] = True
        state['exit_code'] = getattr(event, 'exit_code', None)

    def on_stop(event):
        state['signal'] = getattr(event, 'stop_signal', None)

    gdb.events.exited.connect(on_exit)
    gdb.events.stop.connect(on_stop)
    gdb.execute('starti', to_string=True)
    pidfd = os.pidfd_open(gdb.selected_inferior().pid)
    samples = []
    start = time.monotonic()
    seed = 0x5EED

    def interrupt():
        try:
            signal.pidfd_send_signal(pidfd, signal.SIGINT)
        except ProcessLookupError:
            pass

    try:
        while not state['exited']:
            # Avoid synchronizing the sampler with periodic simulator work.
            seed = (1664525 * seed + 1013904223) & 0xffffffff
            delay = interval * (0.75 + 0.5 * seed / 0xffffffff)
            state['signal'] = None
            timer = threading.Timer(delay, interrupt)
            timer.start()
            resumed = time.monotonic()
            try:
                gdb.execute('continue', to_string=True)
            finally:
                timer.cancel()
                timer.join()
            stopped = time.monotonic()
            if state['exited']:
                break
            if state['signal'] != 'SIGINT':
                raise RuntimeError(f"Unexpected inferior stop: {state['signal']}")
            frame = gdb.newest_frame()
            stack = []
            for _ in range(64):
                if frame is None:
                    break
                pc = frame.pc()
                library = gdb.solib_name(pc)
                stack.append({'function': frame.name() or f'[{Path(library).name if library else "unknown"}]',
                              'pc': hex(pc), 'library': library})
                try:
                    frame = frame.older()
                except gdb.error:
                    break
            samples.append({'at_seconds': stopped - start, 'requested_interval_seconds': delay,
                            'resume_to_stop_seconds': stopped - resumed, 'stack': stack})
    finally:
        os.close(pidfd)
    exclusive, inclusive, edges = collections.Counter(), collections.Counter(), collections.Counter()
    for item in samples:
        names = [frame['function'] for frame in item['stack']]
        if names:
            exclusive[names[0]] += 1
        inclusive.update(set(names))
        edges.update(set(zip(names[1:], names[:-1])))
    result = {'method': 'GDB jittered wall-stack sampling; profiler timings are not benchmark timings',
              'gdb_version': gdb.VERSION, 'interval_ms': interval * 1000, 'samples': samples,
              'elapsed_seconds': time.monotonic() - start, 'exit_code': state['exit_code'],
              'functions': [{'function': name, 'exclusive_samples': exclusive[name], 'inclusive_samples': count,
                             'exclusive_percent': 100 * exclusive[name] / len(samples), 'inclusive_percent': 100 * count / len(samples)}
                            for name, count in inclusive.most_common()],
              'edges': [{'caller': caller, 'callee': callee, 'samples': count} for (caller, callee), count in edges.most_common()]}
    output.write_text(json.dumps(result, indent=2) + '\n')
    if state['exit_code'] != 0 or not samples:
        raise RuntimeError(f"Inferior exit {state['exit_code']}, samples {len(samples)}")


sample()
