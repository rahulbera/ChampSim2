# Ramulator2 backend reference

The operating manual for the optional native DRAM backend: what the build
verifies, which native components are admitted and why the rest are refused, what
the adapter guarantees, the enforced counter limits, the sanitizer mode, the
statistics schema, and the bandwidth procedure.

This was section *Optional native DRAM backend* of `CLAUDE.md` until it outgrew a
file that is loaded into every session. `CLAUDE.md` keeps the entry conditions and
the hazards that cost a day if you trip them, and points here for the rest. Read
this before changing anything native.

Related: [integration writeup](ramulator2-integration.md) (architecture, review and
close-out evidence, open weak points), [validation record](ramulator2-validation.md)
(evidence and limitations), [bandwidth sweep report](ramulator2-bandwidth-sweeps.md)
(tuning procedure and best practices).

## 1. Build, pinning and provenance

The runtime root key `dram-model` defaults to `legacy`; `ramulator2` requires a
build with `WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/absolute/native/root`. Use the exact
Ramulator 2.1 revision `72427a1bba3771564c4fb0e494ba02242fd1eaa7`, Linux/GCC 13,
CMake, Python 3.11+ and PyYAML for the currently verified setup. The helper builds
a pure C++ shared library with `RAMULATOR_PYTHON_BINDINGS=OFF`. Python imports
source-only DRAM metadata during build/export; simulation has no Python runtime.
Native headers remain private to the C++20 driver translation unit. The public
interface, adapter, legacy mode and standalone harnesses remain C++17.

`config/ramulator2_build.py` verifies the clean pinned source, fmt/yaml-cpp
revisions, compiler/options and library fingerprint. Its host/native ABI probe
includes public base/spec/request/config layouts and type identities; response
files and forced includes participate. Incompatible ABI flags fail clearly.
The native product uses Release/C++20 by default, or RelWithDebInfo/C++20 with
`RAMULATOR2_SANITIZE=1`, and receives the selected architecture flags.

A source-root `.champsim-native/manifest.json` and `.champsim-native.lock` let
every build mode and both flavors reuse exactly the same verified library. The
manifest's `inputs` are the root path, native revision, compiler
command/target/version/executable hashes, `architecture_options`, the native mode
string and settings, the pinned dependency revisions and the helper's own hash
(`config/ramulator2_build.py:255`) — deliberately not `BUILD_MODE`, `BUILD_FLAVOR`
or `CXXFLAGS`, which is why CI's non-PIE step reuses one root with
`CXXFLAGS='-fno-pie -no-pie'` and its own `OBJ_ROOT`. A different compiler, a
different `X86_ISA` expansion or a flipped `RAMULATOR2_SANITIZE` is what an
occupied root rejects, along with missing dependency provenance and a replaced
library; use a fresh isolated clone. No build option authorizes replacement.

Mode/compiler/root changes invalidate relevant stamps and dependencies; existing
`.d` edges still apply to `make -n/-q/-t` without executing remakes. Runtime checks
the loaded shared library against recorded provenance, asking the dynamic loader
for the file it loaded as `libramulator.so` (dlopen `RTLD_NOLOAD` + dlinfo). Do not
use dladdr on a native function: in a non-PIE executable its address is a PLT
stub in the program, so the program was fingerprinted and every native run
failed. CI builds a non-PIE binary to keep that fixed.

Concurrent builds against one root are supported: an exclusive `flock` on
`<root>/.champsim-native.lock` serializes preparation, one invocation builds and
the rest reuse the manifest — which is what `make -j6 all test` does through its
per-flavor sub-makes, and what `test/python/test_ramulator2_build.py:159` pins. A
concurrent build whose native policy differs is rejected rather than overwriting
the library. What still corrupts a root is out-of-tree CMake, which writes
`libramulator.so` into the source root: never run one, and never change native
source, against a root in use.

## 2. Admitted native input

`configs/ramulator2.toml` is an example. `ramulator2.config` names a fully expanded
YAML export, relative to the process working directory. Native input supports
External + GenericDRAM + CacheLineInterleave with homogeneous controller
capacity, period and transaction size. Each controller impl must be GenericDDR,
LPDDR5, LPDDR6, GDDR7, HBM12, HBM34 or PRAC, and each addr_mapper RoBaRaCoCh,
ChRaBaRoCo or MOP4CLXOR, or RITAddrMapper over one of those with
`reserved_rows_per_bank` absent or 0. Everything else (BlockHammer casts the
frontend to a type the External shim is not; PassThroughAddrMapper faults at
the first tick; reserved RIT rows make the top of the capacity unreachable) is
rejected before native construction. The DRAM checks still apply to admitted
components. Controller/DRAM family pairing is not validated: a mismatched pair
can be rejected by native code, stall until the no-progress guard aborts (HBM12
over DDR4), or run with the generic controller's semantics (GenericDDR over
LPDDR5). LPDDR6's stock exports have a 12-bit `channel_width`, which the
byte-aligned channel-width check rejects unless the width is edited, which
changes the modeled device. Native mode rejects all `pmem.*` settings;
legacy rejects `ramulator2.*`. The factory resolves one backend, with one operable
and the existing shared unbounded LLC feeder; capacity and clock discovery do not
create a second native instance. A private External shim reports the actual core
count and retains CPU IDs.

