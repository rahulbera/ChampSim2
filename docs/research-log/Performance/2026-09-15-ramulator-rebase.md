# Rebase of the performance branch onto updated Ramulator integration — 2026-09-15

## 1. Integrate the advanced `feat/ramulator` branch

### Issue and scope

While performance work continued in `champsim-perf-fix`, the sibling
`champsim-ramulator` checkout advanced its `feat/ramulator` branch by 56 commits.
The user requested a rebase to include those changes. This is an integration
checkpoint, not another optimization or a new KIPS baseline.

- Work checkout: `/home/rbera/work/alakazam/champsim-perf-fix`, `feat/perf-fix`.
- Old base: `74159f1e5dcb7cea1d27869c2ad8c51d19d0a5a4`.
- Old performance tip: `2876fc9bb007a7ce548be392ed7d6c61fa291746`.
- New base: `ff4bf5d393f2ff59747fa5473528227d5418ea61`, fetched from the local
  `/home/rbera/work/alakazam/champsim-ramulator` checkout.
- Replayed tip before follow-up corrections:
  `aee99d70ac766de9eae7aa6259e4633779a832fe`.

### High-level integration

All 61 performance commits were replayed on the new base. The new upstream
output-file handling stays inside the performance branch's exception handler;
the upstream native watchdog warning and unrenamed-register diagnostic remain
alongside the fixed-PTW and register-query checks.

The main build conflict combined upstream `RAMULATOR2_SANITIZE=1` support with
the performance branch's named modes and isolated build paths. Sanitizers reach
host compilation/linking and the native library, and coexist with the selected
architecture flags. Native ownership remains under the source-root lock and
manifest: an exact matching library can be shared between host modes/flavors;
a different native policy requires a fresh source root. Sanitized build metadata
now reports `RelWithDebInfo C++20 Python=OFF Sanitizers=address,undefined`.

Default production policy remains release, `-O3 -g3`, ChampSim assertions
**enabled**, and **x86-64-v2**. Fast mode disables only ChampSim assertions.
The native sanitizer switch does not change those named host-mode policies.

### Files touched at the integration intersections

The complete replay is recorded in the
[61-commit mapping](2026-09-15-ramulator-rebase-commits.tsv). This list describes
conflict resolutions and follow-up corrections, not every file changed by the
original 61 performance commits or the 56 incoming upstream commits.

1. `src/main.cc`: retain upstream output planning/delivery and native watchdog
   diagnostics within the broader exception handling and fixed-PTW allowance.
2. `test/cpp/src/201-register-rename.cc`: retain both unrenamed-register safety
   coverage and performance-branch public-query/construction coverage.
3. `Makefile`: retain isolated named modes and add the upstream sanitizer switch.
4. `config/build_rules.mk`: inject host sanitizer flags once in the inner Make,
   forward the switch to native preparation, and include it in policy selection.
5. `config/ramulator2_build.py`: combine native sanitizer CMake/ABI settings with
   architecture flags, provenance checks, root ownership and immutable reuse.
6. `config/build_config.py`: represent sanitizer mode in policy identity and
   build-info metadata; reject invalid sanitizer/native combinations.
7. `test/python/test_ramulator2_build.py`: adapt dry-run fixtures to isolated
   build paths and response files; check every host compile/link, actual policy
   selection, native replacement refusal, and combined architecture/sanitizers.
8. `test/python/test_build_modes.py`: isolate nested test fixtures from inherited
   `RAMULATOR2_SANITIZE` settings.
9. `test/cpp/src/252-rob-execution-prefix.cc`: execute the existing checks inside
   each Catch2 mutation section so strict `NoAssertions` validation works.
10. `test/cpp/src/703-dram-address-mapping.cc`: filter the already-excluded
    zero-width oracle geometry in the generator instead of returning silently.
11. `test/ramulator2/run_mutants.py`: reuse verified native-root ownership,
    deprecate the ineffective seed-manifest option with a diagnostic, and isolate
    scratch builds from inherited Make and build-root overrides.
12. `test/ramulator2/test_tools.py`: cover scratch-build environment isolation.
13. `test/ramulator2/README.md`: describe native reuse, fresh roots for policy
    changes, isolated host paths and accurate sanitizer flags.
14. `CLAUDE.md`: reconcile native sanitizer documentation with the named modes
    and separate simulator/test objects.
15. `.github/workflows/test.yml`: collect sanitizer logs from the actual owned
    paths (including the named hidden directories) and correct the non-PIE reuse
    comment; retain its valid build command.
16. `docs/superpowers/plans/2026-09-14-fixed-ptw-performance.md`: keep the
    performance plan at its original path instead of following Git's inferred
    move into the upstream Ramulator-specific documentation directory.
17. This log and its companion commit mapping: record the integration and checks
    while leaving historical measurements and their original hashes intact.

### Commit record and recovery

The replayed series is `ff4bf5d3..aee99d70`; follow-up integration commit:
`2c1b23c3773846cbf6f4bfc3e343836f382dcdcb`. The companion TSV maps every old performance
commit to its rewritten hash, preserving subjects and order.

