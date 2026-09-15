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

Every simulation in this campaign explicitly uses `dram-model=legacy`; builds
for performance experiments use `WITH_RAMULATOR2=0`. Primary timing uses detailed
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
member. AWS hardware validation remains dependent on an available ARM host.

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
closeout: `4bc0f045`. Campaign setup commit will be recorded after creation.

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
setup). No production implementation commit yet.

**Regression verdict.** Pending: candidate build, short matrix, long gate, and
test verification have not completed.

**KIPS before/after.** Pending fresh paired measurements. No gain claimed.

**Status.** Trial preparation in progress; not yet accepted into production.
