# Ramulator2 integration: proposed design and delivery plan

Status: approved by the user on 2026-09-13 for autonomous implementation.

Inspected baselines:

- ChampSim: `79cb5fdb2abd7e754b529eb235c6a69f61e45ea8`.
- Local Ramulator checkout: `/home/rbera/work/alakazam/ramulator2`, revision
  `72427a1bba3771564c4fb0e494ba02242fd1eaa7`. This is **Ramulator 2.1**.

The user has confirmed that Ramulator will be configured by a native YAML file
referenced from ChampSim TOML, and that selecting Ramulator must reject legacy
`pmem.*` settings. The architecture and staged delivery plan below are approved.

## 1. Scope and compatibility contract

One Ramulator-enabled ChampSim executable supports both memory models. A root
runtime key selects the model:

```toml
dram-model = "legacy" # default when omitted
```

```toml
dram-model = "ramulator2"

[ramulator2]
config = "configs/ramulator2/ddr4.yaml"
```

The equivalent overrides are `--set dram-model=legacy` and
`--set dram-model=ramulator2`. Existing command-line ordering applies. Unknown
model names fail before simulation. No silent fallback is allowed when Ramulator
was explicitly requested but was not built, cannot load, or has an invalid config.

Legacy compatibility means:

- Omitting the selector and explicitly selecting `legacy` produce identical
  simulation results, effective configuration, and machine identity.
- Existing TOML configurations and previous legacy result files remain usable.
- Legacy timing, address mapping, queues, warmup, statistics, and module behavior
  remain numerically identical to the inspected baseline, at one and two cores.
- The selector is recorded in effective configuration. This intentional metadata
  addition changes historical `build_id` values; matching historical hashes is
  not the compatibility criterion. Numeric results are compared independently.
- A build without Ramulator remains available and has no Ramulator dependency.

This work models DRAM timing. It does not add functional backing storage, route
trace-v2 physical addresses into the hierarchy, change the CPU model, or change
cache-line size. ChampSim continues to supply translated physical requests and
carry its existing response data tokens.

## 2. Approaches considered

| Approach | Consequences | Decision |
| --- | --- | --- |
| A small backend interface, unchanged legacy controller behind an owner, and a separate Ramulator adapter | Localizes native dependencies and preserves direct legacy unit tests and clock behavior | Recommended |
| Add backend branches and Ramulator state inside `MEMORY_CONTROLLER` | Fewer initial interface edits, but mixes two controllers and encourages assumptions that every backend has legacy channels/statistics | Do not pursue |
| Load a separate adapter plugin through a versioned C ABI | A single executable can run legacy without loading Ramulator; adds loader, packaging, ABI, and lifetime machinery | Defer unless deployment requires this |

The recommended initial dependency arrangement is an explicitly configured,
pinned external Ramulator checkout and its upstream shared library. Vendoring or
adding a submodule can follow independently if repository distribution requires it.

## 3. Backend boundary and ownership

Introduce `champsim::memory_backend`, an owning interface with four responsibilities:

1. Return the selected `operable&` to the simulator.
2. Report immutable physical capacity in bytes.
3. Return owned copies of current simulation and ROI memory statistics.
4. Perform one-time finalization after all simulation phases.

`legacy_memory_backend` owns the existing `MEMORY_CONTROLLER` and returns that
controller itself as the operable. It forwards capacity and copies existing
channel statistics. It adds no clock, request queue, timing transformation, or
extra call to `operate()`.

`ramulator2_memory_backend` exposes one ChampSim operable and owns a private
Ramulator driver. Its public headers use only ChampSim/C++17 types. Ramulator
headers and native objects stay in a private C++20 translation unit.

`static_environment` owns channels, the selected backend, vmem, PTWs, caches, and
cores in that order. The backend is created once, before vmem. Channel storage
remains stable and outlives backend callbacks. `operable_view()` retains the
existing order and includes exactly one memory operable.

Replace the runtime dependency on `environment::dram_view()` with a memory-backend
view. The phase runner obtains snapshots through the interface rather than
copying a `MEMORY_CONTROLLER`. Existing direct controller tests continue to use
the concrete legacy types.

`VirtualMemory` needs capacity, rather than a controller reference. Store capacity
by value and add capacity-taking constructors. Retain its existing
`MEMORY_CONTROLLER&` constructors as delegating overloads so existing users and
tests still compile.

## 4. Native configuration, geometry, and identity

