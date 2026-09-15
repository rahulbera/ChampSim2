# Build optimization campaign — 2026-09-15

Work continues on `feat/perf-fix` in
`/home/rbera/work/alakazam/champsim-perf-fix`. This is a new campaign after the
[source optimization pass](2026-09-14-performance-optimization.md), not a restart
from the original unoptimized simulator. Its initial source revision is
`a612bf2b`; production sources are unchanged from the preceding closeout at
`4bc0f045`.

The [build-mode design](../../superpowers/specs/2026-09-15-portable-build-modes-design.md)
defines debug (`-O0 -g3`, assertions enabled), release (`-O3 -g3`, assertions
enabled), and fast (`-O3 -g3`, ChampSim assertions disabled). The approved x86
minimum is **x86-64-v2**, with an explicit original x86-64 fallback. AArch64 has
its own baseline. Builds are made separately on each cluster.

## Common acceptance gate

Implement, check, measure, review, and record one candidate before retaining it
and moving to the next optimization. An implementation commit is not itself an
acceptance verdict. A rejected candidate must not remain in the production
implementation; retain its evidence and explain the decision here.

Every performance experiment and before/after regression comparison explicitly
uses `dram-model=legacy`; performance builds use `WITH_RAMULATOR2=0`. Separate
Ramulator build-compatibility checks preserve the existing optional backend and
do not contribute KIPS evidence. Primary timing uses detailed
PTW and the existing GLC comparison configuration. Preserve the same traces,
configuration, compiler, libraries, CPU affinity, and counting rules within
each before/after comparison except for the variable being tested.

Acceptance requires all of the following:

1. Relevant unit, Python, build-system, and standalone-tool checks pass. For
   build/assertion changes, exercise normal and trace-memory-value layouts with
   separate objects and binaries. Test-framework assertions stay enabled.
2. The existing short regression matrix passes: four protected traces, detailed
   and fixed PTW, changed page seeds and clocks, prefetching, and legacy DRAM
   geometry. Compare complete reported phase statistics and effective
   configuration, together with warmup/ROI retirement and cycle counts.
3. The long gate passes at **5M warmup / 50M simulation**, one trace from each of
   the 14 SPEC26 workloads plus the existing v1 mcf control. Compare against the
   immediate accepted predecessor. An archived reference is usable only when
   its binary, source identity, inputs, configuration, and complete results have
   been verified. These concurrent correctness runs are excluded from KIPS.
4. Any mismatch is investigated. Do not update expected simulation statistics,
   weaken the comparator, or discard a failing workload to accept a candidate.
   Build provenance and wall-time fields are not simulated behavior; exclusions
   must be explicit and must not mask changes in modeled events or parameters.
5. Performance evidence consists of at least three alternating before/after
   pairs on sqlite, omnetpp, gcc, and mcf, using 1M warmup / 3M simulation and the
   same pinned CPU. No own builds, profilers, or other experiments overlap these
   timings. Record individual pairs, median and range, host contention, and
   trace/configuration/binary hashes. Repeat only when ambiguity justifies it.
6. Independent review checks the implementation and the evidence. “Inert” means
   identical observed results over the stated checks, not proof for every input.
   An optimization needs repeatable performance benefit without an unexplained
   workload regression. Build hygiene may be retained for its maintenance value,
   but cannot be described as a speed improvement without measurements.

KIPS is actual warmup plus simulation instructions retired, divided by 1,000
times whole-process wall seconds. Each stage reports its **incremental** result
against its immediate predecessor. Any cumulative comparison is separately
labelled and measured; do not add percentages or compare unrelated historical
wall times. Cross-machine raw KIPS does not isolate a build-option improvement.

ISA support must be checked on destination nodes as well as the build host.
All 19 current ETH `cpu_part` nodes passed the v2 audit and their loaders report
v3 support (Slurm job `14547781`). The user's successful experience with
headnode `-march=native` motivates a later experiment; record the exact compiler
and expanded native features before treating that as a reproducible profile.
Do not silently introduce an instruction requirement unsupported by a fleet
member. The user deferred ARM hardware validation on 2026-09-15; this campaign now focuses
on x64. Preserve the ARM design, but do not claim real ARM validation.