## 3. Adapter contract

The adapter owns parent packets and weak-mailbox callback contexts. Submit in
RQ/PQ/WQ FIFO prefixes, retaining partially submitted heads; retry only rejected
fragments. A `PREFETCH` head whose cache block is past native capacity is never
submitted: it is popped, a response-requested read is answered at once (as in fast
warmup), and it is counted in `out_of_range_prefetches` in warmup and measured
phases alike. Stock physical prefetchers (`next_line`, `va_ampm_lite`) can cross
the top physical frame, which legacy aliases instead. Out-of-range
demand/RFO/write/translation requests and invalid CPU IDs still throw. Parent
admission and read latency begin at the first accepted native fragment.
Completion waits for all fragments and preserves every response field;
no-response reads still count, and write callbacks never produce upstream replies.
Fast warmup bypasses new demand submissions while native clocks/refresh tick.
Phase resets clear counters, not native queues/clocks or live contexts. Outstanding
gauges and full latency spans survive resets; carry-over completions can exceed
new acceptances. Each finishing CPU updates an owned shared-memory ROI snapshot.
Finalize once after all phases, without a measured drain or additional ticks.

## 4. Enforced counter and tick limits

The driver refuses, with a runtime error, the native send that would take
GenericDRAM's signed `total_num_read_requests`/`total_num_write_requests` past
2,147,483,647 (per statistics phase, counting native transactions, not cache blocks)
and, when any controller lists AQUA, Graphene, Hydra or RRS, the memory tick that
would take their never-reset `int m_clk` past 2,147,483,647 (1.79 s simulated at 833
ps, warmup included). `champsim::ramulator2_native_limits` is the test seam that
lowers them in 704; there is no CLI knob. Per-row and per-bank signed counters in
other native components were audited and left unenforced; the integration writeup
lists them.

## 5. Tests

Tests 704-708 are what the `native_sanitize` job runs. 706 checks the production
adapter over the real driver against an independent model of its contract: a
non-hidden smoke case runs in every enabled `make test`, and the hidden
`[.differential]` and `[.differential-recovery]` campaigns are run through
`test/ramulator2/oracle_variants.py` and `run_differential.py`
(`--require-all-cores` in a multi-core checkout). `run_mutants.py`, in its own
scratch copy and against a `--native-root` already verified for the same compiler
and ISA policy — or a fresh pinned checkout; `--seed-native-obj` is deprecated and
ignored — must detect every mutant in `oracle_mutants.py` not marked equivalent.
Each mutant's pattern must occur exactly once, so editing
`src/ramulator2_memory_backend.cc` or the driver can fail `test_tools.py` until the
mutant is updated. 707 pins global-clock scheduling against a closed-form
reference; 708 tears down drivers and adapters with live requests.

Portable regressions live in `test/ramulator2`; the enabled CI job uses generated
local traces and the pinned native root, preserving the legacy compiler matrix.

## 6. Sanitizer mode

`RAMULATOR2_SANITIZE=1` (only with `WITH_RAMULATOR2=1`) builds the native library
`RelWithDebInfo` with ASan and UBSan and instruments every host compile and link;
the mode is in the build policy, compiler stamp, manifest and
`meta.ramulator2.build`. Changing it selects separate host objects; the native
root's ownership check requires a separate fresh root for a different mode.
Give it its own native root, and run with the
options and ITTAGE-only suppressions in `test/ramulator2/README.md`. Pinned native
`RITAddrMapper` leaks its nested mapper (2,028 bytes in 20 allocations from 704's
`[rit-addr-mapper]` cases). The `native_sanitize` job runs those cases in a last
step with `test/ramulator2/sanitizers/lsan-rit.supp`, which suppresses only
allocations under `RITAddrMapper::create_base_mapper`. Every new case
that constructs a `RITAddrMapper` controller must carry that tag. LeakSanitizer
never runs on the no-progress `abort()`, SIGTERM or SIGINT. Simulator and test
objects use separate policy directories, so building tests first does not change
the simulator's optimization level. Use the canonical paths from
`make print-build-paths` to retain different modes without sharing an alias.

## 7. Modeling bandwidth

