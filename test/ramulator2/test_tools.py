import contextlib
import copy
import csv
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import yaml

from generate_trace import generate
import oracle_mutants
import oracle_variants
from run_integration import check_cpu_traffic, equal_leaves
import run_differential
import run_mutants
from run_oracle import check_leaves, check_events, compare_events

REPO = Path(__file__).resolve().parents[2]


class OracleComparisonTests(unittest.TestCase):
    def setUp(self):
        self.raw = [
            {"path": ["memory_system", "counter.with.dots"], "text": "9007199254740993"},
            {"path": ["memory_system", "unsigned"], "text": "18446744073709551615"},
            {"path": ["memory_system", "id"], "text": "0"},
            {"path": ["memory_system", "latency"], "text": "nan"},
        ]
        self.typed = [dict(item, kind=kind) for item, kind in zip(self.raw, [0, 1, 4, 2])]

    def test_full_range_integers_and_literal_path_components_compare_exactly(self):
        self.assertEqual(check_leaves(self.raw, self.typed), 4)

    def test_changed_low_integer_bit_is_detected(self):
        changed = copy.deepcopy(self.typed)
        changed[0]["text"] = "9007199254740992"
        with self.assertRaises(AssertionError):
            check_leaves(self.raw, changed)

    def test_missing_leaf_or_wrong_type_fails(self):
        with self.assertRaises(AssertionError):
            check_leaves(self.raw, self.typed[:-1])
        changed = copy.deepcopy(self.typed)
        changed[2]["kind"] = 0
        with self.assertRaises(AssertionError):
            check_leaves(self.raw, changed)


class TraceGeneratorTests(unittest.TestCase):
    def test_trace_encodes_complete_load_and_store_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "generated.champsim2"
            generate(target, 4)
            data = target.read_bytes()
            self.assertEqual(len(data), 2048)
            self.assertEqual(struct.unpack_from("<Q", data, 0)[0], 0x400000)
            self.assertEqual(data[10], 1)
            self.assertEqual(struct.unpack_from("<Q", data, 16)[0], 0x800000)
            self.assertEqual(data[116], 8)
            self.assertEqual(struct.unpack_from("<Q", data, 512 + 32)[0], 0x8010C0)
            self.assertEqual(data[512 + 112], 8)
            self.assertEqual(struct.unpack_from("<Q", data, 512 + 16)[0], 0)

    def test_existing_input_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "trace.input"
            target.write_bytes(b"existing input")
            with self.assertRaises(FileExistsError):
                generate(target, 4)
            self.assertEqual(target.read_bytes(), b"existing input")


class EventComparisonTests(unittest.TestCase):
    def setUp(self):
        with Path(__file__).with_name("stream.csv").open() as source:
            self.transactions = list(csv.DictReader(source))
        self.rows = []
        for transaction in self.transactions:
            row = dict(transaction)
            row["tick"] = row.pop("release_tick")
            row["EVENT"] = "SEND"
            row["synchronous"] = ""
            if row["id"] in ("0", "4"):
                self.rows.append(dict(row, accepted="0"))
            self.rows.append(dict(row, accepted="1"))
            self.rows.append(dict(row, EVENT="CALLBACK", accepted="", synchronous="1" if row["id"] in ("1", "18") else "0"))

    def render(self, rows):
        output = io.StringIO()
        writer = csv.DictWriter(output, fieldnames=["EVENT", "tick", "id", "type", "address", "source", "size", "accepted", "parent", "fragment", "synchronous"])
        writer.writeheader()
        writer.writerows(rows)
        return output.getvalue()

    def test_callback_cycle_mismatch_fails_even_with_equal_totals(self):
        original = self.render(self.rows)
        self.assertEqual(compare_events(original, original, self.transactions)["callbacks"], 22)
        changed = copy.deepcopy(self.rows)
        next(row for row in changed if row["EVENT"] == "CALLBACK")["tick"] = "99"
        with self.assertRaises(AssertionError):
            compare_events(original, self.render(changed), self.transactions)

    def test_repeating_an_accepted_fragment_fails(self):
        self.rows.append(next(row.copy() for row in self.rows if row["EVENT"] == "SEND" and row["accepted"] == "1"))
        with self.assertRaises(AssertionError):
            check_events(self.render(self.rows), self.transactions)


class NativeSourceTests(unittest.TestCase):
    def test_second_core_must_reach_native_counters(self):
        channels = {"channel0": {"read_row_hits_core_0": 12, "read_row_hits_core_1": 7}}
        check_cpu_traffic(channels, 2)
        channels["channel0"]["read_row_hits_core_1"] = 0
        with self.assertRaises(AssertionError):
            check_cpu_traffic(channels, 2)


