# vSMT4 V1 小结（单核四槽静态 SMT4，V1.0–V1.6）

> 分支 `smt-vsmt4`，V0 起点 `1029e57b`（tag `vsmt4-v0`）。V1 = 把单核 `max_contexts`
> 2→4、四槽静态 SMT4 落地并首跑验收。本文收口 V1 全部子步，V1.6 是 N=4 首跑验收。
> 详细机制见 `vsmt4-baseline.md`（V0 + V1.2/1.4 沉淀），本文只记 V1 的增量与结论。

---

## 0. V1 子步与 commit 链

| 子步 | commit | 内容 | 门槛 |
|---|---|---|---|
| V1.0 | `de8e37a7` | `num_cpus=1` build 下 skip 601×2/604/605 多核专属测试 | `make test` 1033/1007/0/26 |
| V1.1 | `b82a08ab` | `configs/vsmt4-n4.toml`（num_contexts=4 + register_file_size=256） | —（config 草案） |
| V1.2 | `95ac4e46` | 4-trace 链校验 + 终止语义定论 → `vsmt4-baseline.md` §7 | 记录 |
| V1.3 | `c06dadaf` | `inc/defs.h` `max_contexts` 2→4 | 3 锁 + 锁 4 逐位不变 |
| V1.4 | `91417c64` | 12 unsupported singleton 能力表 → `vsmt4-baseline.md` §9 | 记录 |
| V1.5 | `9dd1635c` | O3_CPU 埋 `mode`/`active` 双字段（纯新增零行为） | 3 锁 + 锁 4 逐位不变 |
| **V1.6** | 本文 | N=4 首跑验收（无 crash、合理 IPC、公平性、水位线、压力） | 见 §2 |

commit 链（自 V0）：`1029e57b` → `de8e37a7` → `b82a08ab` → `c06dadaf` →
`95ac4e46` → `91417c64` → `9dd1635c`（V1.5）。

---

## 1. 三把 N=1 锁 + 锁 4：V1.3 / V1.5 之后逐位复核

V1.3（`max_contexts` 2→4）与 V1.5（mode/active 埋字段）都是「零行为」改动，两道
门槛均逐位复核通过：

| 锁 | sha256（norm，逐位） | IPC |
|---|---|---|
| 锁 1 baseline | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 2.34 |
| 锁 2 prf64 | `9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56` | 1.778 |
| 锁 3 sched64 | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 2.34 |
| 锁 4 N=2 | build_id `0x8b7ce6e1a7b9c3ae`，WS 0.9087 / ANTT 2.2010 / HSU 0.4543 | 1.061/1.066 |

V1.5 提交后把 N=4 路径也复核了一遍：`n4-fotonikx4` / `n4-hetero` 的 toml 与 V1.2
二进制逐位一致（仅 command_line 归一），确认 mode/active 埋字段对 `num_contexts=4`
同样零行为。

---

## 2. V1.6：N=4 首跑验收

口径：`configs/sample.toml` + `configs/vsmt4-n4.toml`（num_contexts=4，
register_file_size=256），`-w 1M -i 3M`。build_id `0xc84700f25e9969a0`。

### 2.1 组 (a)：fotonik3d×4（容量方向，v1 同 trace ×4）

| context | instructions | cycles | roi_cycles | ipc | mpki |
|---|---|---|---|---|---|
| ctx0 | 3000000 | 4889832 | 4889832 | 0.61 | 32.83 |
| ctx1 | 3014001 | 4889832 | 4866382 | 0.62 | 32.76 |
| ctx2 | 3002665 | 4889832 | 4885697 | 0.61 | 32.75 |
| ctx3 | 3001986 | 4889832 | 4886883 | 0.61 | 32.83 |

- **无 crash**，无 `register_file_size too small`（stderr 0 命中）。
- 合计 IPC ≈ **2.458**（12.02M / 4.89M），**< 4×2.34 = 9.36**（诚实口径：四槽合计
  仅 ≈ 单跑 2.34 的 1.05 倍，容量争用明显）。
- 每 context IPC 0.61–0.62（单跑 2.34），mpki 从单跑 2.53 → 32.8（×13，共享
  hashed_perceptron 全局历史串扰，见 baseline.md §7.4）。

