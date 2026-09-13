"""Runtime CLI regressions; run against CHAMPSIM_BINARY or the local build."""
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('CHAMPSIM_BINARY', ROOT / 'bin' / 'champsim')).resolve()


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

    def test_native_rejects_an_operable_clock_that_rounds_to_zero(self):
        result = self.knobs(*self.native_settings(), 'cache.llc.frequency=2000000')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('clock period', result.stderr)
        self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