class ReplayScalarTests(unittest.TestCase):
    def test_integer_counter_cannot_replay_as_float(self):
        original = {"adapter": {"accepted_reads": 2}}
        replay = {"adapter": {"accepted_reads": 2.0}}
        with self.assertRaises(AssertionError):
            equal_leaves(original, replay)

    def test_integer_counter_cannot_replay_as_boolean(self):
        original = {"adapter": {"outstanding_parents": 1}}
        replay = {"adapter": {"outstanding_parents": True}}
        with self.assertRaises(AssertionError):
            equal_leaves(original, replay)

    def test_equal_scalar_types_and_nan_preserve_leaf_count(self):
        original = {"count": 2, "ratio": 2.0, "enabled": True, "name": "native", "undefined": float("nan")}
        replay = {"count": 2, "ratio": 2.0, "enabled": True, "name": "native", "undefined": float("nan")}
        self.assertEqual(equal_leaves(original, replay), 5)


def controllers(path):
    return yaml.safe_load(Path(path).read_text())["memory_system"]


class OracleVariantTests(unittest.TestCase):
    def test_variants_change_only_their_declared_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = oracle_variants.generate(Path(tmp) / "variants")
            runs = {run["name"]: run for run in manifest["runs"]}
            self.assertEqual(len(runs), 12)
            self.assertEqual(len({run["yaml"] for run in runs.values()}), 10)
            fixture = yaml.safe_load((REPO / "configs" / "ramulator2" / "ddr4.yaml").read_text())
            self.assertEqual(yaml.safe_load(Path(runs["ddr4"]["yaml"]).read_text()), fixture)
            asym = controllers(runs["lpddr5-2ch-asym"]["yaml"])["controllers"]
            self.assertEqual([(c["read_buffer_size"], c["write_buffer_size"], c["priority_buffer_size"]) for c in asym], [(1, 1, 2), (4, 3, 2)])
            wide = controllers(runs["ddr4-4ch-interleave2-tiny"]["yaml"])
            self.assertEqual((len(wide["controllers"]), wide["channel_mapper"]["interleave_bits"]), (4, 2))
            self.assertEqual(controllers(runs["ddr4-tx128-tiny"]["yaml"])["controllers"][0]["dram"]["data_payload_bytes"], 128)
            self.assertEqual((runs["ddr4-tx128-tiny"]["tx"], runs["lpddr5"]["tx"], runs["lpddr5"]["period"]), (128, 32, 1453))
            self.assertEqual(runs["ddr4-tiny-2feeders"]["yaml"], runs["ddr4-tiny"]["yaml"])
            self.assertEqual(runs["ddr4-tiny-2feeders"]["feeders"], 2)

    def test_existing_output_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileExistsError):
                oracle_variants.generate(tmp)


PASS_LINE = "seed={seed} yaml=/v.yaml tx=64 feeders=1 ops=10 accepted=4 accepted_by_cpu=3/1 max_backlog={backlog} wall=0.10s PASS"


class CampaignParsingTests(unittest.TestCase):
    def test_totals_sum_counters_keep_maxima_and_skip_constants(self):
        text = "Filters: [.differential]\n" + PASS_LINE.format(seed=1, backlog=7) + "\n" + PASS_LINE.format(seed=2, backlog=5) + "\n"
        verdict = run_differential.summarize(run_differential.parse_log(text), 2, 0)
        self.assertTrue(verdict["ok"], verdict)
        self.assertEqual(verdict["totals"], {"ops": 20, "accepted": 8, "accepted_by_cpu": [6, 2], "max_backlog": 7})

    def test_failed_seed_is_reported_before_the_exit_status(self):
        text = PASS_LINE.format(seed=1, backlog=1) + "\nseed=2 yaml=/v.yaml ops=3 wall=0.01s FAIL seed 2 phase 3 op 9: send mismatch x=1\n"
        verdict = run_differential.summarize(run_differential.parse_log(text), 2, 42)
        self.assertFalse(verdict["ok"])
        self.assertEqual(verdict["discrepancies"], ["seed 2: seed 2 phase 3 op 9: send mismatch x=1", "exit status 42"])

    def test_missing_seed_line_timeout_or_malformed_line_is_a_discrepancy(self):
        records = run_differential.parse_log(PASS_LINE.format(seed=1, backlog=1))
        self.assertIn("1 seed lines for 2 seeds", run_differential.summarize(records, 2, 0)["discrepancies"])
        self.assertIn("timed out", run_differential.summarize(records, 1, None, timed_out=True)["discrepancies"])
        self.assertFalse(run_differential.summarize(run_differential.parse_log("seed=1 PASS"), 1, 0)["ok"])

    def test_an_idle_core_fails_only_when_every_core_is_required(self):
        records = run_differential.parse_log(PASS_LINE.format(seed=1, backlog=1).replace("3/1", "4/0"))
        self.assertTrue(run_differential.summarize(records, 1, 0)["ok"])
        self.assertFalse(run_differential.summarize(records, 1, 0, require_all_cores=True)["ok"])

    def test_runner_writes_a_summary_and_exits_nonzero_on_a_failing_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            binary = tmp / "fake-test-main"
            binary.write_text(f"#!{sys.executable}\n" + """import os, sys
assert sys.argv[1] in ("[.differential]", "[.differential-recovery]"), sys.argv
for seed in range(int(os.environ["DIFF_SEED0"]), int(os.environ["DIFF_SEED0"]) + int(os.environ["DIFF_SEEDS"])):
    bad = "bad" in os.environ["DIFF_YAML"] and seed == 2
    print(f"seed={seed} yaml={os.environ['DIFF_YAML']} tx={os.environ['DIFF_TX']} ops=5 accepted=2 wall=0.01s " + ("FAIL model says no" if bad else "PASS"))
sys.exit(42 if "bad" in os.environ["DIFF_YAML"] else 0)
""")
            binary.chmod(0o755)
            runs = [{"name": name, "variant": name, "feeders": 1, "yaml": f"/{name}.yaml", "tx": 64, "period": 833} for name in ("good", "bad")]
            (tmp / "manifest.json").write_text(json.dumps({"runs": runs}))
            common = ["--binary", str(binary), "--manifest", str(tmp / "manifest.json"), "--seeds", "3", "--jobs", "2", "--cwd", str(tmp)]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_differential.main(common + ["--output-dir", str(tmp / "good"), "--runs", "good"]), 0)
                self.assertEqual(run_differential.main(common + ["--output-dir", str(tmp / "both")]), 1)
            summary = json.loads((tmp / "both" / "summary.json").read_text())
            bad = next(run for run in summary["runs"] if run["name"] == "bad")
            self.assertEqual(bad["discrepancies"][0], "seed 2: model says no")
            self.assertEqual(summary["totals"]["differential"]["ops"], 30)
            with self.assertRaises(FileExistsError):
                run_differential.main(common + ["--output-dir", str(tmp / "good")])