### 2.2 组 (b)：4 条异 trace ×4（隔离方向，v2）

选材 sqlite sp0 / omnetpp sp0 / llvm sp1 / vpr sp0（全 v2），`--trace-version=2`。

| context | trace | 单跑 IPC（prf256） | SMT4 IPC | 单跑 mpki | SMT4 mpki |
|---|---|---|---|---|---|
| ctx0 | sqlite | 0.83 | 0.37 | 5.50 | 32.06 |
| ctx1 | omnetpp | 1.01 | 0.38 | 11.46 | 29.10 |
| ctx2 | llvm | 1.11 | 0.42 | 12.62 | 40.48 |
| ctx3 | vpr | 1.69 | 0.73 | 3.46 | 21.06 |

合计 IPC ≈ **1.895**（15.42M / 8.14M）。隔离方向判据同 V1.2 结论：前端（共享
predictor）串扰为主，asid/地址空间隔离完整（baseline.md §7.2/§7.4），方向性判定
成立、量级待领导者审（关系 V2 是否给 predictor 按 context 复制历史/权重）。

### 2.3 公平性三指标（分母已校正为 register_file_size=256，见 §3）

定义（`tools/smt/fairness.py` 同款）：`speedup_i = IPC_i^co / IPC_i^standalone`，
`WS = Σ speedup_i`，`ANTT = mean(slowdown_i)`，`HSU = 1/ANTT`。分子用每线程自身
`roi_cycles`（非 overshoot-tainted `ipc`），分母用同 config（prf256）单 context 跑。

**fotonik3d×4**（分母 2.65 @ prf256）：

| thread | IPC standalone | IPC co | speedup | slowdown |
|---|---|---|---|---|
| ctx0 | 2.6499 | 0.6135 | 0.2315 | 4.3192 |
| ctx1 | 2.6499 | 0.6165 | 0.2326 | 4.2984 |
| ctx2 | 2.6499 | 0.6140 | 0.2317 | 4.3155 |
| ctx3 | 2.6499 | 0.6139 | 0.2317 | 4.3166 |

**WS = 0.9276，ANTT = 4.3124，HSU = 0.2319**（对称到 0.2%）。

**异 trace ×4**（分母 0.83/1.01/1.11/1.69 @ prf256）：

| thread | IPC standalone | IPC co | speedup | slowdown |
|---|---|---|---|---|
| sqlite | 0.8279 | 0.3687 | 0.4453 | 2.2455 |
| omnetpp | 1.0069 | 0.3760 | 0.3735 | 2.6777 |
| llvm | 1.1142 | 0.4270 | 0.3832 | 2.6096 |
| vpr | 1.6947 | 0.7213 | 0.4256 | 2.3495 |

**WS = 1.6276，ANTT = 2.4706，HSU = 0.4048**。

「同机器」校验通过：co 与 standalone 的有效 `[config]` 仅差 `num_contexts`（1 vs 4）
与 `vmem.randomization`（控制的变量），`register_file_size` 已对齐 256（§3）。

### 2.4 Σneed ≤ 24 水位线（满负荷预留核对）

代码链（`src/ooo_cpu.cc:641-679`、`744-755`）：

- `max_need = NUM_INSTR_SOURCES(4) + NUM_INSTR_DESTINATIONS(2) = 6`（单条指令重命名
  需求上界：distinct unmapped sources + destinations）。
- `context_need(c)` = 该 context 最老未调度指令的 rename 需求，`CHAMPSIM_ASSERT(need
  <= max_need)`；`total_need = Σ_k need_k`。
- N=4 满负荷 → **Σneed ≤ 4×6 = 24**。水位线判据 `free >= need_this + sibling_need`
  （`sibling_need = total_need - need[c]`）保证任何 context 都不能把共享池抽干到让
  兄弟无法重命名其最老指令（死锁下限 `N >= committed + Σneed`）。

核对结论：PRF=256，4 线程足迹 ≈188（baseline.md §7.5），`committed(~188) + Σneed(≤24)
= 212 < 256`，**预留充裕**。运行态无一次 `register_file_too_small_smt` 抛出（所有
N=4 run 的 stderr 均 0 命中）。`need` 上界 6 在 4 线程下公式不变（`max_need` 是
per-instruction 常数，与线程数无关）。