Load the YAML after command-line parsing, during selected-backend construction.
Read it with ChampSim-owned file/error handling and call
`Ramulator::Config::parse_config_string()`. The native file parser calls
`std::exit()` for a missing file, which would bypass ChampSim diagnostics.

Use fully expanded Ramulator 2.1 exports. C++ does not resolve Python presets or
implement the older Ramulator 2.0 configuration format. Ship both a readable
Python configuration and its exported YAML; the simulation itself needs no Python.

Relative paths resolve against the invocation working directory. Record the
absolute normalized path as the effective `ramulator2.config` value, preserving
the original supplied spelling in override provenance. Generalize the runtime
store's effective-value override to support strings as well as numeric values.
Do not change process working
directory while constructing the backend; plugin paths retain their documented
Ramulator semantics.

Confirmed inactive-key policy: supplied legacy `pmem.*` keys are rejected for
`ramulator2`, with a diagnostic explaining that memory settings belong in its
YAML. Ramulator keys are similarly rejected for `legacy`. This preserves the
current strict unused-key contract. Existing complete machine configurations can
be split into a shared core/cache file and a backend-specific file.

The initial supported embedding uses `GenericDRAM` and flat physical addresses.
Controller, DRAM, refresh, scheduler, row-policy, and plugin choices remain native
YAML settings. Validate the assumptions that its native top-level API does not
enforce: channels must have the same clock period and transaction size. Initially
require equal channel capacity and `CacheLineInterleave`, giving an unambiguous
contiguous physical range; report unsupported channel mappers explicitly rather
than claiming that arbitrary native address-vector frontends work.

Derive capacity from each controller's resolved DRAM specification and sum the
channels. In terms of the address mapper's geometry, per-channel usable capacity
is the product of organization counts, divided by internal prefetch size, times
`get_tx_bytes()`. Use checked arithmetic, positive dimensions, and exact division.
This accounts for models with a payload-size override, such as HBM3; multiplying
raw channel width alone is insufficient. Validate capacity against the mapped
address range, ChampSim's reserved first MiB, page size, and Ramulator's signed
address representation. Do not create a dummy legacy controller to obtain capacity.

Configuration identity must include native input contents, not just a pathname:

- Record a deterministic content hash as an effective `ramulator2.config_hash`
  value, using the project's existing stable hashing convention.
- When a replayed configuration supplies this hash, verify it against the file
  and reject a mismatch before simulation. A normal hand-written config only
  needs the `config` pathname; the computed hash becomes its fallback value.
- Record the native revision and integration build information in metadata, and
  include the pinned native revision as an effective `ramulator2.revision` value.
  When a replay supplies this value, reject a mismatch with the linked build.
- Archive the exact YAML text as a scalar metadata value for recovery/auditing.
  The initial replay interface still requires the referenced YAML file to exist
  with matching contents. It does not promise to reconstruct missing files.
- Replaying a result and then intentionally changing native memory configuration
  requires a fresh backend config without the old expected hash.

`--knobs` lists `dram-model` and the backends compiled into the executable.
Selecting Ramulator for this probe requires its YAML, because geometry and period
come from that file. No inactive backend is constructed to enumerate its knobs.

## 5. Clocking, warmup, and phase lifecycle

Derive the Ramulator period from its resolved integer `tCK_ps`; cross-check it
against the public `get_tCK()` nanosecond value. Avoid truncating a float such as
0.625 ns into the wrong picosecond period. The adapter submits requests, then
calls `memory_system->tick()` once per memory operation. The native standalone
frontend/memory `clock_ratio` values do not multiply ChampSim's clocks.

Refactor `static_environment::time_quantum` to combine the already constructed
backend's period with the non-memory frequency sweep. Vmem's cycle-denominated
minor-fault penalty must use this same minimum. Preserve the legacy frequency
conversion and operable ordering exactly. Do not construct a second Ramulator
instance to discover its period.

Recommended warmup is ChampSim's existing fast-memory policy: return warmup reads
and discard warmup writes at the memory operation, without issuing them into
Ramulator's transaction queues. Continue native clock ticks so its time and
maintenance state advance. Caches and translation warm normally; this policy does
not train DRAM row buffers with warmup accesses. A separate timed-warmup mode is
outside this first integration.

At `begin_phase()`, reset adapter counters and call native recursive statistic
reset, retaining clocks, queues, DRAM state, and live completion contexts.
At `end_phase(cpu)`, update and copy statistics. Match legacy shared-memory
semantics: the last finishing CPU supplies the final shared ROI snapshot.

