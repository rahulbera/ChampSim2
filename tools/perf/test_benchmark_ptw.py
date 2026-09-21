import unittest

from benchmark_ptw import parse_counts, throughput


class BenchmarkTests(unittest.TestCase):
    def test_champsim_counts_include_actual_warmup_and_retirement_overshoot(self):
        text = """Warmup finished CPU 0 instructions: 1002 cycles: 700 cumulative IPC: 1.4
Warmup complete CPU 0 instructions: 1002 cycles: 700 cumulative IPC: 1.4
Simulation finished CPU 0 instructions: 5003 cycles: 4000 cumulative IPC: 1.25
Simulation complete CPU 0 instructions: 5003 cycles: 4000 cumulative IPC: 1.25
"""
        self.assertEqual(parse_counts("fixed", text), {
            "warmup_instructions": 1002, "roi_instructions": 5003,
            "warmup_cycles": 700, "roi_cycles": 4000,
        })

    def test_hermes_uses_its_distinct_completion_lines(self):
        text = "Warmup complete CPU 0 instructions: 1001 cycles: 501\nFinished CPU 0 instructions: 5002 cycles: 3000\n"
        self.assertEqual(parse_counts("hermes", text)["roi_instructions"], 5002)
        self.assertEqual(parse_counts("hermes", text)["warmup_instructions"], 1001)

    def test_incomplete_or_multiple_runs_are_not_a_measurement(self):
        for text in ("", "Warmup finished CPU 0 instructions: 10 cycles: 10\n",
                     "Warmup finished CPU 0 instructions: 10 cycles: 10\n" * 2
                     + "Simulation finished CPU 0 instructions: 10 cycles: 10\n"):
            with self.assertRaises(ValueError):
                parse_counts("detailed", text)

    def test_kips_uses_both_phases_with_whole_process_time(self):
        self.assertEqual(throughput({"warmup_instructions": 1000, "roi_instructions": 5000}, 2.0), 3.0)


if __name__ == "__main__":
    unittest.main()
