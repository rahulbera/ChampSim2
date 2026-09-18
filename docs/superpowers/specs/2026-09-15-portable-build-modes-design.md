# Portable simulator build modes

## Context and scope

The user approved the direction of explicit debug, release, and fast builds,
then clarified that ChampSim runs on heterogeneous x86 compute clusters and
AWS Graviton machines, including c7g, c8g, and sometimes c9g instances. They
build separately on each cluster. CPU portability within each architecture and
the documented minimum ISA is a requirement for every standard build mode,
including fast. The user subsequently requested x86-64-v2 as the x86 minimum;
the compute-node audit below supports that choice for the current ETH CPU fleet.

The build-system and assertion changes described here are implemented on
`feat/perf-fix`. The [build-optimization campaign](../../research-log/Performance/2026-09-15-build-optimization.md)
records the accepted x64 policies, rejected profile trials, exact artifacts,
validation evidence, and remaining platform limits.

There will be separate native Linux x86-64 and little-endian AArch64 binaries.
One native executable cannot serve both instruction sets. Building against
each cluster's toolchain and libraries avoids requiring a single binary package
to span different cluster operating systems. Within a cluster, compute nodes
must still supply a compatible runtime and required shared libraries.

## Standard targets

| Target | Optimization and symbols | ChampSim assertions |
| --- | --- | --- |
| `make debug` | `-O0 -g3`, preserve frame pointers | Enabled |
| `make release` | `-O3 -g3` | Enabled |
| `make fast` | `-O3 -g3` | Disabled |

Plain `make` remains release, retaining the existing `bin/champsim` workflow.
Explicit modes also have distinct outputs, so all three binaries can coexist.
Symbols remain available for profiling and failure analysis. Verbose simulator
logging, sanitizers, LTO, and PGO are independent options or later experiments;
none is implicitly enabled by these initial targets.

The initial standard targets use the following GCC instruction-set baselines:

| Compiler target | Baseline flags |
| --- | --- |
| Linux x86-64 | `-march=x86-64-v2 -mtune=generic` |
| Linux little-endian AArch64 | `-march=armv8-a -mtune=generic` |

Provide an explicit `X86_ISA=x86-64` fallback for older x86 machines. The default
is `X86_ISA=x86-64-v2`; these are the two supported x86 values in the initial
implementation. Reject an explicitly supplied x86 ISA option for an Arm target.
The selection applies equally to debug, release, and fast, participates in
artifact isolation and dependency preparation, and appears in build provenance.
Default x86 binaries are portable across v2-capable hosts, not every historical
x86-64 processor. Other clusters must meet the selected minimum or use the
fallback. This is a declared minimum, not automatic tuning to the build host.

Determine architecture from the selected compiler's target, not the login
machine's CPU. Validate equivalent flags for other supported compilers. Fail
clearly for unsupported targets instead of silently guessing. Standard builds
must not acquire `-march=native`, `-mcpu=native`, AVX2/AVX-512, or SVE requirements
from environment flags, response files, compiler defaults, or dependency builds.
Detect conflicting effective options and report them. Compile and link options
must agree, including any future LTO path.

The Arm baseline includes its standard floating-point and Advanced SIMD support;
portability does not mean disabling all vectorization. Runtime-dispatched library
implementations are acceptable when they retain a baseline-compatible fallback.

x86-64-v2 adds CMPXCHG16B, LAHF/SAHF, POPCNT, SSE3, SSSE3, SSE4.1, and SSE4.2
over the original x86-64 baseline. It does not require AVX or AVX2. Choosing v2
enables additional instruction selection and vector operations; it does not
establish a KIPS improvement. The original baseline still permits the compiler's
`-O3` optimizations, so ISA selection and optimization level are distinct axes.
Measure the v1-to-v2 change with assertions and optimization level held fixed
before attributing any later release-to-fast gain to assertion removal.

## CPU-specific alternatives

Three approaches were considered:

1. Portable modes with a declared minimum ISA: the initial implementation, with
   x86-64-v2 by default and an explicit v1 fallback. This serves the user's
   deployment pattern and keeps assertion measurements independent of ISA.
2. Explicit specialized binaries: a later experiment could add AVX2 or a named
   CPU profile, with visibly different output names and recorded requirements.
   These would not carry the standard portability guarantee.