Do not drain or finalize memory at a per-CPU or phase boundary. The current
simulator keeps instructions and requests in flight between phases. After the
final phase, capture the result and finalize native components once to flush
plugin outputs. Do not run an extra measured drain; final destruction releases
outstanding contexts without emitting late ChampSim responses.

Native plugins retain their own reset/finalization contracts. Adapter ROI counters
are authoritative for integration traffic. An opaque plugin snapshot whose
implementation does not reset must not be advertised as an ROI counter.

## 6. Request transport and callbacks

Keep the existing single shared LLC-to-memory channel. It is currently unbounded:
controller rejection retains requests in that feeder; it does not create finite
queue pressure back to the LLC. Preserve this on both backends in the initial
integration. A new finite LLC/memory queue would be a separate model change.

Process RQ, then PQ, then WQ, retaining FIFO order within each queue:

- RQ and PQ generate Ramulator reads, including RFO/translation traffic arriving
  through that route. WQ generates Ramulator writes.
- Align the submitted physical address to ChampSim's block boundary. Keep the
  original ChampSim packet address and all return metadata in an owned context.
- Pass cache-block requests, rather than trace operand widths. No trace payload
  bytes need to cross the native interface.
- A rejected submission stays pending and retries after subsequent memory ticks.
  Never pop an unaccepted request or resubmit an already accepted fragment.
- Allocate a unique parent request identity. Do not use address as its identity;
  repeated same-address reads can coexist.

Support transactions smaller than `BLOCK_SIZE` through fragmentation. Query
`get_tx_bytes()` and require a positive power of two. Split a block into aligned
fragments of at most that size; a larger native transaction can accept a whole
block that fits within its boundary. For 64-byte blocks and 32-byte transactions,
issue two reads and return one ChampSim response after both complete.

Each queue has at most one partially submitted head context. Keep its original
head until all fragments have been accepted; accepted fragments may complete
before the remaining fragments can be submitted. Context state tracks accepted
and completed fragments separately. Preserve program-visible queue ordering and
bounded adapter bookkeeping while allowing native outstanding requests.

Callbacks can run synchronously inside a successful write submission. Prepare
context state before the call, roll back provisional state on rejection, and
queue completions for adapter processing rather than erasing the active context
reentrantly. Test immediate completion of the last fragment explicitly.

On read completion, return the original address, virtual address, data token,
prefetch metadata, and dependency list to the original channel. Honor
`response_requested=false` while still releasing context state. Track writes
until their native callbacks for ownership/accounting, without sending unsolicited
writeback responses. Native write callbacks mean command issue or coalescing,
not completion of a physical write burst.

Leave DRAM read/write forwarding and coalescing to Ramulator for this backend;
do not copy legacy scheduling/forwarding logic into the adapter. The legacy
wrapper preserves its existing forwarding and response quirks unchanged.

Use a small private Ramulator frontend implementation that reports
`NUM_CPUS`, forwards external requests, and performs no CPU simulation. The
exported YAML must specify the `External` frontend; the adapter substitutes its
compatible ChampSim frontend at construction. Ramulator's supplied `External`
reports one core and its controller indexes per-core arrays without bounds checks,
so forwarding `cpu=1` to that frontend would be unsafe. Validate source IDs and
retain their core identity in the shim.

Count accepted transactions and completed operations as progress. A bare native
tick must not defeat ChampSim's deadlock detection. Diagnostics report outstanding
parents/fragments, queue lengths, and time since last completion.

## 7. Statistics and reporting

Use `dram_stats` directly in legacy phase snapshots rather than referring through
`DRAM_CHANNEL::stats_type`. Preserve all legacy fields, values, names, and printer
behavior, including the compiled JSON printer.

Ramulator results contain two distinct sets of data:

- Adapter counters: accepted/completed parent reads and writes, accepted/completed
  fragments, rejected submission attempts, outstanding parents/fragments, and
  exact sums/counts for read latency in picoseconds. Counters use 64-bit storage.
- Native snapshots from `update_stats_recursive()` and `collect_stats()`, retaining
  native names and definitions for controller, refresh, scheduler, and plugin data.

Preserve legacy result documents at schema version 1, with the additive selector
in `[config]`. Ramulator result documents use schema version 2, declare the memory
backend, and place adapter and native data under
`phase.<phase>.<roi|sim>.ramulator2`. CPU/cache reporting remains the same. The
configuration loader accepts both versions. Existing consumers of legacy output
therefore retain their schema; Ramulator-aware consumers opt into the new one.

