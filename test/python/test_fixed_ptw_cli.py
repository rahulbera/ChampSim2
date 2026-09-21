"""The fixed translation timer must fit within the default no-progress guard."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('CHAMPSIM_BINARY', ROOT / 'bin' / 'champsim')).resolve()


@unittest.skipUnless(BINARY.is_file(), 'build the ChampSim executable first')
class FixedPtwCliTests(unittest.TestCase):
    def knobs(self, *settings):
        argv = [str(BINARY), '--knobs', '--set', 'dram-model=legacy', '--set', 'pmem.bank_rows=64',
                '--set', 'ptw.cpu0_ptw.model=fixed']
        for setting in settings:
            argv += ['--set', setting]
        return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=30)

    def effective(self, *settings):
        result = self.knobs(*settings)
        self.assertEqual(result.returncode, 0, result.stderr)
        return tomllib.loads(result.stdout)

    def test_long_fixed_delay_extends_the_default_watchdog(self):
        guard = self.effective('ptw.cpu0_ptw.fixed_latency=100000')['sim']['deadlock_cycle']
        self.assertGreater(guard, 100000)

    def test_default_200_cpu_cycles_fit_even_with_a_slower_cpu(self):
        guard = self.effective('ooo_cpu.cpu0.frequency=500')['sim']['deadlock_cycle']
        self.assertGreater(guard, 1600)  # 200 * 2000 ps / 250 ps.

    def test_explicit_override_and_short_delay_keep_their_values(self):
        self.assertEqual(self.effective()['sim']['deadlock_cycle'], 500)
        self.assertEqual(self.effective('ptw.cpu0_ptw.fixed_latency=100000', 'sim.deadlock_cycle=17')['sim']['deadlock_cycle'], 17)

    def test_unrepresentable_default_guard_is_rejected(self):
        result = self.knobs('ptw.cpu0_ptw.fixed_latency=2147483647')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('fixed PTW', result.stderr)

    def test_long_delayed_translations_complete_a_real_simulation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / 'walks.champsimtrace'
            record = struct.Struct('<QBB2B4B2Q4Q')
            with trace.open('wb') as stream:
                for i in range(4096):
                    stream.write(record.pack(0x400000, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x1000000 + i * 4096, 0, 0, 0))
            trace.chmod(0o444)
            output = root / 'stats.toml'
            result = subprocess.run([
                str(BINARY), '--set', 'dram-model=legacy', '--set', 'pmem.bank_rows=64',
                '--set', 'ptw.cpu0_ptw.model=fixed', '--set', 'ptw.cpu0_ptw.fixed_latency=10000',
                '--set', 'cache.cpu0_stlb.sets=1', '--set', 'cache.cpu0_stlb.ways=1',
                '--hide-heartbeat', '-w', '16', '-i', '512', '--toml', str(output), '--', str(trace),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout[-2000:])
            with output.open('rb') as stream:
                stats = tomllib.load(stream)
            self.assertEqual(stats['config']['ptw']['cpu0_ptw']['fixed_latency'], 10000)
            self.assertIn('Simulation finished CPU 0', result.stdout)


if __name__ == '__main__':
    unittest.main()
