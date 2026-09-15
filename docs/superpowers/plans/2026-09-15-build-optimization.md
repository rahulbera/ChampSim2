# Build optimization campaign implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver explicit, isolated simulator build modes with an x86-64-v2
default and accept performance changes only after the established parity and
KIPS gates.

**Architecture:** Keep ISA selection independent of optimization/assertion mode.
First measure the ISA change with the existing build, then migrate assertions
and introduce build policy. Preserve native dependency provenance and the
simulated configuration's identity separately from compiler provenance.

**Tech stack:** GNU Make, Python, C++17, Catch2, GCC/Clang, optional private
C++20 Ramulator driver, Linux perf and Slurm for later machine validation.

**Spec:** `docs/superpowers/specs/2026-09-15-portable-build-modes-design.md`.

## Global constraints

- Work in `/home/rbera/work/alakazam/champsim-perf-fix`, `feat/perf-fix`.
- User authorized autonomous implementation and experiments; no push or merge.
- Every performance simulation uses `dram-model=legacy`; performance builds use
  `WITH_RAMULATOR2=0`. Native build compatibility is a separate check.
- Default x86 policy: `-march=x86-64-v2 -mtune=generic`; explicit
  `X86_ISA=x86-64` fallback. AArch64 baseline: `-march=armv8-a -mtune=generic`.
- Debug: `-O0 -g3`, preserve frame pointers, assertions enabled. Release:
  `-O3 -g3`, assertions enabled. Fast: `-O3 -g3`, ChampSim assertions disabled.
- Plain `make` remains release and preserves `bin/champsim`.
- Exact deterministic reported-statistics parity is mandatory. Never waive a
  mismatching workload or weaken the comparison to accept a change.
- Each accepted performance candidate must pass 14 SPEC26 workloads plus mcf at
  5M warmup/50M simulation, the existing short matrix, and relevant unit checks.
- Three alternating timing pairs per protected trace at 1M/3M; same CPU within
  each pair; no own build, profile, or other experiment overlaps reported KIPS.
- Log each issue, fix, enumerated touched files, commits, regression verdict,
  before/after KIPS, and acceptance decision in
  `docs/research-log/Performance/2026-09-15-build-optimization.md`.
- Preserve raw evidence outside Git under
  `/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-build-optimization/`.
- The later v3/native experiments are authorized, but must pass separately and
  must not silently raise the default beyond the approved v2 fleet contract.

## Task 1: Establish the campaign and evaluate v2 in isolation

**Files:** new campaign log; this plan; updated spec; evidence-only copies of
the current simulator, manifests, compiler options, libraries, and commands.
No production source change in the trial.

**Interfaces:** consumes the existing `tools/perf/compare_optimization.py` CLI,
the archived `run_long_after.py` and `audit_long.py`, and their validated manifests.
Produces preserved `00-baseline/champsim`, `01-v2/champsim`, build metadata,
regression/timing artifacts, and an explicit retain/reject verdict.

- [ ] Preserve the current binary only after checking SHA256 equals
  `fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79`.
  Save `git rev-parse HEAD`, compiler version, response files, `ldd`, CPU/OS
  details, and current clean-tree status. Copy the exact trace manifests into
  the new evidence directory and verify their hashes through the harness.
- [ ] Check the archived cache-guard long reference's binary hash against the
  preserved binary. If it differs, establish a fresh 5M/50M reference using the
  preserved baseline rather than assuming source equivalence is sufficient.
- [ ] Build an isolated v2 candidate through the current interface:

  ```bash
  env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS \
    make CXX=/usr/bin/g++ WITH_RAMULATOR2=0 -j4 \
    CPPFLAGS=-I.csconfig CXXFLAGS='-march=x86-64-v2 -mtune=generic' \
    OBJ_ROOT=.csconfig-v2-trial DEP_ROOT=.csconfig-v2-trial \
    BIN_ROOT=/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-build-optimization/01-v2
  ```

  Capture the exact command and all build output. Verify compile commands retain
  `-O3` and assertions and add no unrelated options. Check the archived baseline
  and production `bin/champsim` did not change.
- [ ] Run the existing performance-tool tests before using their verdicts:
  `python3 -m unittest discover -s tools/perf -v`.
- [ ] Run short parity with `compare_optimization.py --regression --warmup
  100000 --instructions 500000 --repetitions 1`, supplying the preserved before
  and after binaries, protected trace manifest, both existing configuration
  files, CPU 14, and a new output directory.
- [ ] Run the 15-case long successor comparison and its independent artifact
  audit. Confirm every expected trace appears exactly once and that complete
  phase/configuration fingerprints and retirement/cycle counts match.
- [ ] Build/run the existing normal and memory-value test suites with the trial
  ISA flags and isolated roots; run Python tests. Test builds currently append
  `-Og`, so do not present their timings as optimized simulator evidence.
- [ ] Once all other own computation has stopped, run the protected paired KIPS
  comparison with defaults 1M/3M, three repetitions, CPU 14. Independently parse
  all 24 runs and compare fingerprints; record medians, ranges, and each pair.
