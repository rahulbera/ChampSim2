# BLBP-Style Bit-Level Perceptron Indirect Target Predictor — Design

**Date:** 2026-08-13 · **Branch:** `rbdev` · **Authors:** Rahul Bera and Claude Opus 5
**Paper:** E. Garza, S. Mirbagher-Ajorpaz, T. A. Khan, D. A. Jiménez, "Bit-Level
Perceptron Prediction for Indirect Branches," ISCA 2019.

---

## 1. Decisions (User-Approved)

1. **BLBP-inspired, tuned for our traces** — core mechanism per the paper's
   Algorithms 1–2, constants re-tuned on our workloads. The claim is "a
   BLBP-style predictor", not a reproduction; the untuned (paper-constants)
   configuration is also reported as the nearest reproduction proxy.
   **No reference implementation exists publicly** (verified); this is a
   clean-room build with a different verification stack than ITTAGE's.
2. **One budget: ~64 KB, iso with `ittage_64kb`.** A 32 KB variant only if the
   64 KB result justifies it.
3. **Tuning = Option B**: stream-driven hill-climbing on the cluster, full
   ChampSim validation of finalists. The paper itself tuned this way (§3.6).
4. **Cluster**: traces at `kratos2:/home/rahbera/tracezoo/champsim/version2.1/`
   (68 = 32 SPEC + 36 agentic across 9 families). Launch via the `slurm-launch`
   skill. **~1400 simultaneous single-core jobs available; the tuner is sized
   to use them.**
5. **Train/test split, by family (no window-level leakage; `.toolchain` stays
   with its parent):**
   - TRAIN (40): agentic rubocop, redis, prometheus (+toolchains) = 24;
     SPEC omnetpp(2), vpr(2), llvm(2), gcc(1), sqlite(3), zstd(3),
     stockfish(3) = 16.
   - TEST (28): agentic immutable-js, gin-gonic, ripgrep = 12; SPEC cpython(2),
     ns3(3), gem5(1), cppcheck(2), abc(2), ntest(3), sealcrypto(3) = 16.
   - (An earlier draft said 44/24; the correct arithmetic is 40 train, 28 test.)
   - Indirect-heavy workloads balanced (train: omnetpp/vpr/llvm; test:
     cpython/ns3/gem5); language ecosystems Ruby/C/Go vs JS/Go/Rust.
6. **Deliverable**: detailed report, BLBP vs ITTAGE-64KB iso-sized, SPEC and
   agentic, train and test separated, capture vs `perfect_indirect`.

## 2. Predictor Core (`inc/blbp/blbp.h`)