To model DRAM bandwidth natively, override `nBL` on DDR4_2400R (bandwidth about
76,831 x A / nBL MB/s, A 0.95-1.00): export points with
`configs/ramulator2/bandwidth.py` (driven by `make_bandwidth_sweep.sh`) and read
[the bandwidth sweep report](ramulator2-bandwidth-sweeps.md) first: low-bandwidth
runs need `sim.livelock_period` raised and, at very low bandwidth (the default
aborted at about 13 MB/s), `sim.deadlock_cycle`; tCK scaling, smaller payloads and
extra 32-byte-transaction controllers are the wrong knobs; a DDR5 `nBL` override
needs `nRTW` too; and a single core at B/N matches N cores sharing B only on a
saturated channel with identical workloads.

## 8. Channel count

**One controller is one channel.** `org.count[0]` is the channel level and the driver
requires it to be 1 (`src/ramulator2_driver.cc:211`), so channels are never added by
editing the org — you repeat the whole `- impl: GenericDDR` block. The count must be a
power of two (`:374`), every controller must have identical capacity, tCK and
transaction size (`:396`), and total capacity is per-channel capacity times the count
(`:403`). Statistics then appear per channel as `…native.channel0`, `channel1`, ….

`configs/ramulator2/channels.py` (driven by `make_channel_sweep.sh`) exports
DDR4-3200 points at 1, 2 and 4 channels:

```
channels.py [--channels N ...] [--timing-preset {DDR4_3200W,DDR4_3200AA,DDR4_3200AC}]
            [--org-preset NAME] [--iso-capacity] [--interleave-bits N]
            --out-dir DIR [--quantum-ps PS] [--force]
```

It defaults to `--channels 1 2 4` and `DDR4_3200AA` (22-22-22), the middle of the
three rate-3200 bins Ramulator ships (W is 20-20-20, AC 24-24-24; they differ only in
nCL/nRCD/nRP and the dependent nRC). Output is `ddr4_3200aa_<N>ch.yaml` plus a
manifest.csv carrying the channel count, both capacities, per-channel and aggregate
nominal bandwidth, the deadlock guard and the ChampSim `--set` arguments.

Three things are worth knowing before running a sweep.

- **Select the 3200 preset by name; never fake it by overriding `rate` or `tCK_ps`
  on a 2400 preset.** `resolve()` derives the secondary timings *before* it applies
  overrides, so a faked 3200 keeps 2400's refresh and ACT windows — nRFC/nREFI
  421/9363 where 3200 needs 560/12480, and with `rate` overridden too, nRRDL/nFAW
  6/26 where it needs 8/34. The generator selects the preset, and still runs
  `bandwidth.py`'s stale-derived-timing check, which refuses to write a YAML whose
  exported timings disagree with their recomputation.
- **Capacity is a choice, and neither option is a pure control.** By default the
  per-channel device is fixed, so 1/2/4 channels of `DDR4_8Gb_x8` is 8/16/32 GiB —
  physically what adding DIMMs does, but the physical address space, the vmem frame
  pool and `out_of_range_prefetches` all move with the channel count. `--iso-capacity`
  holds the total at the one-channel figure by dropping to a lower-density part per
  channel (8Gb → 4Gb → 2Gb), which fixes the address space but changes tRFC with the
  density. Export both and say which one a result came from.
- **The nominal figure is the data bus, not an achievable rate.** At nBL 4 and
  tCK 625 ps one 64-bit channel is 25,600 MB/s (102,400 / nBL, against DDR4-2400R's
  76,831 / nBL), so 1/2/4 channels is 25.6/51.2/102.4 GB/s nominal. The ACT window —
  nFAW 34 at 625 ps, four activates per 21.25 ns — caps random 64-byte reads near
  12 GB/s per channel, essentially where 2400R already sat, because nFAW is nearly
  constant in nanoseconds across the two rates. Moving 2400R → 3200 buys a third of
  the bus and only a few percent of random-read throughput; **channel count is the
  knob that actually scales it.**

A new YAML is a new `ramulator2.config` hash, so these points are not comparable with
the recorded DDR4-2400R sweep in [the bandwidth report](ramulator2-bandwidth-sweeps.md).

## 9. Statistics schema

Native TOML uses schema 2 and `phase.<name>.<roi|sim>.ramulator2.adapter`/`.native`,
plus owned raw `native_yaml`. Counters are separate 64-bit fields, including
`out_of_range_prefetches` beside `rejected_submissions`; read latency is summed
picoseconds with a sample count. Write completions mean native command
issue/coalescing, not bus drain. Native paths are escaped by component, controller
indices become `channel0`, etc.; native doubles retain precision and values above
TOML's INT64_MAX use exact decimal strings. Legacy schema 1 and formatter bytes
stay unchanged. `meta.ramulator2` records original YAML, canonical path, hash,
revision and library/build provenance. Replay loads `[config]`, checks current
YAML hash/revision in the driver, and preserves original overrides separately.

The `--toml` output-target rules are not native-specific and stay in `CLAUDE.md`,
under the statistics-document section.