- [ ] Review the artifacts and record the decision before the next production
  optimization. A small or absent ISA speed gain does not invalidate the user's
  chosen support floor; distinguish an accepted platform policy from a measured
  performance improvement. Do not retain a behavior-changing ISA result.

## Task 2: Audit assertions and implement an explicit project policy

**Files:** create `inc/champsim_assert.h` and
`test/python/test_champsim_assert.py`; modify audited assertion sites in
`src/` and ChampSim-owned `inc/` headers; add focused decompression/error tests
under the existing `test/cpp/src/` and `test/python/` conventions where needed.
Record the exhaustive assertion classification in the task report.

**Interfaces:** `CHAMPSIM_ENABLE_ASSERTIONS` defaults to 1 for standalone users.
`CHAMPSIM_ASSERT(expression)` uses that flag, not `NDEBUG`. Enabled assertions
evaluate once, print expression/file/line, and abort. Disabled assertions perform
no evaluation. The header adds no link or vcpkg dependency.

- [ ] Classify every owned `assert` as an internal invariant or operational
  validation. Read callers before changing decoder/error handling. Report any
  side effects, constant-evaluation use, vendored headers, and runtime diagnostic
  checks. Keep this audit read-only until Task 1's experiment is resolved.
- [ ] Add subprocess-based compile/run tests before the helper exists. A real
  C++17 probe must cover a valid constexpr use, one evaluation in enabled mode,
  zero evaluations when disabled, failure diagnostics, and enabled behavior with
  `-DNDEBUG`. The production break caught is a flag or macro implementation that
  removes a required check, evaluates twice, evaluates while disabled, or breaks
  standalone constant evaluation. The initial compile failure must name the
  missing project header, not an unavailable compiler or dependency.

  ```cpp
  #include "champsim_assert.h"
  constexpr int checked(int x) { CHAMPSIM_ASSERT(x > 0); return x; }
  static_assert(checked(3) == 3);
  int main() {
    int calls = 0;
    CHAMPSIM_ASSERT(++calls == 1);
    return calls;
  }
  ```

  Expect exit 1 with checks enabled and exit 0 disabled. Use a separate failing
  condition probe to require abnormal termination and expression/file diagnostics;
  suppress core-dump artifacts in the test subprocess. This test-only counter is
  deliberate; production asserted expressions must not own required side effects.
- [ ] Implement a dependency-free header, with a cold failure path, preserving
  valid C++17 constexpr evaluation. Keep `static_assert` unchanged. Do not rewrite
  third-party or vendored code mechanically.
- [ ] Add a real corrupt/truncated-trace reproducer for each changed decoder
  error path, then implement always-active errors that reach the CLI as a useful
  nonzero failure. Preserve valid concatenated frames and normal EOF. Read the
  stream exception/caller behavior rather than assuming an exception escapes.
- [ ] Migrate only the audited owned sites. Run the new probes, affected C++
  tests, standalone-tool builds, and normal/payload suites. Capture warnings and
  resolve assertion-only unused variables without adding runtime work.
- [ ] Compare enabled-policy release to its predecessor using the complete short
  and long gates and fresh timing; keep the compiler ISA/optimization unchanged.
  Review and log the migration before enabling fast mode.

## Task 3: Implement isolated named builds and compiler provenance

**Files:** modify `Makefile`, `global.options`, `config/module_registry.py` and
`config/ramulator2_build.py`; create a focused `config/build_config.py` helper and
`test/python/test_build_modes.py`; update `config/compile_commands/` consumers,
`src/main.cc`, public metadata declarations/writer as needed, `CLAUDE.md`, and
`tools/perf/README.md`. Keep exact touched-file lists in the report/log.

**Interfaces:** Make targets `debug`, `release`, `fast`; `BUILD_MODE` selects the
same policy for ordinary targets and tests; `X86_ISA` selects v1/v2 independently.
Generated build information records compiler target, version, effective options,
assertion mode, ISA, and dependency identity. `--build-info` provides machine-
readable provenance without constructing a simulated machine. Existing runtime
configuration `build_id` semantics stay unchanged.

- [ ] Write real Make integration tests before production changes. Use a small
  temporary fixture with the real policy/Make path and an actual host compiler
  for enabled/disabled assertion and optimization macro probes; a controlled
  compiler-target query can cover unavailable foreign toolchains. Tests must fail
  for missing mode targets or wrong effective flags, not compare source strings.

  ```cpp
  #include "champsim_assert.h"
  #include <cstdio>
  int main() {
  #ifdef __OPTIMIZE__
    std::puts("optimized");
  #else
    std::puts("unoptimized");
  #endif
    std::printf("assertions=%d\n", CHAMPSIM_ENABLE_ASSERTIONS);
  }
  ```

  Debug must report unoptimized/assertions=1; release optimized/assertions=1;
  fast optimized/assertions=0. Verify v1/v2 feature macros independently. Retain
  distinct executables and repeat a mode switch without cleaning; changing flags
  must rebuild affected outputs while an unchanged repeat stays up to date.
