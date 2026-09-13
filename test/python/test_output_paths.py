"""Run the CLI against disposable inputs: output validation must never erase them."""
import hashlib
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import tomllib
import unittest

try:
    import resource
except ImportError:  # not POSIX
    resource = None

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()

sys.path.insert(0, str(ROOT / "test" / "ramulator2"))
from generate_trace import generate  # noqa: E402


@unittest.skipUnless(BINARY.is_file(), "build bin/champsim to exercise output path validation")
class OutputPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run([str(BINARY), "--knobs"], text=True, capture_output=True, check=True, timeout=30)
        cls.cores = len(re.findall(r"^ooo_cpu\.cpu[0-9]+\.frequency\s*=", result.stdout, re.MULTILINE))
        if not cls.cores:
            raise AssertionError("--knobs did not identify any configured cores")

    @staticmethod
    def trace(directory, name="trace.champsim2"):
        """A short valid v2 trace, left writable: a read-only input would be protected by permissions alone."""
        path = Path(directory) / name
        generate(path, 4096)
        path.chmod(0o644)
        return path

    @staticmethod
    def run_binary(arguments, directory, timeout=120, **options):
        return subprocess.run([str(BINARY), *arguments], cwd=directory, capture_output=True, text=True, timeout=timeout, **options)

    def simulate(self, directory, output, *extra, trace=None, instructions=1000, timeout=120, **options):
        trace = trace or Path(directory) / "trace.champsim2"
        arguments = ["--trace-version", "2", "-w", "0", "-i", str(instructions), "--hide-heartbeat", *extra, "--toml", str(output), "--"]
        return self.run_binary([*arguments, *([str(trace)] * self.cores)], directory, timeout, **options)

    def test_optional_output_cannot_truncate_a_consumed_trace_argument(self):
        for option in ("--toml", "--toml="):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as tmp:
                trace = Path(tmp) / "trace.input"
                trace.write_bytes(b"a disposable trace that must survive")
                result = subprocess.run([str(BINARY), option, str(trace)], capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(trace.read_bytes(), b"a disposable trace that must survive")
                self.assertIn("trace(s)", result.stderr)

    def test_optional_output_cannot_overwrite_a_trace_when_one_too_many_are_given(self):
        # With one trace path more than the binary has cores, the optional value
        # takes the first path and the rest satisfy the count check, so the
        # output check is the only thing standing between that trace and its loss.
        for option in ("--toml", "--toml="):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as tmp:
                traces = [self.trace(tmp, f"trace{index}.champsim2") for index in range(self.cores + 1)]
                digests = [hashlib.sha256(trace.read_bytes()).hexdigest() for trace in traces]
                arguments = ["--trace-version", "2", "-w", "0", "-i", "1000", "--hide-heartbeat", option, *map(str, traces)]
                result = self.run_binary(arguments, tmp)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([hashlib.sha256(trace.read_bytes()).hexdigest() for trace in traces], digests)
                self.assertIn(f"'{traces[0]}'", result.stderr)
                self.assertIn("not a ChampSim statistics document", result.stderr)
                self.assertIn("--toml=FILE", result.stderr)

    def test_output_cannot_replace_the_configuration_it_was_given(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            config = Path(tmp) / "machine.toml"
            config.write_text("[ooo_cpu.cpu0]\nrob_size = 352\n")
            before = config.read_bytes()
            result = self.simulate(tmp, config, "--config", str(config))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(config.read_bytes(), before)
            self.assertIn("not a ChampSim statistics document", result.stderr)

    def test_output_cannot_alias_a_trace_by_name_symlink_or_hardlink(self):
        for alias in ("name", "relative", "symlink", "hardlink"):
            with self.subTest(alias=alias), tempfile.TemporaryDirectory() as tmp:
                trace = Path(tmp) / "trace.input"
                trace.write_bytes(b"a disposable trace that must survive")
                output = Path(tmp) / "output.toml"
                if alias == "name":
                    output = trace
                elif alias == "relative":
                    output = Path("trace.input")
                elif alias == "symlink":
                    output.symlink_to(trace)
                else:
                    output.hardlink_to(trace)
                argv = [str(BINARY), "--toml", str(output), "--", *([str(trace)] * self.cores)]
                result = subprocess.run(argv, cwd=tmp, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(trace.read_bytes(), b"a disposable trace that must survive")
                self.assertIn("aliases an input trace", result.stderr)

    def test_startup_failure_leaves_an_existing_statistics_document_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document)
            self.assertEqual(first.returncode, 0, first.stderr)
            before = document.read_bytes()
            result = self.simulate(tmp, document, "--set", "ooo_cpu.cpu0.no_such_knob=1")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no_such_knob", result.stderr)
            self.assertEqual(document.read_bytes(), before)

    def test_startup_failure_and_knobs_create_no_output(self):
        for case in ("startup failure", "--knobs"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                trace = self.trace(tmp)
                output = Path(tmp) / "never.toml"
                if case == "--knobs":
                    result = self.run_binary(["--knobs", "--toml", str(output)], tmp, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    result = self.simulate(tmp, output, "--set", "ooo_cpu.cpu0.no_such_knob=1")
                    self.assertNotEqual(result.returncode, 0)
                self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), [trace.name])

    def test_successful_run_replaces_a_statistics_document(self):
        umask = os.umask(0)
        os.umask(umask)
        for existing in ("document", "document via symlink", "empty file", "nothing, via a dangling symlink"):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                results = Path(tmp) / "results"
                results.mkdir()
                document = results / "run.toml"
                if existing.startswith("document"):
                    first = self.simulate(tmp, document, instructions=500)
                    self.assertEqual(first.returncode, 0, first.stderr)
                elif existing == "empty file":
                    document.touch()
                if document.exists():
                    document.chmod(0o640)
                output = document
                if "symlink" in existing:
                    output = Path(tmp) / "latest.toml"
                    output.symlink_to(document)

                result = self.simulate(tmp, output, instructions=1000)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
                # Replaced as the same file would have been: its permissions
                # (or a new file's usual ones), the link that named it, and no
                # temporary file left beside it.
                expected_mode = 0o666 & ~umask if existing.startswith("nothing") else 0o640
                self.assertEqual(stat.S_IMODE(document.stat().st_mode), expected_mode)
                self.assertEqual(output.is_symlink(), "symlink" in existing)
                self.assertEqual(sorted(path.name for path in results.iterdir()), ["run.toml"])

    @unittest.skipUnless(resource is not None and hasattr(signal, "SIGXFSZ"), "needs RLIMIT_FSIZE to make the final write fail")
    def test_failed_write_leaves_an_existing_statistics_document_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document)
            self.assertEqual(first.returncode, 0, first.stderr)
            before = document.read_bytes()
            limit = len(before) // 2

            def limit_written_file_size():
                # Ignored, the signal becomes EFBIG: a full disk, as the writer sees it.
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))

            result = self.simulate(tmp, document, instructions=500, preexec_fn=limit_written_file_size)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("failed to write", result.stderr)
            self.assertEqual(document.read_bytes(), before)
            self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"])

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "permissions do not bind the superuser")
    def test_read_only_statistics_document_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document)
            self.assertEqual(first.returncode, 0, first.stderr)
            document.chmod(0o444)
            before = document.read_bytes()
            result = self.simulate(tmp, document)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot open", result.stderr)
            self.assertEqual(document.read_bytes(), before)

    def test_special_files_are_written_in_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            for device in ("/dev/null", "/dev/stdout"):
                if not os.path.exists(device):
                    continue
                with self.subTest(output=device):
                    result = self.simulate(tmp, device)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if device == "/dev/stdout":
                        # The document's own stream is not the report's buffer, so
                        # it need not follow the report; it only has to arrive.
                        self.assertIn("# ChampSim statistics.", result.stdout)

            if not hasattr(os, "mkfifo"):
                return
            with self.subTest(output="fifo"):
                fifo = Path(tmp) / "stats.fifo"
                os.mkfifo(fifo)
                received = []
                reader = threading.Thread(target=lambda: received.append(fifo.read_bytes()), daemon=True)
                reader.start()
                try:
                    result = self.simulate(tmp, fifo, timeout=30)
                except subprocess.TimeoutExpired:
                    result = None
                if reader.is_alive():
                    # The simulator never opened the FIFO: release the reader.
                    try:
                        os.close(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK))
                    except OSError:
                        pass
                reader.join(timeout=10)
                self.assertIsNotNone(result, "the simulator blocked writing the TOML statistics to a FIFO")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(received), 1)
                self.assertEqual(tomllib.loads(received[0].decode())["meta"]["simulation_instructions"], 1000)