3. Runtime dispatch inside a portable binary: a future measured hotspot could
   have a baseline implementation and an optional specialized implementation
   selected using CPU and operating-system support. This requires separate
   correctness and dispatch testing and is outside the initial change.

Defer the previously proposed `fast-avx2` target. Do not compile the whole
simulator for AVX2 and assume that checking CPU support at startup makes it
portable. Do not select a newer Graviton instruction set simply because the
build happens on a newer instance. The conservative Arm baseline also leaves
room for other AArch64 hosts beyond the currently named AWS instances.

## Assertion and error policy

Introduce `CHAMPSIM_ASSERT` with an explicit build-wide enable definition,
independent of accidental `NDEBUG` settings. An enabled assertion evaluates its
condition once and reports the failed expression and source location. A disabled
assertion does not evaluate its condition. Required state changes must never be
inside asserted expressions. Keep valid constant evaluation working, including
the assertion in `dynamic_extent`, and leave `static_assert` enabled.

Audit ChampSim-owned assertion sites before migration. Decoder errors and failed
resource initialization in `inc/inf_stream.h`, for example, need ordinary error
handling that remains active in fast mode. Configuration validation, trace I/O
errors, backend initialization errors, and other operational failures must retain
useful diagnostics. Removing invariant checks must preserve simulation behavior
on valid inputs; invalid inputs must not silently become accepted experiments.

The helper must remain usable by standalone tools that include ChampSim headers
without vcpkg or the simulator's link dependencies. Do not mechanically rewrite
vendored predictors or third-party libraries. Their assertion policy is a
separate, explicitly reported dependency property.

## Build isolation and dependencies

Separate objects, dependencies, test binaries, and generated build metadata by
mode, compiler target, and incompatible build options. Account for the trace
memory-value layout switch and optional native backend. Module registry
generation and the generated makefile must not redirect an explicit mode back
into another mode's output directory. Switching or building modes side by side
must not require cleaning or reuse incompatible objects. Compiler/flag changes
must invalidate the corresponding artifacts.

Select dependencies for the compiler target explicitly; never choose an
arbitrary first directory when both x64 and arm64 vcpkg installations exist.
Apply or verify the baseline policy for compiled dependencies, including
compression libraries, as well as simulator sources and modules. An external
library's unknown CPU requirements cannot be inferred from ChampSim's flags.

The native Ramulator helper currently builds CMake Release with a shared library
output in the Ramulator source root. Preserve and report that dependency build
policy initially; the ChampSim debug/release/fast names describe ChampSim code.
Propagate and fingerprint the portable architecture policy for the native build.
Retain the existing ABI and library provenance checks. Do not allow one build
to replace a native library used by another running simulation; native roots
must remain isolated when builds can replace their libraries. Matching debug
and assertion modes throughout Ramulator is a separate extension.

Record build mode, assertion policy, compiler/version/target, effective flags,
and relevant dependency provenance. Expose build information for users and
benchmark manifests. The existing statistics `build_id` hashes the simulated
configuration; do not redefine it as the binary identity or make configuration
replay depend on optimization mode.

## Validation and performance evidence

Validate in stages, preserving a build of the pre-change source and its flags:

1. Test actual compile commands, mode switching without cleaning, simultaneous
   distinct outputs, flag invalidation, architecture selection, incompatible
   overrides, and explicit dependency selection. Exercise GCC and Clang where
   supported. Keep tests isolated from production objects.
2. Test enabled/disabled assertion semantics, constant evaluation, and the
   standalone tools. Run existing unit and Python tests as appropriate, keeping
   the test framework's checks active in every tested simulator mode. Verify
   operational error diagnostics in fast as well as release/debug.
3. Establish portable release parity with the pre-change simulator, then compare
   release and fast. Use legacy DRAM and unchanged model settings throughout.
   For the long gate, run one trace per SPEC26 workload with 5M warmup and 50M
   simulated instructions, retaining the existing additional mcf control.
   Compare deterministic simulation statistics, excluding only declared timing
   and build-provenance fields. Investigate differences rather than broadening
   exclusions to hide them.
4. On representative older and newer x86 compute nodes, execute the same binary
   built for that cluster. On available Graviton generations, do the equivalent
   with the cluster's portable AArch64 binary. Test real simulation and trace
   decompression, not just executable startup. Verify cross-architecture trace
   interpretation and matching simulation statistics; compilation alone is
   insufficient. Do not call an untested generation validated.
