"""Standalone contract checks for the ChampSim assertion policy."""

import resource
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ChampSimAssertTests(unittest.TestCase):
    def setUp(self):
        self.compiler = shutil.which("g++") or shutil.which("clang++")
        if not self.compiler:
            self.skipTest("a C++17 compiler is required for assertion checks")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def compile(self, source, *definitions):
        source_path = self.directory / "assert_probe.cc"
        executable = self.directory / "assert_probe"
        source_path.write_text(source)
        result = subprocess.run(
            [self.compiler, "-std=c++17", "-I", str(ROOT / "inc"), *definitions, str(source_path), "-o", str(executable)],
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return executable

    @staticmethod
    def run_probe(executable):
        return subprocess.run(
            [str(executable)],
            text=True,
            capture_output=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
        )

    def test_enabled_assertions_preserve_constexpr_and_evaluate_once(self):
        source = """\
#include "champsim_assert.h"
constexpr int checked(int x) { CHAMPSIM_ASSERT(x > 0); return x; }
static_assert(checked(3) == 3);
int main() {
  int calls = 0;
  CHAMPSIM_ASSERT(++calls == 1);
  return calls;
}
"""
        for definitions in ((), ("-DCHAMPSIM_ENABLE_ASSERTIONS=1",), ("-DNDEBUG",)):
            with self.subTest(definitions=definitions):
                result = self.run_probe(self.compile(source, *definitions))
                self.assertEqual(result.returncode, 1, result.stderr)

    def test_disabled_assertions_do_not_evaluate_the_expression(self):
        source = """\
#include "champsim_assert.h"
constexpr int checked(int x) { CHAMPSIM_ASSERT(x > 0); return x; }
static_assert(checked(3) == 3);
int main() {
  int calls = 0;
  CHAMPSIM_ASSERT(++calls == 1);
  return calls;
}
"""
        result = self.run_probe(self.compile(source, "-DCHAMPSIM_ENABLE_ASSERTIONS=0"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failure_reports_expression_file_and_line_then_aborts(self):
        source = """\
#include "champsim_assert.h"
int main() {
  CHAMPSIM_ASSERT(2 + 2 == 5);
}
"""
        result = self.run_probe(self.compile(source))
        self.assertLess(result.returncode, 0, result.stderr)
        self.assertIn("2 + 2 == 5", result.stderr)
        self.assertIn("assert_probe.cc", result.stderr)
        self.assertIn(":3", result.stderr)


if __name__ == "__main__":
    unittest.main()
