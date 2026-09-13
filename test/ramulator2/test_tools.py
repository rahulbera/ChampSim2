import copy
import csv
import io
from pathlib import Path
import struct
import tempfile
import unittest

from generate_trace import generate
from run_integration import check_cpu_traffic
from run_oracle import check_leaves, check_events, compare_events


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