### 2.5 死锁 / 饥饿压力

- **长跑**（fotonik3d×4，`-i 10M`，prf256）：exit 0，四 context 各退休 10016011 /
  10023886 / 10000004 / 10002242（最慢者 ≈ 恰 10M，其余按 SMT-IPC 相对速度超调，与
  §7.1 终止语义一致），per-context IPC 0.62（与 3M 跑同），无死锁/饥饿/抛错。
- **极端 config**（fotonik3d×4，`-i 3M`，`register_file_size=192`：高于 4 线程足迹
  ≈188、低于 188+24=212）：exit 0，无抛错无死锁，per-context IPC 从 0.61 → **0.41**
  （cycles 4889832 → 7385434）。水位线在此被持续压满，context 频繁「等兄弟退休释放
  寄存器」而 stall，但**所有 context 均持续推进**（各退休 ≥ 3M），无饥饿到零——正是
  水位线预留要保证的行为。

---

## 3. register_file_size 是吞吐旋钮：分母校正（V1.6 发现，上报）

V1.2 的 §7.4 单跑分母（`sa-*.toml`）是 `sample.toml` 默认 `register_file_size=128`，
而 co-phase 用 `vsmt4-n4.toml` 的 256。当时以为「足迹 < 128 时 PRF 大小不影响
IPC」，**这是错的**：

| trace | prf128 单跑 IPC | prf256 单跑 IPC | Δ |
|---|---|---|---|
| fotonik3d | 2.34 | 2.65 | **+13.2%** |
| sqlite | 0.80 | 0.83 | +3.7% |
| omnetpp | 0.95 | 1.01 | +6.3% |
| llvm | 1.09 | 1.11 | +1.8% |
| vpr | 1.63 | 1.69 | +3.7% |

机制：`register_file_size` 不是「足迹上限」而是**物理寄存器池总容量**（已提交
architectural + 在飞 rename 目的寄存器），128→256 直接扩大在飞指令窗口，即使单线程
足迹 ≈47 < 128 也涨 IPC。所以公平性分母必须与 co-phase 同 config（prf256），否则
speedup 系统性偏高（fotonik 用 prf128 分母得 WS 1.0503，用 prf256 得 **0.9276**）。

**处置**：V1.6 公平性一律用 prf256 分母（§2.3），`sa-*-prf256.toml` 已跑并存档
（`$HOME/SMT-experiment/stage1/vsmt4-v1.6/`）。`vsmt4-baseline.md` §7.4 的「单跑
IPC / 比值」列仍是 prf128 口径（结论方向不变：有争用、IPC 跌到 0.37–0.73），是否
回改那两列待领导者指示（§4 清单第 7 项）。

---

## 4. V1.6 意外项（上报，不顺手修）

1. **register_file_size 是吞吐旋钮**（§3）：prf128 vs prf256 单跑 IPC 差 1.8%–13.2%，
   是 V1.2 §7.4 分母的口径错误，V1.6 已用 prf256 分母校正。关系 V2 的公平性门槛必须
   固定「同 config（含 register_file_size）单 context 分母」这条。
2. **前端串扰量级**（承接 V1.2 §8.1）：4 线程下 fotonik mpki 2.53→32.8（×13）、异
   trace mpki 2.5×–6.1×，是共享 hashed_perceptron 全局历史。V2 若要公平性「可信」，
   大概率要先给 predictor 按 context 复制历史/权重（或至少复刻 Sniper/真实 SMT 的
   共享 predictor 语义并明确其为建模变量）。
3. **水位线在 prf192 压满的代价**（§2.5）：prf256→192 让 per-context IPC 0.61→0.41
   （−33%），说明 256 相对 4 线程是「余量可观」而非「临界」，V2 若做按 `active` 动态
   共享 PRF 分母，192–256 之间是值得建模的敏感区间。

---

## 5. 单核 `make test` 基线（V1.6 复跑）

```
test cases:   1033 |   1007 passed | 26 skipped
assertions: 118676 | 118676 passed |  0 skipped
```

与 V1.0（`de8e37a7` 的 skip 修复）一致：0 failed、26 skipped（多核专属测试在
`num_cpus=1` build 下 skip）。V1.3（max_contexts 2→4）与 V1.5（mode/active）未改变
测试结果。
