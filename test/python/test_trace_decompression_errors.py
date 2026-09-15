"""Exercise compressed-trace failures through the real ChampSim CLI."""

import base64
import bz2
import gzip
import lzma
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()

# Four 64-byte v1 records compressed as one zstd frame. Keeping the fixture in
# the test avoids requiring a zstd command or Python extension at test time.
ZSTD_V1 = base64.b64decode("KLUv/WAAAJUAADAAEAAECAwEBCcEQjgOgQm4AQ==")
# One 512-byte v2 record compressed as one zstd frame.
ZSTD_V2_ONE = base64.b64decode("KLUv/WAAAVUAABgAEAABAPoqGAI=")


def records(count, size):
    result = bytearray(count * size)
    for index in range(count):
        struct.pack_into("<Q", result, index * size, 0x1000 + 4 * index)
    return bytes(result)


@unittest.skipUnless(BINARY.is_file(), "set CHAMPSIM_BINARY to exercise trace decoder errors")
class TraceDecompressionErrorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run([str(BINARY), "--knobs"], text=True, capture_output=True, check=True, timeout=30)
        cls.cores = len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.frequency\s*=", result.stdout, re.MULTILINE))
        if not cls.cores:
            raise AssertionError("--knobs did not identify any configured cores")

    def run_trace(self, trace, version=1):
        argv = [str(BINARY), "--hide-heartbeat", "-w", "0", "-i", "4"]
        if version != 1:
            argv.extend(["--trace-version", str(version)])
        argv.extend(["--", *([str(trace)] * self.cores)])
        return subprocess.run(argv, text=True, capture_output=True, timeout=30)

    def require_decoder_error(self, result, trace, codec):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR:", result.stderr)
        self.assertIn(str(trace), result.stderr)
        self.assertIn(codec, result.stderr.lower())
        self.assertNotIn("ChampSim completed all CPUs", result.stdout + result.stderr)

    def test_v1_codecs_accept_clean_eof_and_report_corruption_and_truncation(self):
        payload = records(4, 64)
        compressed = {
            "gzip": ("gz", gzip.compress(payload)),
            "bzip2": ("bz2", bz2.compress(payload)),
            "xz": ("xz", lzma.compress(payload)),
            "zstd": ("zst", ZSTD_V1),
        }
        with tempfile.TemporaryDirectory() as tmp:
            for codec, (suffix, contents) in compressed.items():
                with self.subTest(codec=codec, condition="valid"):
                    trace = Path(tmp) / f"valid.champsimtrace.{suffix}"
                    trace.write_bytes(contents)
                    result = self.run_trace(trace)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("ChampSim completed all CPUs", result.stdout)

                with self.subTest(codec=codec, condition="corrupt"):
                    trace = Path(tmp) / f"corrupt.champsimtrace.{suffix}"
                    trace.write_bytes(bytes([contents[0] ^ 0xFF]) + contents[1:])
                    self.require_decoder_error(self.run_trace(trace), trace, codec)

                with self.subTest(codec=codec, condition="truncated"):
                    trace = Path(tmp) / f"truncated.champsimtrace.{suffix}"
                    trace.write_bytes(contents[:-4])
                    self.require_decoder_error(self.run_trace(trace), trace, codec)

    def test_concatenated_zstd_frames_remain_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "concatenated.champsimtrace.zst"
            trace.write_bytes(ZSTD_V1 + ZSTD_V1)
            result = self.run_trace(trace)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("ChampSim completed all CPUs", result.stdout)

    def test_v2_errors_escape_both_probe_and_simulation_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            early = Path(tmp) / "early.champsim2.zst"
            early.write_bytes(bytes([ZSTD_V2_ONE[0] ^ 0xFF]) + ZSTD_V2_ONE[1:])
            self.require_decoder_error(self.run_trace(early, version=2), early, "zstd")

            later = Path(tmp) / "later.champsim2.zst"
            corrupt_second_frame = bytes([ZSTD_V2_ONE[0] ^ 0xFF]) + ZSTD_V2_ONE[1:]
            later.write_bytes(ZSTD_V2_ONE + corrupt_second_frame)
            self.require_decoder_error(self.run_trace(later, version=2), later, "zstd")


if __name__ == "__main__":
    unittest.main()