Serialize native collections as deterministic named/indexed tables of scalars,
with lossless key escaping and stable controller identities at one and multiple
channels. Retain a raw native YAML snapshot for values that need native
interpretation. Do not populate unsupported legacy bus-congestion fields with
zero or equate differently defined native counters with legacy ones.

Extend plain text and TOML reporting together and keep the JSON translation unit
building. Snapshots own their values; they hold no references into destroyed
Ramulator objects.

## 8. Build and dependency integration

Retain the existing Makefile and module-discovery flow. Add an explicit build
option such as `WITH_RAMULATOR2=1` plus `RAMULATOR2_ROOT`; do not bake the developer's
absolute checkout path into tracked files. A helper target can invoke:

```sh
cmake -S /path/to/ramulator2 -B /path/to/ramulator2-build \
  -DCMAKE_BUILD_TYPE=Release -DRAMULATOR_PYTHON_BINDINGS=OFF \
  -DCMAKE_CXX_COMPILER=/path/to/matching-cxx
cmake --build /path/to/ramulator2-build --target ramulator
```

The inspected upstream target writes its shared library into the Ramulator source
root, even with an external build directory. Its FetchContent dependencies are
yaml-cpp `yaml-cpp-0.9.0` and fmt `10.2.1`; record immutable resolved revisions for
reproducible builds. Preserve the upstream shared target so registration-only
objects remain linked. Ordinary static archive linking would need extra care.

Only the private native driver and frontend translation unit require C++20.
Apply their standard/include options to both compilation and dependency
preprocessing: ChampSim's existing dependency recipe does not use `CXXFLAGS`.
Do not expose Ramulator headers through module headers or standalone harnesses.

Use matching compiler/standard-library ABI settings. Verify fmt coexistence and
shared-library loading in the initial link smoke test. Add proper build stamps
or distinct objects so changing backend enablement, root, compiler, or dependency
revision cannot silently reuse stale native objects. Default legacy tests must
still build without native headers or the native library.

Use an explicit development RPATH and document a relocatable packaging layout.
A directly linked Ramulator-enabled executable needs the shared library even
when it selects `legacy`; the legacy-only executable does not. Runtime loading
is a future packaging option, rather than an initial requirement.

## 9. Staged delivery plan

These stages define reviewable deliverables. A detailed implementation checklist
with exact code/test steps follows approval of this design.

### Stage 1: Establish the legacy baseline and isolate memory ownership

Create `inc/memory_backend.h`, `inc/memory_stats.h`, `src/memory_backend.cc`, and
`src/legacy_memory_backend.cc`. Initially the owned memory snapshot contains only
the existing legacy statistics; later stages extend it with adapter/native data.
Modify `inc/environment.h`, `inc/static_environment.h`,
`src/static_environment.cc`, `inc/vmem.h`, `src/vmem.cc`, and `src/champsim.cc`
to use the boundary. Replace legacy statistic alias spellings with `dram_stats`
in `inc/phase_info.h`, `inc/stats_printer.h`, and printer definitions so those
interfaces no longer depend on a concrete controller type. Add the selector with
`legacy` available and a clear unavailable-backend error for `ramulator2`.

Acceptance: existing C++/Python suites pass; default and explicit legacy match
the parent revision on one- and two-core traces. Construction order, component
count, time quantum, vmem mapping, and legacy statistics stay unchanged.

### Stage 2: Build and construct a native memory system

Add `inc/ramulator2_driver.h`, `src/ramulator2_driver.cc`, build integration in
`Makefile`, and versioned fixtures in `configs/ramulator2/`. The driver header
provides a private C++17-facing interface; its implementation owns native types
and the frontend shim. Resolve and validate YAML, period, capacity, transaction
size, native registration, and core count.

Acceptance: legacy-only and enabled executables build; an enabled executable
can construct both backends; malformed/missing YAML, unavailable components,
invalid geometry, and mixed clocks produce startup diagnostics. A native
64-byte read completes through the embedding API. No native frontend CPU runs.

### Stage 3: Connect ChampSim traffic and lifecycle

Add `inc/ramulator2_memory_backend.h`, `src/ramulator2_memory_backend.cc`, and
factory wiring. Implement request contexts, retries, fragmentation, callbacks,
warmup bypass, counter reset, snapshots, diagnostics, and one-time finalization.
Keep a small fake-driver seam for deterministic adapter tests without native
timing dependencies.