A local backup branch,
`backup/feat-perf-fix-before-ramulator-rebase-2026-09-15`, retains the old tip.
A verified `before-rebase.bundle` provides a second history backup. The source
checkout was left on `feat/ramulator` at `ff4bf5d3`. No remote push or mainline
merge is part of this task.

### Regression verdict

The legacy simulator has **exact observed parity** across all 16 short cases
and all 15 long cases against the accepted pre-rebase release. Each long run
uses **5M warmup / 50M simulation**, covering all 14 available SPEC26 workloads
and the v1 mcf control. Both detailed and fixed-PTW paths are represented in the
short matrix, together with page seeds, clocks, prefetching and DRAM geometry.
All these comparisons use `dram-model=legacy`.

Independent audit scripts verified complete reported phase statistics,
effective configuration, config IDs, retirement/cycle counts and archived
binary/config/trace identities. Build provenance and host timings are handled
separately from modeled results. Parity is evidence for these inputs, not a
proof that every possible upstream behavior change is neutral.

| Check | Result |
| --- | --- |
| Release C++ (`--warn NoAssertions`) | 933 passed, 20 skipped; 117,891 assertions passed |
| Fast C++ (`--warn NoAssertions`) | 933 passed, 20 skipped; 117,891 assertions passed |
| Payload-layout C++ | 938 passed, 20 skipped; 119,984 assertions passed |
| Native-enabled C++ | 952 passed, 1 skipped; 132,855 assertions passed |
| Full Python suite against legacy release | 150 tests, 9 skips, no failures |
| Native CLI suite | 11 tests, no failures |
| Performance-tool tests | 8 tests, no failures |
| Native-tool tests with the pinned exporter | 29 tests, no failures |
| Short matrix | 16 cases / 32 fresh runs, exact parity |
| Long matrix | 15 fresh runs vs 15 verified archived references, exact parity |
| DDR4 and LPDDR5 transaction oracles | Both passed, 22 accepted transactions and callbacks each |
| Native simulator integration | DDR4, LPDDR5, multichannel and frequency cases passed |
| Native non-PIE build/provenance | Build passed, ELF `EXEC`, native configuration accepted |
| Two-core mutant-tool smoke | Baseline passed; M01 detected; equivalent M12 survived; working tree unchanged |
| Native sanitizer policy transition | Different policy refused; existing library and manifest hashes unchanged |
| ASan/UBSan native C++ | 45 cases / 50,742 assertions passed; RIT cases separately: 2 / 16 passed |
| ASan/UBSan generated-trace simulation | Passed with native reads and writes; sanitized build identity verified |

Native-dependent cases are skipped in the disabled builds. The Python skips
include unavailable Clang and native-only CLI cases; the separate native CLI
run exercises the latter. Initial strict C++ runs exposed six `NoAssertions`
failures in the two test fixtures above. Their checks and covered geometries
were preserved; simulator statistics were not changed to make tests pass.
Initial focused Python fixture failures and the sanitizer-metadata red/green
check are retained in the evidence directory.

Sanitizer runs used the upstream, narrowly scoped ITTAGE suppression files.
The two RITAddrMapper cases additionally used the existing native-leak
suppression (20 allocations / 2,028 bytes). These are scoped checks, not a claim
that the pinned native model or external dependencies are free of all leaks.
The actual Make sanitizer-policy transition was also attempted against the
occupied release native root: it refused the change and left the library and
manifest SHA-256 hashes unchanged.

Independent source/range-diff review found no core conflict-resolution
regression. Its two tooling/test-coverage follow-ups were corrected and reviewed
again with no remaining actionable finding.

### KIPS before and after

**Not measured for this rebase.** Correctness runs overlapped builds and other
validation. Their elapsed times must not be presented as performance evidence.
Previously recorded KIPS values still describe their original frozen binaries
and revisions; this log does not relabel or overwrite them.

### Evidence and remaining limits

Evidence root:
`/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-ramulator-rebase`.
It contains command/result receipts, build/test logs, frozen binaries and their
identities, `before.json`, the backup bundle, `range-diff.txt`, short and long
independent audits, native oracle/integration summaries, and `rebase-review.md`.

The frozen legacy release used for parity has SHA-256
`3c3ddbc4ea229cbe5c402c533eda8b10184102bf349d7f27ec803e2d34413f3f`;
the verified predecessor has SHA-256
`ebd0abafe640c83144c09b5579dce51846f46a9a9e41644dc474aaa3a33af1d8`.
After validation, the local `bin/champsim` alias was atomically updated to that
new frozen release. `publication.json` records the old/new hashes and target.
`final-binary-audit.json` verifies the four frozen build variants against their
recorded identities and the final build-helper source contents.

Validation is local Linux/x86-64 with GCC 13 and the existing pinned dependency
installation. Hosted CI, a clean-host dependency reproduction, ARM, remote
cluster execution and the full native mutant campaign were not rerun. Native
checks are compatibility/correctness evidence, separate from legacy-only
performance experiments. No new speedup is claimed.
