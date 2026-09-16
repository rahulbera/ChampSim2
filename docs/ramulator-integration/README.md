# Ramulator 2 integration documents

- [Backend reference](ramulator2-reference.md): the operating manual — build, ABI
  and provenance rules, admitted components and every rejection reason, the adapter
  contract, enforced counter limits, the sanitizer mode, the schema 2 statistics
  layout and the bandwidth procedure. Start here when changing anything native.
  (Split out of `CLAUDE.md`, which keeps the entry conditions and the hazards.)
- [Integration writeup](ramulator2-integration.md): design, evidence, weak points
  and the path to mainline.
- [Validation record](ramulator2-validation.md): commands, counts and the
  recovery record behind the writeup.
- [Bandwidth sweeps](ramulator2-bandwidth-sweeps.md): observations and best
  practices for DRAM bandwidth sweeps, with the generator in
  [`configs/ramulator2`](../../configs/ramulator2/bandwidth.py).
- [Design](specs/2026-09-13-ramulator2-design.md),
  [implementation plan](plans/2026-09-13-ramulator2.md) and
  [final implementation review](reviews/2026-09-13-ramulator2.md): the records
  written while the integration was built.

Tests and tools are described in [test/ramulator2](../../test/ramulator2/README.md).