Acceptance: test refusal/retry, partial acceptance, immediate write callbacks,
out-of-order fragment completion, repeated addresses, read-after-write traffic,
suppressed responses, metadata retention, source IDs, and phase carry-over.
Exercise transaction sizes of 32, 64, and 128 bytes. No dropped, duplicated, or
prematurely completed parent requests are allowed.

### Stage 4: Record memory identity and backend statistics

Extend `inc/memory_stats.h` and update `inc/phase_info.h`, `inc/stats_printer.h`,
`src/main.cc`, `src/toml_printer.cc`, `src/plain_printer.cc`, and
`src/json_printer.cc`. Update `inc/runtime_config.h` and `src/runtime_config.cc`
for effective string values and native identity/replay as needed. Implement
source/hash recording, schema selection, native
snapshot conversion, and useful plain-text memory summaries.

Acceptance: legacy printer fixtures remain unchanged; Ramulator TOML parses;
all counter units and denominators are explicit; `--config run.toml` reproduces
the same backend/configuration with matching YAML, and rejects modified YAML.
An unknown or inactive setting is diagnosed according to the approved policy.

### Stage 5: Validate the integrated model and document its use

Add focused tests alongside the existing 700-series memory tests and extend
`501-static-environment.cc`, `098-runtime-config.cc`, and `099-toml-printer.cc`.
Update `configs/README.md`, `README.md`, `CLAUDE.md`, and CI workflows.

Acceptance includes all of the following:

- Unchanged legacy DRAM, PTW/vmem, core/cache, module-discovery, and printer suites.
- Parent revision versus new default versus explicit legacy numeric comparisons,
  at one and two cores, with predictor/module selections and traces pinned.
- Exact native send/tick/callback-stream comparison against a minimal native
  replay driver under the same YAML, including rejected attempts and submission
  order. Compare callback cycles and controller counters, not IPC alone.
- A DDR4 64-byte-transaction case, an LPDDR5 or HBM3 32-byte-transaction case,
  multiple memory channels, and a two-core run.
- Core/cache and DRAM frequencies varied independently, checking that DRAM
  completion times and vmem fault penalties use their intended clocks.
- ROI reset with requests in flight, staggered multicore completion, and no
  measured drain after the last retired instruction.
- Address/undefined-behavior sanitizers on the new adapter with known-clean
  predictor modules, especially callback ownership and core-index handling.
- Legacy-only CI on existing compilers and an enabled job on a C++20-capable
  compiler, with pinned native sources and deterministic local trace fixtures.

Do not require the two DRAM models to produce the same IPC or DRAM latency.
Equivalence is required between old and new **legacy** runs. Ramulator correctness
is established against native behavior and the adapter's transport contract.

## 10. Source evidence and verification limits

The proposal is based on code inspection, not a completed native build or runtime
experiment. Dependency fetching, binary ABI compatibility, and actual native
timing behavior remain explicit implementation-stage checks.

Key inspected sources:

- ChampSim `src/static_environment.cc`: channel construction, memory geometry,
  time quantum, component ownership, and runtime module selection.
- ChampSim `src/dram_controller.cc`: queue-prefix acceptance, fast warmup,
  channel clocking, forwarding, and phase statistics.
- ChampSim `src/champsim.cc`, `src/operable.cc`, and `src/vmem.cc`: clock order,
  phase boundaries, and capacity-dependent physical-page allocation.
- Ramulator `README.md`, section 7; `src/ramulator/base/config.{h,cpp}`:
  expanded native YAML and pure C++ embedding.
- Ramulator `src/ramulator/memory_system/impl/generic_dram_system.cpp`:
  send validation, channel routing, tick, transaction size, and clock reporting.
- Ramulator `src/ramulator/controller/controller_base.cpp`: synchronous
  coalesced-write callbacks, write completion semantics, per-core indexing,
  read completion, and statistic reset.
- Ramulator `src/ramulator/dram/dram_spec.h` and
  `src/ramulator/controller/addr_mapper/addr_mapper_base.cpp`: payload transaction
  size, organization, and mapped physical range.
- Ramulator `resources/gem5_wrappers/ramulator2_base.cc`: working lifecycle,
  embedding, callback, snapshot, and finalization example.
- Ramulator `CMakeLists.txt`: C++20, shared library, dependency pins, and optional
  Python bindings.