Pure, ChampSim-independent, **runtime-configured** (the tuner varies constants
without recompiling; golden tests construct tiny configs, e.g. K=4/N=1 for the
paper's Figure 3 example).

State (paper values in the 64 KB config):

| Component | Geometry |
|---|---|
| `W[N][M][K]` | N=8 sub-predictors × **M=952** rows × K=12 bits, 4-bit sign/magnitude (±7) |
| GHIST | 630-bit shift register of conditional outcomes |
| Local histories | 256 × 10-bit, per branch PC, recording bit 3 of targets |
| IBTB | 64 sets × 64 ways; 8-bit partial tag, 7-bit region ptr, 20-bit offset, 2-bit RRIP |
| Region table | 128 entries, LRU |
| Per-bit θ_k | adaptive threshold, O-GEHL style |

**Prediction (Algorithm 1).** Sub-predictor 0 indexes by local history; 1–7 by
`hash(GHIST[a_i..b_i]) mod M` with intervals initialised to the paper's
{(0,13),(1,33),(23,49),(44,85),(77,149),(159,270),(252,630)}. y_out[k] = Σ_i
transfer(W[i][j_i][k]); candidate similarity = Σ y_out[k] over the candidate's
**1-bits** (0/1 semantics per Figure 4: 0+19+0+32=51, NOT ±1); argmax with
**later-candidate-wins ties** (Figure 3 Prediction 2 shows the tie going to
target2, contradicting Algorithm 1's strict `>`; we follow the figure and
document it).

**Update (Algorithm 2).** Selective bit training (train bit k only if it
differs across the branch's candidate set); train on mispredict or |y_out[k]| <
θ_k; adaptive θ per bit; saturate at ±7. Update is **self-contained** — it
re-derives y_out and candidates rather than trusting predict-time state
(ITTAGE defect-B lesson).

**Assumptions where the paper is silent**, chosen once, documented at the site:
M=947 (derived: 64 KB − IBTB 18.5 KB − region 0.7 KB − histories 0.4 KB ⇒
weights 44.4 KB ⇒ M = ⌊44.4 KB / (8·12·4b)⌋); GHIST hash = folded-history XOR
(ITTAGE lineage, in-tree); transfer function from Figure 5:
{2,4,6,8,11,14,18,24}; SRRIP insertion/promotion per Jaleel et al.
Budget: **63.98 KB**, static_asserted in the shell.

## 3. Adapter (`inc/blbp/blbp_btb.h`, shell `btb/blbp_64kb/`)

`ittage_btb.h` pattern verbatim: copied `direct_predictor` + `return_stack`,
BLBP on the indirect path only. ITTAGE-audit defect classes designed out:

- `always_taken = true` on the entire indirect path (defect A);
- self-contained update, idempotent re-derivation (defect B);
- empty candidate set ⇒ `{0,false}`, no sentinel to leak;
- GHIST fed at `update_btb` from conditional outcomes; local history from
  indirect targets' bit 3;
- single-core guard, no double-init leak (defect F);
- false partial-tag matches in the IBTB are **by design** (paper §3.7) and are
  not "fixed".

**Acceptance criterion: the invariance gate.** BRANCH_CONDITIONAL /
BRANCH_RETURN / BRANCH_DIRECT_JUMP misses bit-identical to basic_btb, ittage_*,
perfect_indirect on every trace, before any BLBP number is read.

## 4. Tuning (`tools/blbp_tune/`)

- **Streams**: extend `tools/ittage_equiv/extract_stream.py` records to
  `(pc, next_ip, branch_type, taken)`; extract each training trace once, full
  250M window, stored on the cluster beside the traces.
- **Objective**: pooled indirect mispredicts over the 40 training streams
  (counts, never a mean of per-trace percentages).
- **Search**: parallel hill-climbing sized to ~1400 cores — 8 independent
  chains seeded from the published constants (different RNG), each generation
  proposing ~160 single-knob mutations per chain (interval endpoints, monotone
  transfer values); all ≈1300 candidates evaluated concurrently, one
  (config, stream) pair per core where beneficial; chains accept their best
  improving mutation; every 5 generations the two worst chains restart from the
  global best. Stop: no global improvement for 3 generations. Checkpoint and
  full accept-log every generation (auditable for overfitting).
- **Validation gate**: top-3 tuned + untuned configs get full ChampSim runs on
  all 68 traces. Stream ranking must agree with full-run ranking on the training
  set, else stop and diagnose. Test-set numbers computed **once**, after the
  tuned config is frozen and committed.

## 5. Verification (Clean-Room Stack)

| Layer | Proves |
|---|---|
| Golden vectors: Figure 3 sequence (3333→2424→1515→0606, tie→target2, convergence to 0101) and Figure 4 similarities (51 vs 41 — the figure's "43" is an arithmetic typo, noted) | core math |
| Unit tests: routing, pairing idempotence, empty-candidate, selective-bit suppression, θ adaptation, ±7 saturation | wiring |
| Invariance gate | adapter honesty |
| Oracle bound: BLBP ≤ perfect_indirect per trace, asserted in rollup | no impossible results |
| Plausibility band: untuned BLBP within ~±15% of ITTAGE-64KB on train (paper: ±5% on theirs) | gross-error tripwire |
| Stream-vs-full-run rank agreement | tuning proxy validity |
| Multi-agent adversarial audit before publishing | what tests don't express |

## 6. Milestones

| # | Work | Exit |
|---|---|---|
| M1 | Pure core, TDD on golden vectors | Fig 3/4 tests green |
| M2 | Adapter + shell + unit tests | invariance gate on 2-trace smoke |
| M3 | Budget accounting | 63.98 KB asserted |
| M4 | Extractor + tuner, local smoke | untuned stream-MPKI on 2 traces |
| M5 | Cluster bootstrap, 68 streams, untuned baseline | 5-config × 68 full runs |
| M6 | Tuning campaign (train only) | convergence + logged curves |
| M7 | Validation + frozen test eval | rank agreement; test computed once |
| M8 | Adversarial audit → report | blockers fixed; research-log entry |

Evaluation metrics/aggregation inherited from the ITTAGE campaign unchanged.