- [ ] Implement policy selection before object and generated-path resolution.
  Query the selected compiler target; choose the target's dependency triplet
  explicitly. Reject empty/unknown modes, unsupported targets, x86 options on
  Arm, conflicting optimization/assertion/ISA overrides, and ambiguous dependency
  selection. Preserve supported wrappers and quote arguments safely.
- [ ] Separate all simulator/test objects, dependencies, generated build headers,
  and binaries by incompatible policy. Module registry inputs may be shared when
  immutable; generated Make fragments must respect output overrides. Keep
  explicit `OBJ_ROOT`, `DEP_ROOT`, `BIN_ROOT`, and test-binary overrides usable
  without accidental production/test sharing. Test combined target invocations
  and separate concurrent mode builds. Preserve side-effect-free `make -n/-q/-t`.
- [ ] Remove unconditional `-O3` from the common options and the test target's
  unconditional `-Og`; supply optimization from the selected mode. Keep test
  framework checks active. Include mode options in dependency preprocessing as
  well as compilation. Ensure compiler/link option fingerprints invalidate stale
  artifacts, and compile-command export represents the selected mode.
- [ ] Add early `--build-info` handling, with JSON produced without runtime model
  construction. Test that malformed simulation configuration cannot corrupt or
  substitute compiler provenance. Expose the same identity to benchmark manifests
  without changing simulated configuration hashes or the statistics comparator.
- [ ] Propagate/fingerprint the architecture policy to the native helper and
  preserve explicit native Release scope, ABI checks, source/dependency revision
  checks, and shared-library replacement safeguards. Exercise disabled-mode tests
  and the existing native smoke tests on the verified toolchain using a separate
  native root. Do not use native simulations as campaign KIPS.
- [ ] Run Python and C++ checks, separate normal/payload modes, standalone tools,
  GCC/Clang builds where available, and the complete release parity gate. Preserve
  a v1 named release to separate symbol/build-layout effects from ISA changes.
  Review and log build hygiene with its own performance result.

## Task 4: Validate and measure assertion removal in v2 fast

**Files:** task report and campaign log; fixes only if the tests identify a real
problem in Task 2/3, followed by their covering checks and review.

**Interfaces:** consumes archived named v2 release and fast binaries and the
unchanged comparison harness. Produces a fast-mode acceptance verdict.

- [ ] Verify actual compile commands and `--build-info` differ only in intended
  assertion policy; both remain `-O3 -g3`, same v2 ISA/compiler/libraries.
- [ ] Execute normal and memory-value tests in fast, including decoder failures
  and invalid runtime input. Check the new assertion probe removes evaluation
  while Catch2's own checks still run.
- [ ] Run all short cases and 15 long 5M/50M comparisons against named v2 release.
  Audit complete outputs before allowing the timing campaign.
- [ ] Run the 24 alternating timing runs, inspect each workload/pair, and review
  the result. Record inertness, before/after KIPS, and limitations. If a correctness
  issue remains, do not advertise fast as validated or hide it with new expected
  statistics. Fix the root cause before considering retention.

## Task 5: Evaluate v3 and resolved ETH headnode native profiles

**Files:** evidence scripts/manifests and campaign log; build-policy/test changes
only when adding an explicit accepted profile, with focused failing tests first.

**Interfaces:** a profile is an explicit named set of compiler/ISA/tuning options,
kept separate from the v2 default, with a declared destination fleet.

- [ ] Record the ETH headnode's CPU model, GCC version, and effective native
  target options using `c++ -march=native -Q --help=target`. Compare requirements
  against the destination-node audit. A successful historical native build is
  supporting context, not an immutable feature specification.
- [ ] Build v3 with unchanged mode, compiler, tuning, assertions, and libraries;
  run the full short/long/performance gate against the accepted v2 predecessor.
  If testing a new host, rebuild both variants on that cluster and compare there.
- [ ] Resolve the native candidate to explicit options and record them. Test it
  separately against the accepted profile with the same full gate. If required
  CPU/OS features are absent on a fleet node, reject fleet-wide use or retain a
  visibly restricted experiment without raising the portable default.
- [ ] Document each acceptance or rejection independently. Add no LTO, PGO, or
  fast-math to these comparisons. Do not adopt a specialized profile merely
  because the compiler accepts it or the executable starts.

## Task 6: Closeout review and documentation

**Files:** campaign log, build documentation, evidence verification report.

- [ ] Reconcile every retained implementation with its exact binary, source,
  tests, parity audit, and measured predecessor. Verify rejected trials are absent
  from the retained production paths and standard defaults match the spec.
- [ ] Obtain whole-campaign code/evidence review, resolve material findings with
  focused tests and a scoped re-review, and preserve the reports.
- [ ] Record any untested platform or dependency scope accurately. Provide final
  incremental results and, if measured, a separately labelled cumulative result.
  Keep the local branch and evidence for the user; do not push or merge.