class MutantCatalogueTests(unittest.TestCase):
    def test_every_mutant_applies_exactly_once_to_the_current_source(self):
        self.assertEqual(len({mutant.name.split("-", 1)[0] for mutant in oracle_mutants.MUTANTS}), len(oracle_mutants.MUTANTS))
        for mutant in oracle_mutants.MUTANTS:
            with self.subTest(mutant=mutant.name):
                text = (REPO / mutant.path).read_text()
                self.assertNotEqual(oracle_mutants.apply(text, mutant), text)

    def test_a_stale_or_ambiguous_pattern_is_refused(self):
        mutant = oracle_mutants.Mutant("M99-test", "test", (("twice", "once"),))
        with self.assertRaises(ValueError):
            oracle_mutants.apply("no match", mutant)
        with self.assertRaises(ValueError):
            oracle_mutants.apply("twice twice", mutant)

    def test_equivalence_and_core_limits_decide_what_must_survive(self):
        m01, m12, m19 = oracle_mutants.by_name(["M01", "M12", "M19-cpu-forced-zero"])
        self.assertTrue(m12.equivalent)
        self.assertEqual(run_mutants.judge(m01, 2, True), ("detected", True))
        self.assertEqual(run_mutants.judge(m01, 2, False), ("survived", False))
        self.assertEqual(run_mutants.judge(m12, 2, False), ("equivalent, survived", True))
        self.assertFalse(run_mutants.judge(m12, 2, True)[1])
        self.assertTrue(run_mutants.judge(m19, 1, False)[1])
        self.assertFalse(run_mutants.judge(m19, 2, False)[1])
        with self.assertRaises(KeyError):
            oracle_mutants.by_name(["M98"])

    def test_the_scratch_build_core_count_is_rewritten_exactly_once(self):
        defs = (REPO / "inc" / "defs.h").read_text()
        self.assertIn("inline constexpr std::size_t num_cpus = 4;", run_mutants.set_num_cpus(defs, 4))
        with self.assertRaises(ValueError):
            run_mutants.set_num_cpus("no definition", 4)


class ScratchCopyTests(unittest.TestCase):
    def test_copy_takes_tracked_and_untracked_files_and_leaves_the_source_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, copy_dir = Path(tmp) / "repo", Path(tmp) / "copy"
            repo.mkdir()
            git = ["git", "-C", str(repo), "-c", "user.name=test", "-c", "user.email=test@example.com"]
            subprocess.run(git + ["init", "-q"], check=True)
            (repo / ".gitignore").write_text("ignored.txt\n")
            (repo / "src").mkdir()
            (repo / "src" / "tracked.cc").write_text("tracked\n")
            subprocess.run(git + ["add", "."], check=True)
            subprocess.run(git + ["commit", "-q", "-m", "initial"], check=True)
            (repo / "untracked.py").write_text("untracked\n")
            (repo / "ignored.txt").write_text("ignored\n")
            os.symlink("src/tracked.cc", repo / "link.cc")
            before = subprocess.check_output(git + ["status", "--porcelain"])
            self.assertEqual(run_mutants.copy_repository(repo, copy_dir), 4)
            self.assertEqual((copy_dir / "src" / "tracked.cc").read_text(), "tracked\n")
            self.assertTrue((copy_dir / "untracked.py").exists())
            self.assertFalse((copy_dir / "ignored.txt").exists())
            self.assertEqual(os.readlink(copy_dir / "link.cc"), "src/tracked.cc")
            (copy_dir / "src" / "tracked.cc").write_text("mutated\n")
            self.assertEqual((repo / "src" / "tracked.cc").read_text(), "tracked\n")
            self.assertEqual(subprocess.check_output(git + ["status", "--porcelain"]), before)
