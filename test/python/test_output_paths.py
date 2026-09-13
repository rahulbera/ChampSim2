"""Run the CLI against disposable inputs: output validation must never erase them."""
import hashlib
import os
from pathlib import Path
import re
import shutil
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

    @unittest.skipUnless(os.path.exists("/dev/fd"), "needs /dev/fd")
    def test_refusal_through_a_descriptor_name_does_not_blame_a_trace(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            notes = Path(tmp) / "notes.txt"
            notes.write_text("precious\n")
            with notes.open("rb") as held:
                descriptor = held.fileno()
                result = self.simulate(tmp, f"/dev/fd/{descriptor}", pass_fds=(descriptor,))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a ChampSim statistics document", result.stderr)
            self.assertNotIn("trace", result.stderr.split("ERROR:", 1)[1])
            self.assertEqual(notes.read_text(), "precious\n")

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

    def test_output_spelled_through_a_missing_or_non_directory_component_is_refused(self):
        # The kernel cannot open any of these names. Folding '..' by text instead
        # turned each into an existing file, which the run then replaced.
        cases = {
            "missing/../configuration": lambda tmp: "missing/../machine.toml",
            "absolute missing/../configuration": lambda tmp: f"{tmp}/missing/../machine.toml",
            "missing/../trace": lambda tmp: "missing/../trace.champsim2",
            "regular file/../notes": lambda tmp: "machine.toml/../notes.txt",
            "dangling symlink to missing/../notes": lambda tmp: self.symlink(tmp, "latest.toml", "missing/../notes.txt"),
        }
        for case, spelling in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                trace = self.trace(tmp)
                config = Path(tmp) / "machine.toml"
                config.write_text("[ooo_cpu.cpu0]\nrob_size = 352\n")
                notes = Path(tmp) / "notes.txt"
                notes.write_text("precious\n")
                protected = {path: path.read_bytes() for path in (trace, config, notes)}
                output = spelling(tmp)
                before = sorted(path.name for path in Path(tmp).iterdir())

                result = self.simulate(tmp, output, "--config", str(config))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("cannot open", result.stderr)
                self.assertNotIn("ChampSim completed", result.stdout)
                self.assertEqual({path: path.read_bytes() for path in protected}, protected)
                self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), before)

    @staticmethod
    def symlink(directory, name, target):
        link = Path(directory) / name
        link.symlink_to(target)
        return name

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

    def test_long_output_name_is_replaced_by_rename(self):
        # NAME_MAX is 255 bytes on common filesystems; the temporary sibling's
        # name must not grow with the target's.
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            name = "r" * 240 + ".toml"
            for instructions in (500, 1000):
                result = self.simulate(tmp, name, instructions=instructions)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads((Path(tmp) / name).read_text())["meta"]["simulation_instructions"], instructions)
            self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), sorted([name, "trace.champsim2"]))

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "permissions do not bind the superuser")
    def test_files_in_a_read_only_directory_are_written_in_place(self):
        for existing in ("empty file", "statistics document"):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                results = Path(tmp) / "results"
                results.mkdir()
                document = results / "run.toml"
                if existing == "empty file":
                    document.touch()
                else:
                    first = self.simulate(tmp, document, instructions=500)
                    self.assertEqual(first.returncode, 0, first.stderr)
                results.chmod(0o555)
                try:
                    result = self.simulate(tmp, document, instructions=1000)
                finally:
                    results.chmod(0o755)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
                self.assertEqual(sorted(path.name for path in results.iterdir()), ["run.toml"])

    def test_hard_linked_statistics_document_updates_every_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            other = Path(tmp) / "latest.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)
            other.hardlink_to(document)
            result = self.simulate(tmp, document, instructions=1000)
            self.assertEqual(result.returncode, 0, result.stderr)
            for path in (document, other):
                self.assertEqual(tomllib.loads(path.read_text())["meta"]["simulation_instructions"], 1000)
            self.assertEqual(document.stat().st_nlink, 2)
            self.assertTrue(os.path.samefile(document, other))

    def test_failed_rename_writes_the_statistics_document_in_place(self):
        # A single file bind-mounted into a container refuses rename (EBUSY)
        # but can be written; strace injects that failure without privileges.
        strace = shutil.which("strace")
        if strace is None:
            self.skipTest("strace is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            probe = subprocess.run([strace, "-f", "-o", os.devnull, "-e", "inject=rename,renameat,renameat2:error=EBUSY", "true"], capture_output=True, timeout=30)
            if probe.returncode != 0:
                self.skipTest(f"strace cannot inject faults here: {probe.stderr.decode(errors='replace')}")
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)

            arguments = ["--trace-version", "2", "-w", "0", "-i", "1000", "--hide-heartbeat", "--toml", str(document), "--"]
            command = [strace, "-f", "-o", os.devnull, "-e", "inject=rename,renameat,renameat2:error=EBUSY", str(BINARY), *arguments]
            command += [str(Path(tmp) / "trace.champsim2")] * self.cores
            result = subprocess.run(command, cwd=tmp, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("in place", result.stderr)
            self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
            self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"])

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

    def assert_report_then_document(self, text, previous=""):
        """The plain report, then one parseable statistics document to the end."""
        self.assertTrue(text.startswith(previous), text[:200])
        completed = text.find("ChampSim completed all CPUs")
        signature = text.find("# ChampSim statistics.")
        self.assertGreaterEqual(completed, 0, text[-400:])
        self.assertGreater(signature, completed)
        self.assertEqual(tomllib.loads(text[signature:])["meta"]["simulation_instructions"], 1000)

    def test_standard_stream_targets_receive_the_document_after_the_report(self):
        # Replacing the file a shell redirected stdout to would unlink the log
        # the report was written to, and a non-empty log is not a statistics
        # document: the stream itself is the target.
        cases = {
            "/dev/stdout": ("/dev/stdout", "stdout"),
            "/proc/self/fd/1": ("/proc/self/fd/1", "stdout"),
            "the log's own name": ("run.log", "stdout"),
            "/dev/stderr": ("/dev/stderr", "stderr"),
        }
        for case, (output, stream) in cases.items():
            if output.startswith("/") and not os.path.exists(output):
                continue
            for previous in ("", "previous line 1\nprevious line 2\n"):
                with self.subTest(case=case, previous=bool(previous)), tempfile.TemporaryDirectory() as tmp:
                    trace = self.trace(tmp)
                    log = Path(tmp) / "run.log"
                    log.write_text(previous)
                    inode = log.stat().st_ino
                    arguments = ["--trace-version", "2", "-w", "0", "-i", "1000", "--hide-heartbeat", "--toml", output, "--", *([str(trace)] * self.cores)]
                    with log.open("ab") as appended:
                        redirect = {"stdout": appended, "stderr": subprocess.PIPE} if stream == "stdout" else {"stdout": subprocess.PIPE, "stderr": appended}
                        result = subprocess.run([str(BINARY), *arguments], cwd=tmp, timeout=120, **redirect)
                    text = log.read_text()
                    self.assertEqual(result.returncode, 0, (result.stderr or result.stdout or b"").decode(errors="replace") + text[-400:])
                    if stream == "stdout":
                        self.assert_report_then_document(text, previous)
                    else:
                        self.assertTrue(text.startswith(previous))
                        self.assertIn("ChampSim completed all CPUs", result.stdout.decode())
                        signature = text.find("# ChampSim statistics.")
                        self.assertEqual(tomllib.loads(text[signature:])["meta"]["simulation_instructions"], 1000)
                    self.assertEqual(log.stat().st_ino, inode)
                    self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.log", "trace.champsim2"])

        with self.subTest(case="/dev/stdout through a pipe"), tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            result = self.simulate(tmp, "/dev/stdout")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assert_report_then_document(result.stdout)

    def test_special_files_are_written_in_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            if os.path.exists("/dev/null"):
                with self.subTest(output="/dev/null"):
                    result = self.simulate(tmp, "/dev/null")
                    self.assertEqual(result.returncode, 0, result.stderr)

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