Raw campaign evidence is stored outside Git under:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-build-optimization/
```

Preserve prior evidence. Record source and binary identities, full compile/link
commands, libraries, CPU/OS details, test outputs, comparison outputs, and rejected
trials. New result directories must be new rather than overwritten.

## Stage order

1. Isolate the x86-64-v2 instruction-set change using the current release build
   and unchanged assertion/symbol policy; establish its behavior and KIPS result.
2. Introduce the assertion helper and preserve operational error checks in every
   mode; verify the enabled policy before removing any checks for performance.
3. Implement isolated named build modes, target selection, and provenance;
   establish release parity and measure the effect of build hygiene separately.
4. Compare v2 release with v2 fast after the assertion audit; apply the complete
   regression and performance gate before retaining the fast implementation.
5. Experiment with v3, then the ETH headnode's resolved native target, one at a
   time. Preserve v2 as the portable default unless a subsequent decision changes
   the supported fleet contract. CPU tuning, LTO, and PGO remain separate variables.

## 0. Preserve the campaign baseline and validation controls

**Issue.** The previous campaign already optimized the simulator substantially.
Compiler-policy gains must be attributable to a known immediate baseline.

**Fix.** Preserve the current release binary and its build provenance before any
trial; reuse the validated comparison machinery and trace selection, checking
their identities. Establish this log and the stage-by-stage acceptance rules.

**Files touched.**

1. `docs/research-log/Performance/2026-09-15-build-optimization.md`: establish the
   new campaign, required evidence, sequence, and per-stage reporting format.
2. `docs/superpowers/plans/2026-09-15-build-optimization.md`: record the execution
   steps and contracts for the campaign.
3. `docs/superpowers/specs/2026-09-15-portable-build-modes-design.md`: point this
   campaign at the new log and record authorization for later v3/native trials.

**Commit hashes.** Campaign source baseline: `a612bf2b`; preceding production
closeout: `4bc0f045`. Campaign setup: `e07f6ed8`.

**Regression verdict.** No simulator change in setup. Existing release binary
SHA256: `fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79`.
Fresh candidate checks are recorded in their own sections below.

**KIPS before/after.** Not applicable to documentation and baseline capture.
No new speedup is claimed.

## 1. Raise the x86 instruction-set floor to v2

**Issue.** The current release build targets the original x86-64 instruction set.
The supported ETH CPU fleet permits v2 instructions, which may improve generated
code while retaining compatibility across that fleet.

**Fix.** First compile an isolated candidate with
`-march=x86-64-v2 -mtune=generic` through the existing build interface. Keep `-O3`,
assertions, dependencies, and the current symbol policy unchanged. Only after
the trial is evaluated will the named build-policy implementation encode the
approved default and explicit v1 fallback.

**Files touched.**

1. `docs/research-log/Performance/2026-09-15-build-optimization.md`: record this
   isolated trial and its eventual acceptance decision.

The trial uses separate objects and an archived binary outside production output.
It initially changes no simulator source or checked-in build flags.

**Commit hashes.** Trial source: `a612bf2b` (plus documentation-only campaign
setup). Trial/acceptance record: `6b71dee0`. No production implementation commit yet.

**Regression verdict so far.** The isolated candidate builds successfully. All
16 short cases (32 runs) match complete phase statistics, effective configuration,
and warmup/ROI retirement and cycle counts; an independent saved-output audit
also passes. Normal C++ tests pass 899 cases / 81,745 assertions, with seven
native skips; memory-value C++ tests pass 904 cases / 83,838 assertions, with seven
native skips. Python passes 66 tests with four skips; performance-tool tests pass
seven tests. These historical C++ test targets append `-Og`; they validate the
v2 code paths but are not optimized-simulator timing evidence. All 15 long
5M/50M comparisons also pass with exact reported parity, including the independent
saved-output audit. The 24 protected timing runs also match exactly. A separate audit re-read stdout
counts, complete phase/configuration data, input and binary hashes, and recomputed
KIPS. No own build, profiler, or regression simulation overlapped the timings.

**KIPS before/after.** Three alternating pairs per workload, 1M/3M, CPU 14;
medians and observed ranges below. These are incremental changes from the
preserved, already optimized v1 binary, measured together on this host.

| Workload | v1 KIPS (range) | v2 KIPS (range) | Change |
| --- | --- | --- | --- |
| sqlite | 448.72 (440.48–451.19) | 428.17 (427.16–431.60) | −4.58% |
| omnetpp | 514.08 (508.96–515.33) | 491.03 (486.89–491.79) | −4.48% |
| gcc | 607.53 (606.32–608.80) | 580.89 (573.72–583.79) | −4.39% |
| mcf | 157.01 (156.96–157.89) | 148.79 (147.97–149.35) | −5.23% |

All twelve individual pairs favor v1. Individual results and host-load records
are retained in `01-v2/timing/runs.json` and `independent-audit.json`. This trial
provides no speedup evidence for raising the ISA floor; the cost is repeatable
on the local host and should be remeasured on the destination CPUs. Recorded
one-minute host load spans 1.02–1.65; process CPU/wall ratios span
0.999893–0.999928. The host is shared; these observations do not guarantee
absence of external contention.

**Status.** Observationally inert over the stated checks. Retain v2 as the
user-approved platform policy, **not as a performance optimization**; keep the
explicit v1 fallback. The later named-build implementation will encode that
policy. Do not present a release-to-fast gain as recovery of this cost unless a
separate cumulative comparison establishes it. Independent task review passed for both specification compliance and evidence
quality; no actionable findings. Review is preserved as `task-1-review.md` in the
campaign evidence.

**Evidence.** `00-baseline/`, `baseline.json`, and `01-v2/` under the campaign
evidence root. The preserved baseline exactly matches the binary of the archived
`11-cache-guards/long-regression` reference from the preceding campaign; no
assumption of source-only equivalence is needed for that reference.

## 2. Separate project assertions from operational validation

**Issue.** Disabling the existing assertions indiscriminately would remove some
configuration and decompression checks. Fast mode needs an explicit project
assertion policy while operational errors remain detectable.

**Fix.** Introduce a dependency-free `CHAMPSIM_ASSERT` helper, migrate owned
invariants, and retain configuration/resource/decoder checks as ordinary errors.
Preserve the existing valid trace-reading contract, including concatenated zstd
frames and the tested treatment of trailing partial decompressed records.

**Files touched.**

1. `inc/champsim_assert.h`: add a standalone, constexpr-compatible assertion
   controlled by `CHAMPSIM_ENABLE_ASSERTIONS`, defaulting to enabled.
2. `inc/blbp/blbp.h`: migrate internal invariants; validate configuration vector lengths.
3. `inc/extent.h`: use the project assertion in the constexpr extent constructor.
4. `inc/inf_stream.h`: preserve codec return status, report initialization/I/O/decoder
   errors, distinguish clean EOF, and propagate stream-buffer exceptions.
5. `inc/msl/stat_methods.h`: reject invalid set-dueling geometry with ordinary errors.
6. `inc/util/span.h`: migrate iterator and derived-bandwidth invariants.
7. `prefetcher/ip_stride/ip_stride.cc`: migrate the lookahead invariant.
8. `prefetcher/spp_dev/spp_dev.cc`: migrate internal table/counter invariants.
9. `replacement/drrip/drrip.cc`: migrate victim iterator invariants.
10. `replacement/lru/lru.cc`: migrate victim iterator invariants.
11. `replacement/ship/ship.cc`: migrate victim iterator invariants.
12. `src/cache.cc`: migrate internal cache invariants.
13. `src/ooo_cpu.cc`: migrate internal scheduling and memory-operation invariants.
14. `src/register_allocator.cc`: validate register-file size and migrate the free-list invariant.
15. `src/vmem.cc`: validate construction parameters before dependent arithmetic;
    migrate the available-page invariant.
16. `src/dram_controller.cc`: validate geometry before unsafe arithmetic, preserving
    direct-mapper checked-getter behavior and separately validating controller capacity.
17. `src/legacy_memory_backend.cc`: identify invalid legacy DRAM keys at construction.
18. `src/static_environment.cc`: validate user bandwidths and PTE-page size at
    construction; keep the per-cycle bandwidth constructor unchanged.
19. `src/main.cc`: catch errors from trace setup/probing and simulation reads,
    emit a useful diagnostic and return nonzero.
20. `test/cpp/src/048-stat-methods.cc`: cover invalid set-dueling geometry; apply
    the required formatter to the touched file.
21. `test/cpp/src/083-decompress-stream.cc`: exercise real codec validity,
    corruption, truncation, EOF, zstd concatenation and output-buffer boundaries.
22. `test/cpp/src/088-tracereader-v2-zst.cc`: use a real valid naming fixture and
    independently test missing-file errors.
23. `test/cpp/src/168-blbp-core.cc`: cover invalid BLBP vector lengths.
24. `test/cpp/src/201-register-rename.cc`: cover register-file construction limits.
25. `test/cpp/src/501-static-environment.cc`: cover invalid operational configuration
    and preserve valid zero bandwidth and PTE-page boundaries.
26. `test/cpp/src/700-dram-size.cc`: cover unrepresentable controller capacity.
27. `test/cpp/src/703-dram-address-mapping.cc`: cover invalid mapper geometry while
    retaining existing oversized-row construction/copy/getter expectations.
28. `test/python/test_champsim_assert.py`: compile/run real C++ probes for enabled,
    disabled, NDEBUG-independent, diagnostic and constexpr behavior.
29. `test/python/test_trace_decompression_errors.py`: test real compressed traces
    through the CLI, including early probing and later simulation failures.
30. `test/python/test_operational_config_errors.py`: require useful CLI errors
    for aggregate DRAM overflow and preserve valid PTE/register boundaries.
31. `docs/research-log/Performance/2026-09-15-build-optimization.md`: record the
    implementation, checks, investigations and eventual performance verdict.
32. `docs/superpowers/plans/2026-09-15-build-optimization.md`: reconcile task
    completion checkboxes with the recorded acceptance evidence.

**Commit hashes.** Initial implementation: `99365e1e`, from predecessor
`ebd94626`; review fix: `8f0b0687`. Final acceptance is documented by this entry.

**Regression verdict so far (pre-review-fix candidate).** All 16 short cases (32 runs) match complete reported
phase/configuration/retirement/cycle results, including the independent artifact
audit. All 15 long comparisons at 5M warmup / 50M simulation also pass exact
reported parity, including the independent saved-output audit. Final normal C++: 912 passed / 81,837
assertions, seven native skips; memory-value C++: 917 passed / 83,930 assertions,
seven native skips. Focused enabled and disabled checks each pass 15 cases / 100
assertions. Python runs 72 tests with four native skips; the three CLI decoder
tests pass against the explicitly selected candidate. Three standalone tools
build. Historical test targets still append `-Og`; these are correctness checks.
The disabled diagnostic probe also defines `NDEBUG` deliberately to test operational
errors; it is not the later fast policy, which leaves vendored assertions alone.

The audit classifies 61 owned assertions: 40 internal invariants migrated and 21
operational sites converted to ordinary validation. Nine vendored assertions and
16 `static_assert` sites remain unchanged. No required state mutation was inside
an owned asserted expression. Invalid-input handling intentionally changes from
assertion/undefined/silent failure to diagnostics; neutrality concerns valid
simulations and their complete reported behavior.

Early setup failures and inherited Conda flags are excluded from acceptance and
preserved. Final commands scrub those variables and use the controlled v2/generic
policy. The existing suite caught an early change to the direct mapper's error
point; construction/copy and checked-getter behavior were restored before freezing.
A naming test's nonexistent positive-control path was replaced with a real fixture,
while missing-file behavior remains tested independently.

An acceptance run was also interrupted to investigate suspected premature
truncation when output remained buffered. Real gzip/xz/bzip2/zstd tests at 65,536,
65,537 and 131,072 decoded bytes all passed exact-byte and EOF checks. Inspection
of pinned zstd 1.5.7 found its retained input byte prevents that suspected path.
No valid-input failure was reproduced and no speculative decoder rewrite was
applied; retain the added boundary tests. The interrupted run and provisional
withdrawal/resolution receipts remain in `02-assertions/rejected-drain-v1/`; that
directory name does not establish a demonstrated bug. Fresh acceptance uses the
same verified production binary, SHA256
`803c8f2312f598925daae5cc87ab5596e63ae134a64c51af971934a36d463cec`.

**Review finding.** Independent review reproduced an uncaught operational
configuration error: aggregate `pmem.bank_rows=4611686018427387904` throws
`std::invalid_argument` while the construction scope catches only
`std::runtime_error`. The candidate aborts instead of reporting `ERROR:`/exit 1.
Acceptance is withheld despite valid-trace parity. Fix the catch/type mismatch,
add a CLI regression, preserve the reviewed binary and results, and repeat the
full gate on a distinct replacement binary. Review also identified initialization
diagnostic/coverage details and an empty investigation log mislabelled as passing;
these are included in the scoped fix.

**Review fix and replacement checks.** `8f0b0687` catches construction errors
through `std::exception`, checks zstd initialization bounds and adds contextual
initialization diagnostics. The exact formerly aborting CLI case now exits 1
with `ERROR:`; valid PTE/register/capacity boundary checks pass. Independent
scoped re-review approves specification compliance and code quality with no
remaining findings. Existing intentional VMEM/branch warnings remain visible.
The replacement binary is `candidate-fix1-bin/champsim`, SHA256
`36f57f4c12aa13223c28aebe000c3b17dbe3c2753c02cf075c55fa0e7ba3621f`.
Full normal C++: 913 passed / 81,840 assertions, seven native skips; payload:
918 passed / 83,933 assertions, seven native skips. Python runs 74 tests with
four native skips. Affected enabled/disabled, post-commit boundary, CLI and
standalone checks also pass. Fresh short comparisons and their independent audit
pass 16 cases / 32 runs. All 15 replacement long comparisons at 5M/50M and
the independent successor audit also pass exact reported parity. All 24 timing
runs preserve complete phase/configuration/count/config-id parity; an independent
saved-output audit reverified inputs/binaries/stdout and recomputed the figures.

**KIPS before/after.** Three alternating pairs per protected workload, CPU 14,
1M warmup / 3M simulation; medians and observed ranges below. Both binaries use
v2/generic, `-O3`, assertions enabled and the same pre-mode symbol policy. This
isolates the assertion/error-handling stage against its accepted v2 predecessor.

| Workload | Previous v2 KIPS (range) | Project-assertion v2 KIPS (range) | Change |
| --- | --- | --- | --- |
| sqlite | 428.96 (422.57–431.89) | 436.30 (434.19–438.23) | +1.71% |
| omnetpp | 487.13 (486.22–488.13) | 493.01 (491.49–495.82) | +1.21% |
| gcc | 576.30 (574.58–576.57) | 587.16 (586.27–587.80) | +1.88% |
| mcf | 148.01 (147.56–149.50) | 151.86 (151.67–154.74) | +2.60% |

All twelve pairs favor the candidate. Process CPU/wall ratios span
0.999872–0.999933; recorded one-minute host load spans 1.00–1.18. No own build,
regression, profiler or remote experiment overlapped the timing window. These
are incremental results, not gains over the initial unoptimized simulator or
over the campaign's original v1 baseline. Fast assertion removal has not yet
been measured.

**Status.** Observationally inert for valid simulations over the complete short,
15-workload long, unit and timed checks. The stage also improves measured KIPS
on all four protected workloads. Operational invalid-input diagnostics are
intentionally stronger. Code review, scoped re-review and final evidence review approve the implementation
and all acceptance gates. Retain this stage and proceed to named build modes.

**Evidence.** `02-assertions/` contains implementation/test/investigation receipts.
Final acceptance comparisons and audits are under `02-assertions/fix-1-validation/`;
its predecessor is `01-v2/champsim` and its candidate is
`02-assertions/candidate-fix1-bin/champsim`. Initial and corrected review reports
are preserved at the campaign evidence root.

## 3. Isolate named build modes and record compiler provenance

**Issue.** The old Make rules share production/test objects, append `-Og` for
unit tests, and do not give mode/ISA/compiler choices distinct artifact paths.
This risks stale or mixed outputs and makes the effective build policy unclear.

**Fix.** Introduce debug, release and fast modes with independent target/ISA
selection, isolated object/dependency/generated-header/binary paths, and atomic
compatibility aliases. Validate compiler options and expose standalone JSON
build information without constructing the simulated machine or changing its
configuration identity. Preserve the optional native backend's build checks.

**Files touched.**

1. `Makefile`: dispatch named and ordinary builds to isolated mode/flavor rules.
2. `global.options`: remove unconditional optimization from shared options.
3. `config/build_config.py`: resolve, validate and fingerprint build policy;
   generate metadata, canonical paths and atomic compatibility aliases.
4. `config/build_rules.mk`: build separate simulator/test artifacts with selected
   flags, dependencies and native preparation.
5. `config/makefile.py`: preserve configured registry/output roots in discovery.
6. `config/filewrite.py`: pass discovery path metadata to the Make emitter.
7. `config/ramulator2_build.py`: propagate architecture and enforce locked,
   source-root ownership of the native library.
8. `config/compile_commands/selected.py`: export the actual selected compiler argv.
9. `config/compile_commands/src.py`: route source export through selected policy.
10. `config/compile_commands/inc.py`: route header export through selected policy.
11. `config/compile_commands/module.py`: preserve module options in selected export.
12. `config/compile_commands/test.py`: preserve test mode/flavor in selected export.
13. `inc/build_info.h`: declare the standalone build-provenance interface.
14. `src/build_info.cc`: serialize generated compiler/build metadata as JSON.
15. `src/main.cc`: handle standalone `--build-info` before model construction.
16. `test/python/test_build_modes.py`: exercise actual Make/compiler behavior,
    isolation, invalid flags, ELF symbols, and compiler macro parsing.
17. `test/python/test_build_info.py`: test standalone and mixed CLI invocations.
18. `test/python/test_ramulator2_build.py`: check native ownership and locking.
19. `tools/perf/benchmark_ptw.py`: capture provenance outside measured execution.
20. `tools/perf/compare_optimization.py`: retain provenance in comparison manifests.
21. `tools/perf/test_build_info_provenance.py`: test available/unavailable metadata.
22. `.github/workflows/test.yml`: resolve the native production object leaf for
    `run_oracle.py` and update native log/manifest artifact paths.
23. `CLAUDE.md`: document modes, roots, overrides and compatibility boundaries.
24. `tools/perf/README.md`: document reproducible build and measurement selection.
25. `test/ramulator2/README.md`: document canonical native compatibility builds.
26. This campaign log: record implementation, corrections, exact evidence and
    incremental performance acceptance.
27. `docs/superpowers/plans/2026-09-15-build-optimization.md`: mark completed
    implementation and acceptance steps.

**Commit hashes.** Initial implementation: `35877649`, based on `d7976387`;
symbol-retention and compiler-parser corrections: `20fcec7e`; grouped linker
switch correction: `d71de93c`; test-fixture isolation: `65fe18c3`. Scoped
reviews approve specification compliance and quality. The subsequent
documentation commit records the acceptance below.

**Regression verdict.** Inert over the completed checks. The final named v2
release passes all 16 short cases (32 runs) and all 15 long comparisons at
5M/50M. Independent saved-output audits pass for both matrices, including full
phase/configuration/build-id and retirement/cycle-count parity. Fresh normal
and payload C++ suites pass 913 and 918 cases respectively (seven expected
skips each; 81,840 and 83,933 assertions). Python passes 105 tests with five
expected skips; fast and debug each pass eight CLI checks. Standalone and
performance-tool checks also pass. Superseded intermediate binaries and any
stopped runs remain preserved with their reasons; none supply final timing.
The immediate accepted simulator predecessor
is `02-assertions/candidate-fix1-bin/champsim` (SHA256 `36f57f4c…3621f`), with the
long reference at `02-assertions/fix-1-validation/long-regression`.

Separate deployment preparation has established a GCC 11 build of that source on
ETH kratos6, using controlled v2 dependencies. Raw/gzip/bzip2/xz/zstd synthetic
smokes all complete with matching statistics/configuration/counts; their different
trace-path labels are checked explicitly before cross-codec comparison. A further
65-run check passed on thirteen nodes, covering Xeon 5118, Xeon 6226R, EPYC 7742
and EPYC 9554, with an independent saved-artifact audit. Six remaining node jobs
never obtained resources and were cancelled; nineteen-node execution coverage is
not claimed. A named GCC 11 release build also passed all five codec smokes, with
the same complete fingerprint as the predecessor fleet. These cluster binaries
precede the final build-policy corrections.

Separate native compatibility checks pass: 919 enabled C++ cases (one expected
disabled-backend-only skip), both exact transaction oracles, and all four native
simulator/replay cases. Actual CMake/driver flags and shared sim/test library
identity were independently checked; the library remained unchanged. An
optimization-dependent GCC 13/libstdc++ warning in unchanged test 183 is retained
with its focused sanitizer investigation; no invalid memory access was found.

**KIPS before/after.** Three alternating pairs at 1M/3M on CPU 14; medians
include actual warmup and ROI retirement. This is incremental over the accepted
assertion-enabled predecessor (`36f57f4c…3621f`), not the initial source baseline.
Both binaries retain assertions, `-O3`, v2/generic ISA and the same libraries.

| Workload | Before KIPS | After KIPS | Change | Before range | After range |
| --- | ---: | ---: | ---: | --- | --- |
| sqlite | 432.48 | 447.83 | +3.55% | 430.20–432.63 | 447.38–448.99 |
| omnetpp | 497.66 | 512.53 | +2.99% | 491.84–500.26 | 511.02–512.57 |
| gcc | 583.69 | 605.40 | +3.72% | 580.78–588.59 | 602.41–608.96 |
| mcf | 152.52 | 157.19 | +3.06% | 148.80–154.05 | 155.12–157.68 |

All twelve individual pairs favor the candidate: sqlite +4.37/+3.51/+3.45%,
omnetpp +2.46/+2.69/+4.21%, gcc +4.33/+2.35/+4.24%, and mcf
+2.04/+3.38/+4.24%. CPU/wall ratios are 0.999899–0.999936, with one-minute
load 0.45–1.13. No own builds, tests, profilers or remote experiments overlapped.
The independent artifact audit verifies all 24 runs, binary/trace/configuration
hashes, full results, counts, pair ordering and recomputed KIPS.

**Decision.** Retain the named-build implementation: observed behavior is inert,
the build contract passes its tests, and this controlled comparison shows a
repeatable gain on every protected workload. Attribute it to this combined build
stage; the experiment does not isolate debug symbols or artifact layout as the
cause. Fast assertion removal is evaluated separately next.

**Evidence.** `03-build-modes/` and `eth-build-recon/` under the campaign root.

**Corrections found during acceptance.** The first committed candidate
(`c1d96880…a5780d`) passed the entire short and 15-workload long gate, including
independent audits. Review nevertheless reproduced an invalid build-policy
combination: user `LDFLAGS=-s` removed the promised debug and symbol sections.
Likewise, `module.options=-g0` removed module debug information. The correction
rejects those options across supported argument/response-file channels and
checks actual ELF sections in every mode. A bounded GCC/Clang/linker-option audit
also covers long and abbreviated debug options, selective symbol lists and
explicit linker scripts. Preserving options remain accepted. The grouped-switch
source Python suite passed 104 tests with five expected skips; the later fixture
correction raises the final count to 105. A second review
found grouped GNU ld switches such as `-Wl,-sx`; four validation lines now reject
stripping combinations using the argumentless-short-option alphabet. Generated
groups, repetitions and nested responses are tested, with supported long options
preserved. Scoped re-review approves the fix.

A real ETH Clang 14 build then exposed a separate portability bug: a regular
expression consumed a newline after an empty preprocessor definition and lost
the following assertion definition. Horizontal-whitespace parsing and a real
compiler regression cover both assertion and payload definitions. The actual
Clang output is preserved. The intermediate symbol-fix binary passed its short
matrix; its long run was stopped and preserved when this parser fix superseded
it. Final acceptance uses another distinct frozen binary, without overwriting
either earlier result. Initial Clang invocation failure due to an absent
unversioned executable is recorded separately as a setup error.

The latest native compatibility run passes all 919 enabled C++ cases, both
transaction oracles and four integration/replay cases, with independent actual
flag/library-ownership checks. Its frozen policy helper is `e867b54e…c2607`.
The final `d71de93c` helper changes only user-option validation from that version:
an AST comparison and real compiler probes independently confirm identical
standard simulator/test/native policies, apart from the recorded helper hash
and resulting policy key. This is explicit source/policy reconciliation, not a
claim that the preserved native binary was rebuilt from the final helper.

Both actual ETH build nodes also pass final-helper policy and real compiler-macro
reconciliation against their preserved GCC 11 and Clang 14 full builds. A first
check on the headnode correctly failed compiler-identity equality: its GCC
package is `11.3.0-1ubuntu1~22.04`, while kratos12 uses
`11.3.0-1ubuntu1~22.04.1`, with different executable hashes. The check was rerun
on kratos12/kratos13 and passed without relaxing identity validation. Thus even
matching headline compiler versions do not establish identical toolchains.

The approved-source local v2 release is frozen at
`03-build-modes/candidate-release-v2-final/champsim`, SHA256
`ebd0abafe640c83144c09b5579dce51846f46a9a9e41644dc474aaa3a33af1d8`.
Its source commit is `d71de93c`; the complete fresh gate passed under
`03-build-modes/release-validation-final/` against the accepted assertion-stage
binary. The later `65fe18c3` changes only test-fixture isolation, so the frozen
production binary and its historical source receipt remain unchanged.

The final rooted `make pytest` invocation exposed a fixture defect: inherited
`OBJ_ROOT`, `DEP_ROOT` and `BIN_ROOT` let temporary compiler fixtures write into
the supplied project output roots. The test-only correction clears ambient
roots while preserving explicit per-fixture overrides. A real Make regression
fails before the fix; the exact formerly failing rooted invocation then passes
105 tests, with all 10,879 output-tree entries unchanged. Scoped review approves
the fix. The initial failure and the temporary unrooted workaround remain
recorded; the workaround is not the accepted solution.

**GCC 11 build-time limitation.** On ETH, the standard `-O3 -g3` release build
took about 18 minutes with four compile jobs. Two large translation units,
`generated_registry.cc` and `static_environment.cc`, dominated; an earlier
20-minute allocation timed out. A ten-second Linux perf sample of the active
compiler processes attributed 26.45% of sampled cycles to
`drop_overlapping_mem_locs`, part of GCC's variable-tracking pass. This is
compiler profiling, not simulator KIPS. The
[GCC 11 debugging-option documentation](https://gcc.gnu.org/onlinedocs/gcc-11.5.0/gcc/Debugging-Options.html)
describes variable-location and assignment tracking; the function is in
[GCC 11's variable-tracking implementation](https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-11/gcc/var-tracking.c).

An isolated recompilation of `static_environment.cc`, keeping the actual
`-O3 -g3` and v2/generic flags, took 58.24 seconds with
`-fno-var-tracking-assignments`, or 55.44 seconds when variable tracking was also
disabled. Both objects retained `.debug_info` and `.symtab`; the former reported
only 0.07 seconds of variable-tracking CPU time. This supports assignment
tracking as the cause of the extreme compile cost. These are diagnostic
one-off builds, not a controlled simulator throughput comparison or an accepted
optimization. The standard policy remains unchanged: disabling tracking trades
away optimized-variable debugging quality and needs its own evaluation before
adoption. Raw perf data, compiler commands and time reports are preserved under
`eth-build-recon/named-fix1-cluster-results/`.

## 4. Disable ChampSim invariant checks in v2 fast

**Issue.** Release checks internal invariants during simulation. Removing their
runtime cost may improve throughput, but checks that perform required work or
validate operational failures must not disappear.

**Fix.** Select the already implemented fast mode. Its only intended compiler
policy change from named release is `CHAMPSIM_ENABLE_ASSERTIONS=0`; keep
`-O3 -g3`, x86-64-v2/generic, the compiler and libraries unchanged. Catch2 and
operational diagnostics remain active. This stage validates removal separately
from the earlier assertion migration and named-build changes.

**Files touched.**

1. This campaign log: record the fast policy comparison, gates and KIPS verdict.
2. `docs/superpowers/plans/2026-09-15-build-optimization.md`: track the completed
   fast-mode acceptance steps.

**Commit hashes.** Validation starts at `052e93c3`, using production source
`d71de93c`. Fast selection was implemented in `35877649` on top of the assertion
policy from `99365e1e`/`8f0b0687`; this stage needs no additional source change.
The subsequent documentation commit records acceptance.

**Regression verdict.** Inert over the completed checks. Actual compiler/response-file
and metadata audits confirm the intended policy difference. Normal C++ passes
913 cases (7 skips, 81,840 assertions); payload passes 918 (7 skips, 83,933
assertions). The three assertion probes and eight focused operational/CLI cases
pass. All 16 short comparisons (32 runs) and the independent saved-output audit
pass. Frozen release `ebd0abaf…af1d8` is compared with fast `467161d7…e5275`,
using release's accepted 15-case long reference. All 15 long cases and both
saved-output audits now pass at 5M/50M. Three standalone tools rebuild byte for
byte and pass their operational checks. No production or test source fix was
needed. Independent review approves specification compliance and task quality
with no findings. Failed command/fixture-preparation attempts are preserved and
explained in the report; no expected simulation statistics were changed.

**KIPS before/after.** Three alternating pairs at 1M/3M on CPU 14, comparing
named v2 release directly to named v2 fast. All 24 runs retain complete reported
parity; the independent audit verifies hashes, counts, ordering and recomputed
KIPS. These results are incremental over named release.

| Workload | Release KIPS | Fast KIPS | Change | Release range | Fast range |
| --- | ---: | ---: | ---: | --- | --- |
| sqlite | 444.93 | 451.38 | +1.45% | 439.11–445.79 | 451.28–451.89 |
| omnetpp | 506.32 | 512.39 | +1.20% | 504.19–506.63 | 511.63–515.67 |
| gcc | 602.69 | 606.72 | +0.67% | 599.66–602.72 | 605.56–607.24 |
| mcf | 156.09 | 158.53 | +1.56% | 155.71–156.36 | 156.78–160.21 |

All twelve individual pairs favor fast: sqlite +2.77/+1.45/+1.37%, omnetpp
+0.99/+1.20/+2.28%, gcc +0.67/+1.27/+0.47%, and mcf +0.44/+1.81/+2.47%.
CPU/wall ratios are 0.999892–0.999939; one-minute load is 0.25–1.18. All own
builds, tests, profiles and remote experiments stopped before timing.

**Decision.** Accept v2 fast as the optional assertion-disabled mode. The gain
is modest but consistent across these pairs, with no observed behavior change.
Plain `make` remains assertion-enabled release. These local results do not
establish fast-mode KIPS on ETH or ARM, and they do not disable third-party
assertions or promise this gain for every simulator configuration.

**Evidence.** `04-fast/` under the campaign evidence root. The frozen inputs
remain under `03-build-modes/candidate-{release,fast}-v2-final/`.

## 5. Evaluate an explicit x86-64-v3 fast profile

**Issue.** v2 excludes newer vector and integer instructions available on the
inspected ETH CPU partition. Compiler eligibility alone does not show that v3
improves simulator throughput or preserves every reported result.

**Fix.** Trial an explicit `X86_ISA=x86-64-v3` selection with generic tuning and
fast's existing assertion policy. Keep v2 as the default. Use the accepted v2
fast binary as the immediate predecessor and apply the full acceptance gate.
The profile requires CPU and OS support for the cumulative v3 features, as
specified by the [x86-64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex).

**Files touched.**

1. `config/build_config.py`: add explicit v3 selection and verify its required
   compiler features while retaining default v2 and rejecting excess extensions.
2. `test/python/test_build_modes.py`: add real compiler/Make v3 coverage and
   preserve the supported test-host/compiler boundaries.
3. `CLAUDE.md`: document opt-in v3 requirements and the unchanged default.
4. This campaign log: record the profile trial, gates and acceptance verdict.
5. `docs/superpowers/plans/2026-09-15-build-optimization.md`: track profile gates.

**Commit hashes.** Starts from fast acceptance `084f7a1d`; the profile
implementation is `6b7e1641`; test-only compiler portability correction is
`1f093c02`. Independent review approved the implementation. The performance
gate rejects it; `7cf06993` restores all three changed files exactly to the
accepted predecessor. The trial and restoration remain visible in history.

**Regression verdict.** The frozen v3 candidate passes all 16 short cases
(32 runs), all 15 long 5M/50M comparisons and their independent saved-artifact
audits. Normal C++ passes 913 cases (7 skips, 81,840 assertions), payload passes
918 (7 skips, 83,933 assertions), and corrected Python execution runs 108 tests
with five expected skips and no failures. A first Python invocation selected Catch2 for a
production CLI test; the failed setup is preserved, and correcting the executable
selection required no source change. The test-only follow-up skips the two
positive v3 fixtures when the selected compiler lacks named v3 support; the
unsupported-profile diagnostic and v2 fallback remain tested. Four focused
checks pass, and a transparent compiler driver rejecting only named v3 produces
the two expected skips. This is capability coverage, not an actual GCC 9/10 run.

The candidate is `05-profiles/candidate-fast-v3/champsim`, SHA256
`3be0bedddee5673b35ca4c69081df26ce91e70c254be63fa87e5769643b36f06`, built from
`6b7e1641`. The exact predecessor remains
`03-build-modes/candidate-fast-v2-final/champsim`, SHA256
`467161d7aaef15cf2fb70eae0b0a4fbc1c446538eda318af1f7f33c7c7ee5275`, with long
reference `04-fast/validation/long-regression`.

**KIPS before/after.** Three alternating pairs at 1M/3M, CPU 14, detailed PTW,
legacy DRAM, identical compiler/libraries and unchanged assertion policy:

| Workload | v2 fast | v3 fast | Change | Before range | After range |
| --- | ---: | ---: | ---: | --- | --- |
| sqlite | 450.20 | 431.62 | -4.13% | 448.85–453.43 | 430.70–432.93 |
| omnetpp | 514.48 | 487.07 | -5.33% | 509.90–516.82 | 486.63–489.83 |
| gcc | 611.31 | 578.26 | -5.41% | 611.10–612.51 | 577.84–580.41 |
| mcf | 159.39 | 151.11 | -5.19% | 159.26–159.62 | 149.90–151.30 |

All twelve individual pairs favor v2 fast; v3 loses 4.04–5.95% across pairs.
The independent saved-output audit passes all 24 runs, including hashes, full
reported parity, ordering, retired counts and recomputed KIPS. Process CPU/wall
ratios are 0.999895–0.999934; sampled one-minute load is 0.54–1.09. All own
builds, tests, profilers and remote experiments stopped before timing.

**Decision.** Reject the explicit v3 profile as a performance optimization.
Its observed simulation behavior is inert, and the tested fleet can execute it,
but the consistent 4.13–5.41% median regression fails the acceptance bar.
The trial source and frozen evidence remain available; the profile is removed
from the retained build policy. No retiming is justified by these consistent
results. The microarchitectural cause of this compiler-codegen regression has
not been isolated; it is not attributed specifically to AVX frequency effects.
Default v2 release and optional v2 fast remain the accepted configurations.

Restoration checks pass: the committed tree equals `084f7a1d`; the build helper
returns to SHA256 `3e2831c65498c7e8b3b96cdddc308ca246c8c49aec0bf013113dc97ba83a088c`.
All 27 build-policy tests finish without failures (one skip), and Python runs
105 tests without failures (five skips). The accepted release and fast binary
hashes, rejected v3 binary and read-only receipts remain unchanged. Because the
accepted production source and frozen bytes are restored exactly, the completed
accepted C++ and long-regression evidence still applies; no new simulated
behavior is introduced by the restoration. Evidence: `05-profiles/rejection-restoration/`. The scoped restoration review
approves spec compliance and quality with no open findings.

**Evidence.** New artifacts belong under `05-profiles/`; any separate ETH builds
and synthetic smokes are compatibility checks, excluded from KIPS. Actual
Clang 14 v2-fast and v3-fast builds from the exact committed source both pass
five codec smokes on kratos13. The same v3 binary passes fifteen further smokes
on kratos0, kratos10 and safari-nexus1: four CPU families and twenty v3 runs in
total. Independent audits retain full phase/configuration/metadata/count parity
with the established predecessor fingerprint. The actual Clang policies differ
only in selected ISA and verified build identity. All four Slurm jobs completed
successfully before timing; exact source, transfer, build, accounting and audit
receipts are under `05-profiles/eth/`.

## 6. Reject unrestricted ETH headnode-native ISA

**Issue.** The headnode's native compiler target permits instructions that one
member of the intended CPU fleet cannot execute. Historical successful binaries
do not guarantee compatibility of future compiled paths.

**Fix.** Resolve and record the native target, then reject fleet-wide adoption at
the compatibility gate. The actual ETH headnode reports Xeon Gold 5118 and GCC
11.3.0; its native query resolves both architecture and tuning to
`skylake-avx512`, enabling AVX512F/BW/CD/DQ/VL. Kratos10's EPYC 7742 loader reports
v2/v3 support, without v4. The ABI places those AVX-512 features in v4, while
[GCC's target documentation](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/x86-Options.html)
confirms that `-march` permits instructions for the selected machine.

**Files touched.**

1. This campaign log: record the resolved target, incompatible fleet member and
   rejection before a simulator trial.
2. `docs/superpowers/plans/2026-09-15-build-optimization.md`: record the completed
   native compatibility decision at stage closeout.

**Commit hashes.** No production native-profile implementation is adopted.
The decision accompanies the rejected-v3 stage record; the accepted production
policy is restored by `7cf06993`. No native-profile source change was needed.

**Regression verdict.** Not applicable: no native-ISA candidate is admitted to
the regression gate. This is a compatibility rejection, not a demonstrated
simulator statistics failure or an observed illegal-instruction crash.

**KIPS before/after.** Not measured. No native-ISA speed claim is made.

**Evidence and decision.** The exact headnode command/output is preserved under
`eth-headnode-native/`; the nineteen-node ISA/loader audit is under sibling
`2026-09-15-build-portability/eth-isa-audit-compact-20260915T111617.630091Z/`.
Retain v2 as default; the separate v3 trial was rejected on throughput. A future native build needs an
explicitly restricted destination set and its own validation; it is not a
portable fleet profile in this campaign.

## 7. Close out retained artifacts and publish the local release alias

This section records the initial `dc0c95e1` closeout. Its source-equivalence
checks and `06-closeout/` receipts remain historical; Section 8 records the later
input-guard correction and its separate policy/source reconciliation.

**Issue.** The accepted implementations and their evidence span several source
commits and immutable binaries. The local `bin/champsim` alias still contained
the preserved pre-campaign binary, so it did not expose the accepted standalone
build-information interface. Closeout must connect each retained stage to its
actual predecessor and evidence without treating later documentation or test-only
commits as fresh simulator builds.

**Fix.** Reconcile the accepted source, frozen binaries, adjacent policy receipts,
saved test logs, short/long parity audits, and timing audits. Keep release as the
plain-`make` default, x86-64-v2/generic as the x64 default, and fast as the
optional assertion-disabled mode. The rejected v3 selection and unrestricted
head-native profile remain absent from production commands and policy. After the
reconciliation passed, atomically replace only the local `bin/champsim` alias
with the exact accepted release-v2 bytes, using a temporary file in `bin/` and a
same-directory rename.

**Files touched.**

1. This campaign log: add the final source, artifact, gate, result, publication,
   and limitation reconciliation.
2. `docs/superpowers/specs/2026-09-15-portable-build-modes-design.md`: replace the
   stale pre-implementation introduction and link this completed campaign record.
3. `docs/superpowers/plans/2026-09-15-build-optimization.md`: mark receipt and
   documentation work complete while leaving whole-campaign review open.

`bin/champsim` is a local ignored build artifact rather than a committed source
file. Its replacement and all closeout receipts are preserved under
`06-closeout/` in the external evidence root.

**Commit hashes.** Task 6 starts from
`18ce2dd05e9e9875c4134b90dcca778652f4eb81`. Retained production behavior is
from `d71de93c688243283ae2cb2e05ec4b6fcc5dcfb2`; the later
`65fe18c3dac03c80eb5c8c4b8f51f7c2a4c31991` changes only test-fixture isolation.
Accepted documentation source is `084f7a1db5274b60c1aa3a1ae27f78121a3bbd02`.
The rejected v3 trial is preserved in history at `6b7e1641` and `1f093c02`, and
`7cf069933df9614c38ea676d7fff1267c4308055` restores its three production/test
files exactly to `084f7a1d`. This closeout adds no simulator, build, test,
dependency, or native source change.

**Regression verdict.** The read-only reconciliation passes; no build, test,
simulation, profiler, or remote job was rerun for closeout. Each accepted or
evaluated candidate's saved gate contains 16 short cases / 32 runs with complete
reported parity, 15 long cases / 15 predecessor plus 15 candidate runs at 5M
warmup and 50M simulation with exact parity, and 24 timing runs / 12 alternating
pairs across four protected traces. The long set is the 14 SPEC26 workloads plus
the existing v1 mcf control. Stage 1's actual short audit is
`01-v2/short-regression/independent-audit.json`; its timing audit is likewise
under `01-v2/timing/`, while later stages use their saved validation roots.

The retained and reference identities are:

| Role | Source | SHA256 | Policy or disposition |
| --- | --- | --- | --- |
| Campaign baseline / v1 predecessor | `a612bf2b` | `fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79` | Preserved at `00-baseline/champsim` |
| Isolated v2 policy trial | `a612bf2b` | `22d757ff9ec50182219d434a666e7e21e904bebe9e55e2b32c84086c525f81e7` | Accepted as platform policy, not as a speed gain |
| Assertion/error-handling stage | `99365e1e` + `8f0b0687` | `36f57f4c12aa13223c28aebe000c3b17dbe3c2753c02cf075c55fa0e7ba3621f` | Accepted assertion-enabled predecessor |
| Named release v2 | `d71de93c` | `ebd0abafe640c83144c09b5579dce51846f46a9a9e41644dc474aaa3a33af1d8` | release, `-O3 -g3`, v2/generic, assertions 1, native 0 |
| Named fast v2 | `d71de93c` | `467161d7aaef15cf2fb70eae0b0a4fbc1c446538eda318af1f7f33c7c7ee5275` | fast, `-O3 -g3`, v2/generic, assertions 0, native 0 |
| Named debug v2 | `d71de93c` | `f0a7b40529c998640748d05069c9b66e0e319bbd88d6c7b419d767109bf0e13c` | debug, `-O0 -g3`, frame pointers, assertions 1, native 0 |
| Named release v1 fallback | `d71de93c` | `533355738c2c06eb24f1612eb2cedc1ac3750f695a85689923f93606d4df8e2a` | release, `-O3 -g3`, original x86-64/generic, assertions 1, native 0 |
| Rejected fast v3 trial | `6b7e1641` | `3be0bedddee5673b35ca4c69081df26ce91e70c254be63fa87e5769643b36f06` | Frozen for audit; absent from retained production policy |

The four retained named candidates' adjacent receipts all identify helper SHA256
`3e2831c65498c7e8b3b96cdddc308ca246c8c49aec0bf013113dc97ba83a088c`
and production source `d71de93c`; release, fast, debug, and v1 policy keys are
respectively `1d264c7c13a56cfb2ef15e36`, `13827087e0e486083ad30810`,
`21dfb5663f09db1031488f09`, and `cb0b65743e5b358691515f6d`.
At initial closeout, the later accepted commit changed tests only, and the
`dc0c95e1` production tree equaled accepted `084f7a1d` outside documentation. The v1 hash above is the
artifact-and-receipt value; an initial Task 6 handoff transposed its final bytes,
and the correction is retained explicitly in the closeout verification receipt.

Saved local test receipts report the actual totals rather than collapsing skips
into passes. The v2 trial ran 906 normal cases (899 passed, 7 skipped; 81,745
assertions), 911 payload cases (904 passed, 7 skipped; 83,838 assertions), and
66 Python tests with four skips. The assertion replacement, named release, fast,
and v3 candidates each ran the later 920 normal cases (913 passed, 7 skipped;
81,840 assertions) and 925 payload cases (918 passed, 7 skipped; 83,933
assertions). Their recorded Python totals are 74 with four skips for the
assertion replacement, 105 with five skips for named release, and 108 with five
skips for the v3 trial. The restoration then passed 27 build-policy tests with
one skip and 105 Python tests with five skips. The seven C++ skips in each later
normal/payload suite are the expected native-disabled cases.

**KIPS before/after.** These are the previously recorded incremental medians
against each stage's immediate accepted predecessor. They are repeated here
without adding percentages or combining measurements from different windows:

| Incremental comparison | sqlite | omnetpp | gcc | mcf | Decision |
| --- | --- | --- | --- | --- | --- |
| Preserved v1 → isolated v2 | 448.72 → 428.17 (−4.58%) | 514.08 → 491.03 (−4.48%) | 607.53 → 580.89 (−4.39%) | 157.01 → 148.79 (−5.23%) | Retain v2 as the approved platform floor only |
| v2 → assertion/error handling | 428.96 → 436.30 (+1.71%) | 487.13 → 493.01 (+1.21%) | 576.30 → 587.16 (+1.88%) | 148.01 → 151.86 (+2.60%) | Accept |
| Assertion stage → named release | 432.48 → 447.83 (+3.55%) | 497.66 → 512.53 (+2.99%) | 583.69 → 605.40 (+3.72%) | 152.52 → 157.19 (+3.06%) | Accept combined build stage |
| Named release → named fast | 444.93 → 451.38 (+1.45%) | 506.32 → 512.39 (+1.20%) | 602.69 → 606.72 (+0.67%) | 156.09 → 158.53 (+1.56%) | Accept optional fast |
| Named v2 fast → v3 fast | 450.20 → 431.62 (−4.13%) | 514.48 → 487.07 (−5.33%) | 611.31 → 578.26 (−5.41%) | 159.39 → 151.11 (−5.19%) | Reject v3 |

No cumulative baseline-to-final comparison was measured. The incremental
percentages are therefore not added and no cumulative speedup is claimed.

**Evidence and decision.** `06-closeout/prepublication-verification.json` binds
the accepted source, artifact, policy, test, parity, timing, native and rejection
receipts before publication. `06-closeout/publication.json` records the old
alias and preserved baseline at
`fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79`,
the atomic publication command, and the new local alias at the accepted release
SHA256 `ebd0abafe640c83144c09b5579dce51846f46a9a9e41644dc474aaa3a33af1d8`.
The published `--build-info` exactly matches the adjacent accepted receipt:
release, x86-64-v2/generic, assertions enabled, `-O3 -g3`, and native disabled.
This publishes already accepted bytes; it is not a fresh build from the closeout
documentation commit. Future ordinary `make` invocations retain their normal
canonical-build and atomic-alias behavior. No other alias was changed.

Full SPEC correctness, normal/payload C++, Python, and protected timing evidence
is actual local GCC 13 x64 evidence. ETH evidence consists of actual GCC 11 and
Clang 14 builds, real policy/macro checks, and five codec smokes per preserved
named-release build; it is not an ETH full-SPEC or KIPS claim. The rejected-v3
stage separately includes actual Clang 14 v2-fast/v3-fast builds and synthetic
codec smokes on representative CPU families. GCC 9/10 coverage is a synthetic
compiler-capability/fallback check rather than an actual local runtime.

Native compatibility evidence preserves the binary built with helper
`e867b54e5625e83012a9d41ef56f22d6daca288e614fd4a07ab577b05c3c2607`.
The final denial-only helper change is reconciled to `d71de93c` by AST, effective
policy, and real compiler-macro proof in
`03-build-modes/native-validation-review-fix2/d71de93c-source-policy-reconciliation.json`;
the native binary was not rebuilt from `d71de93c`. Native full SPEC, native fast
or v3, native KIPS, ARM hardware, and Darwin hardware are unvalidated. ARM
hardware was explicitly deferred. External installed-library ISA requirements
remain unknown without separate provenance. The existing GCC 13 test 183
`-Wnonnull` diagnostic and GCC 11 `-O3 -g3` compile cost remain documented
investigations, not suppressed or solved issues.

The 26 campaign decisions and tradeoffs are indexed by `final-rulings.md` at the
evidence root. Final whole-campaign code/evidence review and SDD archival remain
controller-owned and intentionally open in the plan. The local branch and raw
evidence are retained; nothing was pushed, merged, or cleaned up.


## 8. Reject untracked explicit linker inputs

**Issue.** Final whole-campaign review I1 found that bare filenames and `-Wl,`
filenames bypassed the explicit-script guard. A script named like an object can
introduce `INPUT` or `GROUP` children whose contents are absent from the policy
fingerprint. Real Make fixtures retained `core=42` after recompiling only the
child to return 43, with unchanged policy path, binary hash and timestamp. Direct
forwarded objects had the same stale-input defect. This establishes stale reuse;
silent debug-symbol stripping was not reproduced. Review M2 also identified an
incorrect description of the CI workflow change in Section 3.

**Fix.** Validate each response-expanded option channel independently. Reject
unconsumed positional inputs and unknown opaque forwarded linker options, while
consuming supported driver/linker operands and preserving transparent compiler
wrappers. Check resolved `-L`/`-l` library formats by magic bytes: ELF, Mach-O and
ordinary archives retain content identity; implicit scripts and thin archives
are rejected because their child inputs are untracked. This deliberately narrows
custom linker input support without implementing a general linker-script parser.
Correct the CI inventory to identify the production object leaf used by the
native oracle and its log/manifest artifact paths.

**Files touched.**

1. `config/build_config.py`: enforce input grammar and resolved-library format.
2. `test/python/test_build_modes.py`: real Make rejection, response/channel,
   wrapper/operand and changed-library regression coverage; scrub inherited
   `LDLIBS` and `LOADLIBES` from fixture environments.
3. `CLAUDE.md`: document supported library, linker-operand and wrapper boundaries.
4. This campaign log: correct M2, preserve initial closeout as historical and
   record this separate final-review correction.

**Commit hashes.** Correction follows review baseline
`dc0c95e131a7b06d940714bd10682f77c1f92421`; the exact correction commit is recorded
in `07-final-review/fix/final-fix-report.md`. The measured named candidates retain
production source `d71de93c` and helper
`3e2831c65498c7e8b3b96cdddc308ca246c8c49aec0bf013113dc97ba83a088c`.
They have not been rebuilt or relabeled as products of the new helper.

**Regression verdict.** The new guard rejects the demonstrated stale-input paths
before metadata or a misleading binary is produced. Permitted ordinary archive
replacement selects a new policy/output and executes the new value. Focused
build-policy suite passes 32 tests with one expected skip; full Python passes
110 tests with five expected skips, using the accepted production release for
both binary environment variables. Commands and logs are recorded in
`07-final-review/fix/final-fix-report.md`. Exact standard-policy reconciliation
covers 28 selections: v2/default and explicit v1 across debug/release/fast,
simulator/test and normal/payload settings, plus separate standard native release
selections. Actual core/module preprocessor macros agree; native driver macros
also agree where enabled. Resolved policies, compile/link argument vectors and
Make output paths differ only by the verified helper source digest and its
recomputed policy key/derived leaf. All other tracked simulator/build inputs are
unchanged. This supports reuse of the existing measured artifacts; it is a
simulator-source and guard-policy proof, not a new simulator/native build.

**KIPS before/after.** No new speed claim or timing run. The measured release
`ebd0abafe640c83144c09b5579dce51846f46a9a9e41644dc474aaa3a33af1d8`
and fast `467161d7aaef15cf2fb70eae0b0a4fbc1c446538eda318af1f7f33c7c7ee5275`
artifacts remain unchanged, as do the published local release alias and native
library. Earlier incremental timing tables retain their original scope.

**Evidence and limits.** New receipts and actual commands are under
`07-final-review/fix/`, especially `red-stale-input-proof.json`,
`green-stale-input-proof.json`, `reconciliation.json`, and the final fix report.
`07-final-review/final-rulings.md` records the follow-up ruling;
`06-closeout/` and the original ledger receipts remain immutable. Final scoped
re-review remains the controller's gate. Existing limits remain: unknown external
library ISA and unresolved compiler/system library identity, unvalidated Darwin/
Arm hardware, synthetic older-GCC fallback coverage, ETH codec rather than full
SPEC/KIPS coverage, six cancelled node allocations, and native evidence from its
preserved earlier helper reconciled through the recorded guard changes. M1's
GCC 13 O3 test-183 diagnostic remains unsuppressed and nonblocking; the existing
focused sanitizer result found no invalid memory access.