5. Measure KIPS with paired repeated runs on the same allocated CPU, without
   overlapping builds or other agent experiments. Report release-to-fast gains
   separately from earlier source optimizations. Report results per host; raw
   KIPS from different CPUs does not isolate the benefit of build flags.

Retain commands, compiler and library identities, CPU/OS details, binary hashes,
traces, configuration, results, and timing data. Log implementation sections,
files, commit hashes, parity verdicts, and before/after KIPS in
`docs/research-log/Performance/2026-09-15-build-optimization.md` as requested for
the new campaign. The preceding source-optimization log remains unchanged.
That log must distinguish prospective validation from completed evidence.

The user authorized starting the campaign with v2, followed by progressive v3
and resolved ETH-headnode-native experiments. Each candidate must pass the same
regression and performance gate before adoption. Record native expansion and
destination-node compatibility explicitly; the portable default remains v2.

## Cluster reconnaissance: 2026-09-15

Initial read-only SSH through the user's `kratos2` alias succeeded. The endpoint reported
hostname `safari-proxy`, architecture `x86_64`, Slurm 21.08.5, GCC 11.3.0, and
glibc 2.35. These are login-environment observations, not compute-node guarantees.
`sinfo` listed 19 nodes in `cpu_part`; their advertised feature fields were null.
CPU count and partition membership do not establish processor generation or ISA.

No simulator build, benchmark, or Slurm job was run during that initial login
reconnaissance. The user's subsequent v2 question prompted a compute-node audit
through Slurm, requesting one CPU and 64 MiB per node, with a one-minute time
limit and a ten-second immediate-allocation limit. The successful audit was job
`14547781` and returned exactly the expected 19 distinct `cpu_part` nodes:

| Processor | Nodes | v2 supported | Loader also reports v3 |
| --- | --- | --- | --- |
| Intel Xeon Gold 5118 | 8 | Yes | Yes |
| Intel Xeon Gold 6226R | 9 | Yes | Yes |
| AMD EPYC 7742 | 1 | Yes | Yes |
| AMD EPYC 9554 | 1 | Yes | Yes |

The probe checked the intersection of flags across every logical CPU listed in
each node's `/proc/cpuinfo` for the additional v2 requirements. It also executed
the node's system ELF loader with `--help` and checked its supported ISA levels.
Every node had all required additional v2 flags and loader-reported v2 support.
v3 is consequently a reasonable later experiment for this CPU partition, but
does not become the default or establish compatibility with uninspected hosts.

Evidence is under
`/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-build-portability/eth-isa-audit-compact-20260915T111617.630091Z/`:
`probe.py`, `command.json`, `stdout.jsonl`, `stderr.txt`, and `summary.json`.
An earlier verbose attempt completed remotely but interleaved output from
different nodes, preventing complete JSON parsing. Its raw output is preserved
in sibling `eth-isa-audit-20260915T111449.287423Z/`. The successful rerun retained
the same checks and shortened each emitted record; no failed record was counted
as a pass.

This is an ISA availability audit, not a ChampSim simulation, behavior-parity
test, or KIPS measurement. Simulator validation must still cover representative
generations using Slurm allocations. GPU and bio partitions were not inspected.
AWS access details were not provided, and no AWS machines have been inspected
or validated for this change. Do not use the login node as a benchmark host.

## References

- [Scarab's pinned build modes](https://github.com/litz-lab/scarab/blob/0a628226b10e7ec020667016c11dd8fbad3fc1d0/src/CMakeLists.txt): its AVX-labelled mode uses
  `-march=haswell`, which is broader than enabling AVX2 alone.
- [gem5 build definitions](https://github.com/gem5/gem5/blob/stable/SConstruct):
  distinguishes debug, optimized with debugging, and fast with checks removed.
- [GCC 13 x86 options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/x86-Options.html):
  distinguishes the instruction-set requirements of `-march` from `-mtune`.
- [x86-64 psABI microarchitecture levels](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex):
  specifies the cumulative v2/v3/v4 instruction requirements and OS enablement.
- [GCC 13 AArch64 options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/AArch64-Options.html):
  defines Arm architecture baselines, tuning, and optional feature controls.
- [AWS C/C++ guidance](https://github.com/aws/aws-graviton-getting-started/blob/main/c-c%2B%2B.md):
  recommends choosing requirements for the oldest deployment generation and
  describes conservative Armv8-A builds with runtime feature detection.
