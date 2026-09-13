"""Runtime CLI regressions; run against CHAMPSIM_BINARY or the local build."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import tomllib
import unittest

try:
    import resource
except ImportError:  # not POSIX
    resource = None

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('CHAMPSIM_BINARY', ROOT / 'bin' / 'champsim')).resolve()

sys.path.insert(0, str(ROOT / 'test' / 'ramulator2'))
from generate_trace import generate  # noqa: E402

GUARD_WARNING = 'WARNING: sim.deadlock_cycle'


def no_core_dump():
    if resource is not None:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


@unittest.skipUnless(BINARY.is_file(), 'build the ChampSim executable first')
class RamulatorCliTests(unittest.TestCase):
    def knobs(self, *settings):
        command = [str(BINARY), '--knobs']
        for setting in settings:
            command.extend(['--set', setting])
        return subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=30)

    def effective(self, *settings):
        result = self.knobs(*settings)
        self.assertEqual(result.returncode, 0, result.stderr)
        return tomllib.loads(result.stdout)

    def native_settings(self, fixture='ddr4'):
        if '# DRAM backends (dram-model): legacy, ramulator2' not in self.knobs().stdout:
            self.skipTest('native backend is disabled in this binary')
        return ('dram-model=ramulator2', f'ramulator2.config=configs/ramulator2/{fixture}.yaml')

    def cores(self):
        listing = self.knobs().stdout
        return len(re.findall(r'^ooo_cpu\.cpu[0-9]+\.frequency\s*=', listing, re.MULTILINE))

    def test_legacy_preserves_default_and_explicit_no_progress_threshold(self):
        self.assertEqual(self.effective()['sim']['deadlock_cycle'], 500)
        self.assertEqual(self.effective('sim.deadlock_cycle=17')['sim']['deadlock_cycle'], 17)

    def test_native_default_allows_ten_microseconds_at_actual_fastest_clock(self):
        for fixture in ('ddr4', 'lpddr5'):
            with self.subTest(fixture=fixture):
                settings = self.native_settings(fixture)
                self.assertEqual(self.effective(*settings)['sim']['deadlock_cycle'], 40000)
                self.assertEqual(self.effective(*settings, 'cache.llc.frequency=5000')['sim']['deadlock_cycle'], 50000)
                # 6000 MHz rounds to 166 ps: 60240 ticks fall short of 10 us.
                self.assertEqual(self.effective(*settings, 'ooo_cpu.cpu0.frequency=6000')['sim']['deadlock_cycle'], 60241)

    def test_native_explicit_no_progress_override_remains_authoritative(self):
        settings = self.native_settings()
        for ticks in (1, 500, 90000):
            with self.subTest(ticks=ticks):
                self.assertEqual(self.effective(*settings, f'sim.deadlock_cycle={ticks}')['sim']['deadlock_cycle'], ticks)
        result = self.knobs(*settings, 'sim.deadlock_cycle=0')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('sim.deadlock_cycle', result.stderr)

    def test_native_rejects_blockhammer_before_it_casts_the_external_frontend(self):
        # BlockHammer's setup() static_casts the frontend to its BHO3 CPU. With
        # ChampSim's External shim that is undefined behaviour: a SIGSEGV during
        # construction or a silently inert controller, depending on memory
        # layout. Only a subprocess survives the crash.
        self.native_settings()
        source = (ROOT / 'configs' / 'ramulator2' / 'ddr4.yaml').read_text()
        self.assertEqual(source.count('impl: GenericDDR'), 1)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'blockhammer.yaml'
            path.write_text(source.replace('impl: GenericDDR', 'impl: BlockHammer'))
            result = self.knobs('dram-model=ramulator2', f'ramulator2.config={path}')
        self.assertEqual(result.returncode, 1, f'signal/rc {result.returncode}: {result.stderr}')
        self.assertIn("controller impl 'BlockHammer' is not supported with ChampSim's External frontend", result.stderr)
        self.assertIn('supported: GenericDDR, LPDDR5, LPDDR6, GDDR7, HBM12, HBM34, PRAC', result.stderr)
        self.assertEqual(result.stdout, '')

    def test_native_warns_when_an_explicit_guard_is_shorter_than_ten_microseconds(self):
        settings = self.native_settings()
        with tempfile.TemporaryDirectory() as tmp:
            # What a legacy --knobs dump or statistics document carries, converted
            # as the migration advice used to say: pmem removed, native selected.
            converted = Path(tmp) / 'converted.toml'
            converted.write_text('dram-model = "ramulator2"\n\n[sim]\ndeadlock_cycle = 500\n')
            cases = {
                '--set 500': ([], ('sim.deadlock_cycle=500',), 500),
                '--config 500': (['--config', str(converted)], (), 500),
                '--set 39999': ([], ('sim.deadlock_cycle=39999',), 39999),
            }
            for name, (arguments, extra, ticks) in cases.items():
                with self.subTest(case=name):
                    command = [str(BINARY), '--knobs', *arguments]
                    for setting in (*settings, *extra):
                        command.extend(['--set', setting])
                    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    # The value stays authoritative and stdout stays a TOML document.
                    self.assertEqual(tomllib.loads(result.stdout)['sim']['deadlock_cycle'], ticks)
                    warnings = [line for line in result.stderr.splitlines() if line.startswith(GUARD_WARNING)]
                    self.assertEqual(len(warnings), 1, result.stderr)
                    self.assertIn(f'sim.deadlock_cycle = {ticks}', warnings[0])
                    self.assertIn(f'{ticks * 250} ps', warnings[0])
                    self.assertIn('40000', warnings[0])
                    self.assertIn('500', warnings[0])
                    self.assertIn('remove', warnings[0])
                    self.assertNotIn(GUARD_WARNING, result.stdout)

    def test_no_guard_warning_when_the_key_is_absent_long_enough_or_legacy(self):
        cases = {'legacy explicit 500': ('sim.deadlock_cycle=500',)}
        if '# DRAM backends (dram-model): legacy, ramulator2' in self.knobs().stdout:
            settings = self.native_settings()
            cases.update({
                'native absent': settings,
                'native explicit 40000': (*settings, 'sim.deadlock_cycle=40000'),
                'native explicit 90000': (*settings, 'sim.deadlock_cycle=90000'),
            })
        for name, settings in cases.items():
            with self.subTest(case=name):
                result = self.knobs(*settings)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn(GUARD_WARNING, result.stderr)

    def test_no_progress_abort_delivers_every_diagnostic_through_a_pipe(self):
        self.assert_no_progress_abort_delivers_every_diagnostic(lambda record: record, ticks=1)

    def test_no_progress_abort_diagnostics_survive_unrenamed_register_ids(self):
        # Real traces name architectural registers past the 128-entry physical
        # register file (706.stockfish_r uses 155). The deadlock printer counts
        # dependencies of IFETCH/DECODE/DISPATCH entries, which are not renamed
        # yet: an unchecked lookup threw std::out_of_range, std::terminate cut
        # off every later operable, and the buffered stdout was lost.
        def unrenamed_register_ids(record):
            record[12] = 155  # source_registers[0]
            record[13] = 255  # source_registers[1]
            return record
        # Two ticks: a one-tick stall comes before any instruction is fetched,
        # and the queues it prints are empty.
        self.assert_no_progress_abort_delivers_every_diagnostic(unrenamed_register_ids, ticks=2, queue_entries=True)

    def assert_no_progress_abort_delivers_every_diagnostic(self, rewrite, ticks, queue_entries=False):
        backends = {'legacy': ((), re.compile(r'^(\[WQ\] entry: +[0-9]+ .*|WQ empty)$'))}
        if '# DRAM backends (dram-model): legacy, ramulator2' in self.knobs().stdout:
            backends['ramulator2'] = (self.native_settings(), re.compile(r'^  (Last completion [0-9]+ ps ago|No native completion yet)$'))
        cores = self.cores()
        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp) / 'generated.champsim2'
            generate(generated, 4096)
            records = generated.read_bytes()
            trace = Path(tmp) / 'stall.champsim2'
            trace.write_bytes(b''.join(bytes(rewrite(bytearray(records[offset:offset + 512]))) for offset in range(0, len(records), 512)))
            for backend, (settings, final_line) in backends.items():
                with self.subTest(backend=backend):
                    command = [str(BINARY), '--trace-version', '2', '-w', '0', '-i', '1000', '--hide-heartbeat']
                    for setting in (*settings, f'sim.deadlock_cycle={ticks}'):
                        command.extend(['--set', setting])
                    command.extend(['--', *([str(trace)] * cores)])
                    # stdout is a pipe, as in a batch job: fully buffered.
                    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=60, preexec_fn=no_core_dump)
                    self.assertEqual(result.returncode, -signal.SIGABRT, result.stderr.decode(errors='replace'))
                    stdout = result.stdout.decode(errors='replace')
                    self.assertIn('DEADLOCK!', stdout)
                    if queue_entries:
                        self.assertIn('num_reg_dependent', stdout)
                    # The memory backend is the last operable, so its diagnostic is
                    # the last thing printed -- and the first thing a lost buffer loses.
                    lines = [line for line in stdout.splitlines() if line.strip()]
                    self.assertRegex(lines[-1], final_line)

    def test_native_rejects_an_operable_clock_that_rounds_to_zero(self):
        result = self.knobs(*self.native_settings(), 'cache.llc.frequency=2000000')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('clock period', result.stderr)
        self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
