# PGO profiles

The fast build uses GCC profile-guided optimization when a profile for its compiler and
DRAM model is present here. With the integer DRAM mapping, no measured trace was slower with a
profile than without, and most gained 10–29%. Section 14 of the
[LTO/PGO log](../docs/research-log/Performance/2026-09-27-lto-pgo.md) has the measurements.

## Layout

```
pgo/
  train-legacy-dram.json  train-ramulator2.json   what `make pgo-train` runs
  check-legacy-dram.json  check-ramulator2.json   what `make pgo-check` measures
  train-modules.toml                              module overlay for one training run
  legacy-dram/gcc-13.3/   MANIFEST.json + obj#*.gcda
  legacy-dram/gcc-11.3/   ...  (trained on kratos2, the cluster's compiler)
  ramulator2/gcc-13.3/    ...
```

- **One profile per DRAM model.** `legacy-dram` is used by builds with `WITH_RAMULATOR2=0`, and
  `ramulator2` by builds with `WITH_RAMULATOR2=1`. Each is trained only on its own backend.
  The native backend requires GCC 13, so a `ramulator2` profile only exists for GCC 13 versions.
- **One profile per exact GCC `major.minor`.** GCC ignores profile data written by any other
  version (the files are stamped, `B33*` for 13.3). A cluster that builds with GCC 11 needs a
  profile trained with that GCC 11.
- **`MANIFEST.json`** records the compiler, the source commit, when the profile was made, and
  every training run with its trace and config hashes.

## Building

`PGO` defaults to `auto`, like `LTO` and `TCMALLOC`:

- **`auto`** uses `pgo/<dram model>/gcc-<version>/` when all of these hold:
  - the build is fast, with GCC 11 or newer and no sanitizer;
  - the profile's manifest names the same GCC version and DRAM model.

  Otherwise it builds without a profile. `--build-info` shows which happened, and why, under
  `pgo`.
- **`PGO=1`** requires a usable profile and fails otherwise. **`PGO=0`** builds without one.
- **`PGO_PROFILE=<dir>`** uses another profile directory, for example a candidate profile before
  it is committed.

The profile's contents are part of the build fingerprint, so a new profile rebuilds everything.

## Training and checking

```bash
make pgo-train TRACE_ROOT=/path/to/tracezoo/champsim             # legacy DRAM
make pgo-train TRACE_ROOT=... WITH_RAMULATOR2=1 RAMULATOR2_ROOT=...  # Ramulator 2
make pgo-check TRACE_ROOT=...                                    # PGO against non-PGO fast
```

**`pgo-train`** takes about 10 minutes on one core:
1. It builds an instrumented fast binary.
2. It runs the training list one run after another, so libgcov merges the counters at each exit.
   Profiles collected separately and merged with `gcov-tool` were rejected by GCC 13.
3. It replaces `pgo/<dram model>/gcc-<version>/`.

Review the result and commit it.

**`pgo-check`** builds PGO and non-PGO fast binaries and runs them on the check list through
`tools/perf/compare_optimization.py`, which needs Python 3.11 or newer. It fails if statistics
differ or if PGO is more than `--threshold` (default 3%) slower on any trace.

The training list is the recipe that was validated:
- seven ordinary traces;
- two DRAM-heavy ones (GAP `bfs`, SPEC17 `roms`);
- one run on the default machine with `train-modules.toml`.

A fork may add its own project's traces. Trace paths are relative to `TRACE_ROOT`.

## Keeping a profile

**A committed profile ages well.** Measured at today's commit with profiles trained one change
and three hot-path commits behind, geomean speedups were:

| Profile | Geomean |
|---|---|
| Fresh | +20.4% |
| One change behind | +18.7% |
| Three hot-path commits behind | +19.0% |

- **Changed functions fall back to normal optimization.** A function whose control flow changed
  loses its profile and is compiled as if unprofiled; partial training ensures it is not
  optimized for size instead.
- **Moved code keeps its profile.** A function that only moved within its file keeps it.
- **Refresh weekly, or after large changes.** `champsim-infra`'s weekly job does the former.
  Never tune a profile by hand.

## Portability

**A profile trained in one checkout works in another,** at any path and in any build directory.
Three things in the build make that so:

1. **`-fprofile-prefix-path=<source root>`** makes profile file names relative to the source
   root.
2. **The compile rule adds `-dumpdir obj/<object directory>/`.** GCC hashes functions with
   internal linkage (`static`, anonymous namespace) with the object's auxiliary base name.
3. **Include paths are relative to the source root.** For such functions defined in headers,
   the hash also includes the header's path as compiled.

Two things stay path-dependent, and lose only their internal-linkage header functions when the
path differs:
- **The vcpkg headers**, when the dependency tree lies outside the checkout.
- **Ramulator's headers** in a native build, whose root is an absolute path.

## Measuring with PGO

A PGO build's speed moves by about ±3% with unrelated source changes, because GCC lays functions
out by the order in which the training first ran them. Gate behavior-preserving optimizations on
`PGO=0` builds, or retrain both sides.
