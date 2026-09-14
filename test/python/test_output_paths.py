"""Run the CLI against disposable inputs: output validation must never erase them."""
import errno
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
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

    def test_replay_from_a_statistics_document_writes_that_document_in_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document)
            self.assertEqual(first.returncode, 0, first.stderr)
            recorded = tomllib.loads(document.read_text())
            inode = document.stat().st_ino

            result = self.simulate(tmp, document, "--config", str(document))
            self.assertEqual(result.returncode, 0, result.stderr)
            replayed = tomllib.loads(document.read_text())
            self.assertEqual(replayed["meta"]["config_files"], str(document))
            self.assertEqual((replayed["config"], replayed["meta"]["build_id"]), (recorded["config"], recorded["meta"]["build_id"]))
            self.assertEqual(document.stat().st_ino, inode)

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

    def test_successful_run_writes_a_statistics_document_in_place(self):
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
                inode = document.stat().st_ino if document.exists() else None

                result = self.simulate(tmp, output, instructions=1000)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
                # The same file, with its permissions (or a new file's usual
                # ones), still named by the link that named it, and nothing
                # else beside it.
                expected_mode = 0o666 & ~umask if existing.startswith("nothing") else 0o640
                self.assertEqual(stat.S_IMODE(document.stat().st_mode), expected_mode)
                if inode is not None:
                    self.assertEqual(document.stat().st_ino, inode)
                self.assertEqual(output.is_symlink(), "symlink" in existing)
                self.assertEqual(sorted(path.name for path in results.iterdir()), ["run.toml"])

    def test_long_output_name_is_written(self):
        # NAME_MAX is 255 bytes on common filesystems: a name near it is
        # created, then written again in place.
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

    @staticmethod
    def new_file_group(directory):
        """The group a file created in `directory` gets on Linux."""
        status = os.stat(directory)
        return status.st_gid if status.st_mode & stat.S_ISGID else os.getegid()

    @unittest.skipUnless(hasattr(os, "getgroups") and hasattr(os, "chown"), "needs supplementary groups")
    def test_document_in_another_group_keeps_its_group_and_inode(self):
        # Written in place, not replaced by a new file in the group a new file gets.
        with tempfile.TemporaryDirectory() as tmp:
            groups = [group for group in os.getgroups() if group != self.new_file_group(tmp)]
            if not groups:
                self.skipTest("no supplementary group differs from the group of a new file")
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)
            os.chown(document, -1, groups[0])
            document.chmod(0o640)
            before = document.stat()

            result = self.simulate(tmp, document, instructions=1000)
            self.assertEqual(result.returncode, 0, result.stderr)
            after = document.stat()
            self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
            self.assertEqual((after.st_gid, after.st_ino, stat.S_IMODE(after.st_mode)), (groups[0], before.st_ino, 0o640))
            self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"])

    @unittest.skipUnless(hasattr(os, "getxattr") and shutil.which("setfacl"), "needs setfacl and extended attributes")
    def test_document_with_an_access_acl_keeps_it(self):
        # Written in place, not replaced by a new file with a new file's ACL
        # (none, or the directory's default) instead of this one's named entries.
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)
            document.chmod(0o600)
            granted = subprocess.run([shutil.which("setfacl"), "-m", f"u:{os.getuid() + 1}:r", str(document)], capture_output=True, text=True, timeout=30)
            if granted.returncode != 0:
                self.skipTest(f"setfacl failed here: {granted.stderr.strip()}")
            acl = os.getxattr(document, "system.posix_acl_access")
            before = document.stat()

            result = self.simulate(tmp, document, instructions=1000)
            self.assertEqual(result.returncode, 0, result.stderr)
            after = document.stat()
            self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
            self.assertEqual((after.st_ino, after.st_mode), (before.st_ino, before.st_mode))
            self.assertEqual(os.getxattr(document, "system.posix_acl_access"), acl)
            self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"])

    @staticmethod
    def access_acl(path):
        """The POSIX access ACL attribute of `path`, or None when it has none."""
        try:
            return os.getxattr(path, "system.posix_acl_access")
        except OSError as error:
            if error.errno in (errno.ENODATA, errno.ENOTSUP, errno.EOPNOTSUPP):
                return None
            raise

    def directory_with_a_default_acl(self, parent):
        """A directory whose default ACL denies others and grants nobody rw, or skip."""
        directory = Path(parent) / "shared"
        directory.mkdir()
        granted = subprocess.run([shutil.which("setfacl"), "-d", "-m", "u::rwx,g::rwx,o::---,u:nobody:rw", str(directory)],
                                 capture_output=True, text=True, timeout=30)
        if granted.returncode != 0:
            self.skipTest(f"setfacl -d failed here: {granted.stderr.strip()}")
        return directory

    @unittest.skipUnless(hasattr(os, "getxattr") and shutil.which("setfacl"), "needs setfacl and extended attributes")
    def test_new_output_in_a_default_acl_directory_gets_the_directory_policy(self):
        # The kernel ignores the umask where a default ACL applies; a new
        # document must get what any other new file there gets.
        for umask in (0o022, 0o077):
            with self.subTest(umask=oct(umask)), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                shared = self.directory_with_a_default_acl(tmp)
                reference = shared / "reference.toml"
                previous = os.umask(umask)
                try:
                    os.close(os.open(reference, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666))
                finally:
                    os.umask(previous)
                document = shared / "run.toml"

                result = self.simulate(tmp, document, preexec_fn=lambda: os.umask(umask))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
                self.assertEqual(stat.S_IMODE(document.stat().st_mode), stat.S_IMODE(reference.stat().st_mode))
                self.assertIsNotNone(self.access_acl(reference))
                self.assertEqual(self.access_acl(document), self.access_acl(reference))
                self.assertEqual(sorted(path.name for path in shared.iterdir()), ["reference.toml", "run.toml"])

    @unittest.skipUnless(hasattr(os, "getxattr") and shutil.which("setfacl"), "needs setfacl and extended attributes")
    def test_document_without_an_acl_in_a_default_acl_directory_gains_none(self):
        # A new file there would carry the directory's named entries; the
        # existing document, written in place, keeps having none.
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            shared = self.directory_with_a_default_acl(tmp)
            document = shared / "run.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)
            stripped = subprocess.run([shutil.which("setfacl"), "-b", str(document)], capture_output=True, text=True, timeout=30)
            self.assertEqual(stripped.returncode, 0, stripped.stderr)
            document.chmod(0o640)
            self.assertIsNone(self.access_acl(document))
            before = document.stat()

            result = self.simulate(tmp, document, instructions=1000)
            self.assertEqual(result.returncode, 0, result.stderr)
            after = document.stat()
            self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
            self.assertIsNone(self.access_acl(document))
            self.assertEqual((after.st_ino, stat.S_IMODE(after.st_mode)), (before.st_ino, 0o640))

    # Long enough that the run is still going well after its output checks.
    LONG_RUN = 200_000

    def start_simulation(self, directory, output, instructions):
        trace = Path(directory) / "trace.champsim2"
        arguments = ["--trace-version", "2", "-w", "0", "-i", str(instructions), "--hide-heartbeat", "--toml", str(output), "--"]
        return subprocess.Popen([str(BINARY), *arguments, *([str(trace)] * self.cores)], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)

    def wait_until_simulating(self, process, directory, timeout=60):
        """Return once `process` holds its trace open, which it does only after every output check has passed."""
        trace = os.path.realpath(Path(directory) / "trace.champsim2")
        descriptors = Path(f"/proc/{process.pid}/fd")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and process.poll() is None:
            try:
                if any(os.readlink(entry) == trace for entry in descriptors.iterdir()):
                    return
            except OSError:
                pass
            time.sleep(0.02)
        self.fail(f"the simulator never opened its trace (exit status {process.poll()})")

    def finish(self, process):
        try:
            stdout, stderr = process.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
        return subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "needs /proc to see when the run is under way")
    def test_output_is_untouched_while_the_run_is_in_progress(self):
        for existing in (True, False):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                document = Path(tmp) / "run.toml"
                if existing:
                    first = self.simulate(tmp, document, instructions=500)
                    self.assertEqual(first.returncode, 0, first.stderr)
                before = document.read_bytes() if existing else None
                inode = document.stat().st_ino if existing else None

                process = self.start_simulation(tmp, document, self.LONG_RUN)
                try:
                    self.wait_until_simulating(process, tmp)
                    time.sleep(0.5)
                    self.assertIsNone(process.poll(), "the run ended before it could be checked mid-run")
                    self.assertEqual(document.read_bytes() if document.exists() else None, before)
                    self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"] if existing else ["trace.champsim2"])
                finally:
                    result = self.finish(process)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], self.LONG_RUN)
                if existing:
                    self.assertEqual(document.stat().st_ino, inode)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "needs /proc to see when the run is under way")
    def test_statistics_document_removed_during_the_run_is_created_again(self):
        for links in ("one name", "a second hard link"):
            with self.subTest(links=links), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                document = Path(tmp) / "run.toml"
                other = Path(tmp) / "latest.toml"
                first = self.simulate(tmp, document, instructions=500)
                self.assertEqual(first.returncode, 0, first.stderr)
                if links == "a second hard link":
                    other.hardlink_to(document)
                earlier = document.read_bytes()

                process = self.start_simulation(tmp, document, self.LONG_RUN)
                try:
                    self.wait_until_simulating(process, tmp)
                    time.sleep(0.5)
                    document.unlink()
                    self.assertIsNone(process.poll(), "the run ended before the document was removed")
                finally:
                    result = self.finish(process)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], self.LONG_RUN)
                if links == "a second hard link":
                    # The other name still holds the file that was removed from this one.
                    self.assertEqual(other.read_bytes(), earlier)

    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "needs /proc to see when the run is under way")
    def test_document_replaced_during_the_run_is_refused_without_the_startup_hint(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.trace(tmp)
            document = Path(tmp) / "run.toml"
            first = self.simulate(tmp, document, instructions=500)
            self.assertEqual(first.returncode, 0, first.stderr)

            process = self.start_simulation(tmp, document, self.LONG_RUN)
            try:
                self.wait_until_simulating(process, tmp)
                document.write_bytes(b"precious notes written during the run\n")
                self.assertIsNone(process.poll(), "the run ended before the document was replaced")
            finally:
                result = self.finish(process)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(document.read_bytes(), b"precious notes written during the run\n")
            self.assertIn("not a ChampSim statistics document", result.stderr)
            self.assertIn("The run completed, but the TOML statistics were not written.", result.stderr)
            # Advice about a trace swallowed by --toml belongs to the startup check only.
            self.assertNotIn("--toml=FILE", result.stderr)

    def test_concurrent_runs_naming_one_new_output_are_not_refused_at_startup(self):
        # Every run proves at startup that the new name can be created. Runs
        # launched together must not mistake one another's proof for a refusal.
        runs, rounds = 6, 8
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.trace(tmp)
            document = Path(tmp) / "shared.toml"
            arguments = ["--trace-version", "2", "-w", "0", "--hide-heartbeat", "--toml", str(document), "--", *([str(trace)] * self.cores)]
            refused = []
            for round_index in range(rounds):
                if document.exists():
                    document.unlink()
                start = threading.Barrier(runs)

                def launch(index, results):
                    start.wait()
                    results[index] = subprocess.run([str(BINARY), "-i", str(1000 * (index + 1)), *arguments], cwd=tmp, capture_output=True, text=True,
                                                    timeout=120)

                results = [None] * runs
                threads = [threading.Thread(target=launch, args=(index, results)) for index in range(runs)]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()
                refused += [result.stderr for result in results if result.returncode != 0]
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["warmup_instructions"], 0)
                self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["shared.toml", "trace.champsim2"], f"round {round_index}")
            self.assertEqual(refused, [], f"{len(refused)} of {runs * rounds} concurrent runs failed")

    def strace_or_skip(self):
        strace = shutil.which("strace")
        if strace is None:
            self.skipTest("strace is not installed")
        probe = subprocess.run([strace, "-f", "-o", os.devnull, "-e", "trace=openat", "true"], capture_output=True, timeout=30)
        if probe.returncode != 0:
            self.skipTest(f"strace cannot trace here: {probe.stderr.decode(errors='replace')}")
        return strace

    def traced_simulation(self, strace, directory, output, log, instructions=1000):
        """simulate() under strace, tracing opens and unlinks into `log`."""
        arguments = ["--trace-version", "2", "-w", "0", "-i", str(instructions), "--hide-heartbeat", "--toml", str(output), "--"]
        command = [strace, "-f", "-o", str(log), "-e", "trace=open,openat,unlink,unlinkat", str(BINARY), *arguments]
        command += [str(Path(directory) / "trace.champsim2")] * self.cores
        return subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=120)

    FILE_CALL = re.compile(r'^\d+ +(open|openat|unlink|unlinkat)\((?:[^,"]+, )?"([^"]*)"(?:, ([A-Z0-9_|]+))?(?:, (0[0-7]*))?\) += (-?\d+)')

    def traced_calls(self, log, name):
        """(call, flags, mode or None, result) for every open and unlink of a file called `name` in an strace log."""
        calls = []
        for line in Path(log).read_text().splitlines():
            if (match := self.FILE_CALL.match(line)) and Path(match.group(2)).name == name:
                call, _, flags, mode, returned = match.groups()
                calls.append((call.removesuffix("at"), set((flags or "").split("|")) - {""}, mode and int(mode, 8), int(returned)))
        return calls

    def test_output_is_truncated_or_created_only_by_its_final_open(self):
        # At startup an existing document is opened without O_TRUNC, and a new
        # name is created exclusively and removed again. After the run an
        # existing document is opened without O_CREAT, which
        # fs.protected_regular refuses for another user's file in a sticky
        # directory even when it may be written; a new name is created
        # exclusively, with mode 0666 for the umask or a default ACL to narrow.
        strace = self.strace_or_skip()
        for existing in (True, False):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as logs:
                self.trace(tmp)
                document = Path(tmp) / "run.toml"
                if existing:
                    first = self.simulate(tmp, document, instructions=500)
                    self.assertEqual(first.returncode, 0, first.stderr)
                log = Path(logs) / "strace.log"

                result = self.traced_simulation(strace, tmp, document, log)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(tomllib.loads(document.read_text())["meta"]["simulation_instructions"], 1000)
                self.assertEqual(sorted(path.name for path in Path(tmp).iterdir()), ["run.toml", "trace.champsim2"])
                calls = self.traced_calls(log, "run.toml")
                context = log.read_text()[-3000:]
                *startup, final = calls
                self.assertEqual(final[0], "open", context)
                self.assertGreaterEqual(final[3], 0, context)
                if existing:
                    self.assertTrue(all(call == "open" and not flags & {"O_TRUNC", "O_CREAT"} for call, flags, _, _ in startup), context)
                    self.assertIn("O_TRUNC", final[1], context)
                    self.assertNotIn("O_CREAT", final[1], context)
                else:
                    created = [index for index, (call, flags, _, _) in enumerate(startup) if "O_CREAT" in flags]
                    self.assertEqual(len(created), 1, context)
                    self.assertTrue({"O_CREAT", "O_EXCL"} <= startup[created[0]][1], context)
                    self.assertEqual(startup[created[0]][2], 0o666, context)
                    self.assertIn("unlink", [call for call, _, _, _ in startup[created[0] + 1:]], context)
                    self.assertTrue({"O_CREAT", "O_EXCL"} <= final[1], context)
                    self.assertNotIn("O_TRUNC", final[1], context)
                    self.assertEqual(final[2], 0o666, context)

    @unittest.skipUnless(resource is not None and hasattr(signal, "SIGXFSZ"), "needs RLIMIT_FSIZE to make the final write fail")
    def test_failed_final_write_exits_1_and_says_the_document_may_be_partial(self):
        # Written in place, a document is truncated before the new one is
        # written, so a full disk at that moment loses the earlier one.
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
            self.assertIn(f"ERROR: failed to write the TOML statistics to '{document}' (File too large); it may now be empty or partial.", result.stderr)
            self.assertLessEqual(len(document.read_bytes()), limit)
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

    def test_unopenable_device_or_socket_fails_before_the_run(self):
        # Only a FIFO skips the startup open, which would consume its reader.
        cases = {}
        if os.path.exists("/dev/tty"):
            # A new session has no controlling terminal, so /dev/tty cannot be opened.
            cases["/dev/tty without a controlling terminal"] = (lambda tmp: "/dev/tty", {"start_new_session": True})
        if hasattr(socket, "AF_UNIX"):
            cases["a Unix socket"] = (lambda tmp: self.bound_socket(tmp), {})
        for case, (output, options) in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                self.trace(tmp)
                result = self.simulate(tmp, output(tmp), stdin=subprocess.DEVNULL, **options)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("cannot open", result.stderr)
                self.assertNotIn("ChampSim completed", result.stdout)

    def bound_socket(self, directory):
        path = Path(directory) / "stats.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(str(path))
        return path

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
