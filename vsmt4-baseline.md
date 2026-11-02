# vSMT4 baseline（单核路线，V0 + V1.2）

> 分支 `smt-vsmt4`，起点 `b1f05f8c`（Stage 6 收尾），标签 `vsmt4-baseline`。
> V0 的唯一机制改动是把 `num_cpus` 翻回 1，其余零机制改动。§1–§6 沉淀三把 N=1 锁、
> 锁 4（N=2）、单核 `make test` 基线、4-trace 供给侦察、stage3-debt a~d 归类。
> §7–§8 是 V1.2 的终止语义定论 + 4-trace 链校验（asid / 前端串扰 / PRF 足迹）。

---

## 0. V0 干了什么

vSMT4 长在**单核多 context**（`num_cpus=1`，`max_contexts` 2→4 是 V1 的事），所以
第一笔代码 commit 把 Stage 6 翻上去的 `num_cpus` 翻回来：

| commit | 内容 |
|---|---|
| `6397f457` | `inc/defs.h:23` `num_cpus` 2 → 1（V0 第一个 commit） |
| `9eae3f91` | `configs/vsmt4-lock4.toml`（锁 4 覆盖 config） |

Stage 6 里对单核无影响的通用加固**保留**：6.1 `stable_sort`（仅 `num_cpus=2` 才 >16
operable，单核不触发）、6.5c 空 trace 兜底。只摘掉 `num_cpus=2` 外壳。

## 1. 三把 N=1 锁 —— 逐位 PASS

`tools/run-accept.sh vsmt4-v0`（`configs/sample.toml` + `-w 1M -i 3M`，fotonik3d）。
`num_cpus=1` 重编后逐位复现，sha256 与 Stage 0 起的老口径完全一致：

| 锁 | stress | sha256（norm，逐位） | IPC |
|---|---|---|---|
| 锁 1 | baseline | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 2.34 |
| 锁 2 | prf64（`register_file_size=64`） | `9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56` | 1.778 |
| 锁 3 | sched64（`scheduler_size=64`） | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 2.34 |

结论：`num_cpus` 翻回 1 是**行为保持**的，顺带洗清了 6.1 `stable_sort` 的嫌疑（它只对
`num_cpus=2` 的 >16 operable 排序生效，单核不触发）。三把锁全 PASS，故 V0 第一 commit
成立，此后这就是日常门槛。

## 2. 锁 4（N=2 = 单核双 context fotonik3d ×2）

命令（`configs/sample.toml` + `configs/vsmt4-lock4.toml`，等价 `--set
ooo_cpu.cpu0.num_contexts=2`，同一 trace 传两遍）：

```
./bin/champsim --config configs/sample.toml --config configs/vsmt4-lock4.toml \
  --trace-version=1 -w 1000000 -i 3000000 -- TRACE TRACE
```

### 2.1 单 context 分母（standalone，= 锁 1）

| | instructions | cycles | roi_cycles | ipc | mispredicts |
|---|---|---|---|---|---|
| sa-fotonik | 3000000 | 1281979 | 1281979 | **2.34** | 7602 |

与 N=1 锁 1 逐字节同（cycles 1281979、mispredicts 7602、ipc 2.34）。

### 2.2 双 context 逐 context 数（r1）

| context | asid | instructions | cycles | roi_cycles | ipc | mispredicts |
|---|---|---|---|---|---|---|
| cpu0（ctx0） | `{0,0}` | 3000000 | 2828581 | 2828581 | 1.061 | 67710 |
| cpu1（ctx1） | `{1,1}` | 3014764 | 2828581 | 2814582 | 1.066 | 67437 |

（`meta.num_cpus=1`、`config.ooo_cpu.cpu0.num_contexts=2`、`build_id=0x8b7ce6e1a7b9c3ae`）

### 2.3 公平性三指标（`tools/smt/fairness.py`，self-test PASS）

| thread | IPC standalone | IPC co | speedup | slowdown |
|---|---|---|---|---|
| ctx0 | 2.3401 | 1.0606 | 0.4532 | 2.2064 |
| ctx1 | 2.3401 | 1.0659 | 0.4555 | 2.1955 |

**WS = 0.9087，ANTT = 2.2010**（对称到 0.49%）。与 Stage 5.3b 的 fotonik3d×2
（WS 0.9087 / ANTT 2.2010）**完全一致** —— `num_cpus` 翻回 + 保留 6.1/6.5c 对双 context
同样行为保持。

### 2.4 确定性 ×2

r1 / r2 两次独立运行，规范掉 `command_line`（唯一非确定字段，含输出路径）后 sha256：

```
250a66ca0a3cf92081bd9802bb999da028252e9fd01107d10a7256db37f3b268  co-fotonik-dual-r1.norm
250a66ca0a3cf92081bd9802bb999da028252e9fd01107d10a7256db37f3b268  co-fotonik-dual-r2.norm
```

逐位一致，确定性通过。

### 2.5 config 对称 + 物理地址口径（锁 4 的两条硬前提核对）

- **config 键对称**：`num_cpus=1` build 下两条 context 共享唯一的
  `[config.ooo_cpu.cpu0]`（没有 `[config.ooo_cpu.cpu1]`），不存在 Stage 6 在
  `num_cpus=2` 上发现的「只设 cpu0、cpu1 回落默认 → 1.65% skew」陷阱。锁 4 唯一新增
  knob 是 `num_contexts=2`。
- **物理地址**：两条 context 重放**同一条** trace，走同一个共享 PTW/vmem → 同一份
  VA→PA 映射，物理地址**完全相同（重叠）**。LLC miss 恰为 standalone 的 2×
  （load 87→174、rfo 162→327、translation 10→20），是「同程序双线程」重叠足迹的直接
  证据。所以 Stage 6 的「多程序、物理不重叠负载才有效」红线**不适用**本锁——那是对
  异 trace 多程序负载（sqlite+omnetpp）的约束；锁 4 是共享结构交错污染锁。

## 3. 单核 `make test` 基线

```
test cases:   1033 |   1007 passed | 4 failed | 22 skipped
assertions: 118694 | 118690 passed | 4 failed |  0 skipped
```

对照 `num_cpus=2` build：`1033 / 1013 / 0 / 20`。单核多出的 4 个失败、2 个跳过全在
多核专属测试（**预期**，不修测试）：

| 失败用例 | 原因 |
|---|---|
| `601-fixed-ptw.cc:85` | `CHECK(f.cfg.unconsulted_keys().empty())` → false（cpu1 fixed 夹具） |
| `601-fixed-ptw.cc:203` | `REQUIRE(unused.size()==1)` → 2==1（cpu1 夹具） |
| `604-multicore-ptw.cc:7` | `cpus.at(1)` 越界（`__n(1) >= size(1)`，无 cpu1） |
| `605-multicore-same-trace.cc:69` | `cpus.at(1).contexts.at(0)` 越界（无 cpu1） |

601 的两个失败是 6.5a 补的 cpu1 fixed-PTW 夹具；604/605 是 6.5a/6.5b 的显式多核测试。
单核 build 下 `cpus` 只有 `cpu0`，`cpus.at(1)` 必然 `vector::_M_range_check`。

## 4. 4-trace 供给侦察（V1 硬前置）

- **供给方式**：trace 路径按位置放在 `--` 之后，数量**必须恰等于**
  `num_cpus × num_contexts`（`src/main.cc:294`），行主序 **core-major / context-minor**
  （`src/main.cc:385-393`）：`T0→(core0,ctx0) T1→(core0,ctx1) T2→(core0,ctx2)
  T3→(core0,ctx3)`。
- **asid 显式/自动**：**自动**。v1 trace（`input_instr`，64B）**不携带 asid 字段**；
  tracereader 按全局 trace 槽位盖章 `asid = {槽位, 槽位}`（`src/champsim.cc:56-57`）。
  4 trace → ctx0..3 = `{0,0},{1,1},{2,2},{3,3}`。**没有显式 asid knob**。
- **trace 版本是单一全局旗标**：`--trace-version` 只取 1 或 2，同一 run 内所有 trace
  必须同版本。本地 v1 只有 fotonik3d 一条；sqlite/omnetpp/llvm/vpr 是
  `.champsim2.zst`（v2）。4-trace 若混 fotonik3d(v1) 与 sqlite(v2) 会因版本不同不可行。
- **4-trace 选材标准（PRF 足迹 ×4 ≲ 256）**：单线程 PRF 足迹
  （prf128→prf64 敏感度作代理）：fotonik3d ~47 寄存器/线程（−24%）、vpr −24%、
  omnetpp −17%、llvm −10%、sqlite −8.5%。fotonik3d×4 ≈ 188 < 256，最饿的 vpr×4 也
  < 256。**V1 必须先把 `register_file_size` 提到 256**（4×64），当前 128 装不下
  4 线程足迹 ≈188（`CLAUDE.md` 已记 fotonik3d 单线程 ~47）。
- **当前被拒记录（预期）**：
  - `num_contexts=2` + 4 trace → `ERROR: expected 2 trace(s) ... got 4`（exit 1）。
  - `num_contexts=4` → `ERROR: runtime config: ooo_cpu.cpu0.num_contexts must be
    between 1 and 2`（`max_contexts=2` 上界，`src/static_environment.cc:186`）。

**4-trace config 草案**（V1.1 把 `max_contexts` 2→4 之后才生效）：

```toml
[ooo_cpu.cpu0]
num_contexts = 4
register_file_size = 256      # 4 x ~64，覆盖 4 线程足迹 ~188
# trace 供给：-- T0 T1 T2 T3  → ctx0..3 asid {0,0},{1,1},{2,2},{3,3}
```

## 5. stage3-debt a~d 归类（挡 V1 vs 准确性债后置）

逐条对代码现状核销（不修，只归类）：

| 项 | 现状（V0 验） | 挡 V1? | 归类 |
|---|---|---|---|
| a. TLB tag 按 asid 键控 | 已核销：`matches_tag` 判别子已是 `keyed=(vmem!=nullptr)`（`src/cache.cc:184`），TLB 存 asid、物理缓存不存（`src/cache.cc:260-262`），`ed341fe3` 更正到位 | 否 | **已落地** |
| b. 8→12 全局态单例 | 4 个 ittage/blbp 已 per_context 复制；剩 12 个（cbp6×4 + perfect/ideal×8）由 `check_smt_capability` 按二元门槛 `num_contexts>1` **显式拒绝**（`inc/modules.h:314-319`），翻 4 不多拒一个 | 否 | **准确性债后置**（安全排除，V1 用 hashed_perceptron + basic_btb 均 supported） |
| c. load_waiters 未参与完成 | 已端到端铺好（`set_union` 合并，`src/cache.cc:124`），完成键仍是 `(block, asid)` | 否 | **准确性债后置**（广播 vs 精确 waiter，N=1 不动） |
| d. 真·双 context / trace asid / 多 trace | Stage 3/4/5 已落地，本次锁 4 端到端验证 | 否 | **已落地** |

**结论：a~d 四项都不挡 V1。** a、d 已落地（锁 4 就是证据）；b 是显式拒绝的安全排除
（翻 `max_contexts` 2→4 不改变 `num_contexts>1` 的二元门槛，也不影响 V1 要用的
hashed_perceptron + basic_btb）；c 是广播 vs 精确 waiter 的准确性后置债。V1 不必把
任何一项设成开工门槛。

## 6. 意外项

1. **锁 4 逐位复现 Stage 5.3b**：WS 0.9087 / ANTT 2.2010 与 Stage 5 完全一致 ——
   `num_cpus` 2→1 + 保留 6.1/6.5c 对双 context 也行为保持，不只对 N=1。
2. **trace 版本单一全局旗标**是 4-trace 供给的真约束：本地 v1 只有 fotonik3d 一条，
   4 路异 trace 只能用 v2（sqlite/omnetpp/llvm/vpr），且 v2 无显式分支类型（分支类型
   从寄存器用法推断，见运行期 WARNING）。
3. **物理地址口径**：锁 4 是「同程序重叠足迹」锁，不是「多程序非重叠」锁；Stage 6 的
   非重叠红线对锁 4 不适用，对将来的异 trace 多程序锁（sqlite+omnetpp）才生效。

---

## 7. V1.2：终止语义 + 4-trace 链校验

（V1.3 已把 `max_contexts` 2→4，本节两套 4-trace config 均在 `num_contexts=4` 下端到端
运行。口径：`configs/sample.toml` + `configs/vsmt4-n4.toml`，`-w 1M -i 3M`。）

### 7.1 终止语义（锁 4 ctx1>3M / ctx0 恰 3M 之谜，读码定论）

读 `src/champsim.cc` + `src/ooo_cpu.cc` 的结束条件，确切规则：

- **结束条件 = 全体达标**（`src/champsim.cc:176-180`）：core complete 当且仅当**每个**
  context 满足 `retired >= length` **或** 该 context 的 trace 已 EOF；外层 `while`
  循环（`:115`）在全部 core complete 时退出。这是唯一结束判据。
- **没有「谁达标谁停 fetch」**：核心 fetch 段（`O3_CPU::initialize_instruction`，
  `src/ooo_cpu.cc:119-150`）**不认识 `length`** —— 只受「共享 fetch 带宽 + 分支预测」
  门控；trace 供给（`do_cycle`，`src/champsim.cc:64`）只按 `!trace.eof()` 续喂。故一个
  先达标的 context **继续取指、继续退休**，直到全体达标、阶段结束。
- **退休是共享 RETIRE_WIDTH=5 的轮转**（`src/ooo_cpu.cc:1300-1309`）：每周期全体
  context 合计最多退 5 条。所以「最后达标」者超调 ≤ 5（落点 ≈ 恰 3M），而先达标者
  的**超调 = 它与最慢者退休进度差**，可很大。

实测印证（`-w 1M -i 3M`）：

| 组 | 各 context 最终 instructions（target 3M） |
|---|---|
| 锁 4 N=2 | ctx0 **3000000**、ctx1 3014764（+14764） |
| fotonik3d×4 | ctx0 **3000000**、ctx1 3014001、ctx2 3002665、ctx3 3001986 |
| 异 trace×4 | sqlite **3000001**、omnetpp 3061527、llvm 3429233、vpr 5929323 |

最慢者（每组第一列，SMT 下 IPC 最低）落点 ≈ 恰 3M；其余按 SMT-IPC 相对速度比例超调
（异 trace 组 vpr SMT-IPC 0.73 ≈ sqlite 0.37 的 1.97× → 超调 ≈ 2.9M）。这正是「全体
达标才结束、无 per-context 停 fetch」的直接后果。**V3 的 trace-EOF 下折正吃这条**
（`:180` 的 `|| eof` 分支：短 trace EOF 即算达标、不再参与退休判定）。

### 7.2 asid 确认：ctx0..3 = {0,0},{1,1},{2,2},{3,3}

代码链完整核销（与 §4 侦察一致，运行态确认）：

- `src/main.cc:385-393`：`trace_index` 行主序 core-major/context-minor，
  `trace 0→(core0,ctx0) … trace 3→(core0,ctx3)`。
- `src/main.cc:377-380`：tracereader 按**位置**构造（`i++` 从 0），
  `inc/instruction.h:298/301`：`ooo_model_instr{slot, instr}` → `asid = {slot, slot}`。
- `src/champsim.cc:55-57`：`do_cycle` 不重派 asid，asid 随 trace 落点。
- `test/cpp/src/605` 已 pin 同机制（2-context），4-context 是同一代码路径
  （`max_contexts=4` 后 `contexts` 数组放大）。

故 ctx0..3 的 asid 确定是 `{0,0},{1,1},{2,2},{3,3}`；stats 文档无 asid 字段，此为代码
确定性结论（非运行期观测）。

### 7.3 组 (a)：fotonik3d×4（容量方向，v1 同 trace ×4）

| context | instructions | cycles | ipc | mpki | acc% |
|---|---|---|---|---|---|
| ctx0 | 3000000 | 4889832 | 0.61 | 32.83 | 86.19 |
| ctx1 | 3014001 | 4889832 | 0.62 | 32.76 | 86.22 |
| ctx2 | 3002665 | 4889832 | 0.61 | 32.75 | 86.23 |
| ctx3 | 3001986 | 4889832 | 0.61 | 32.83 | 86.19 |

合计 IPC ≈ 2.46（12.0M 指令 / 4.89M 周期）。单跑 fotonik3d = 2.34 / mpki 2.53。方向
判据：合计 IPC 2.46 仅 ≈ 单跑 2.34（远低于 4×2.34=9.36），容量方向争用明显（共享
前端带宽 + 共享 PRF + 分支预测器污染）。4 context 各 mpki 从单跑 2.53 → 32.8（×13），
是**共享 hashed_perceptron 全局历史**的串扰（见 7.4 机制），非 asid/地址空间串扰。

### 7.4 组 (b)：4 条异 trace ×4（隔离方向，v2）

选材：sqlite sp0 / omnetpp sp0 / llvm sp1 / vpr sp0（全 v2；版本单一全局旗标不可混
fotonik3d v1）。`--trace-version=2`，`register_file_size=256`。单跑分母是同一二进制、
同 `-w 1M -i 3M` 的单 context 跑（`sa-*.toml`），非 -i 200M 的 prf 扫描值（两者区域
不同、不可比）。

| context | trace | 单跑 IPC | SMT4 IPC | 比值 | 单跑 mpki | SMT4 mpki | 比值 |
|---|---|---|---|---|---|---|---|
| ctx0 | sqlite | 0.80 | 0.37 | 0.46× | 5.50 | 32.06 | 5.8× |
| ctx1 | omnetpp | 0.95 | 0.38 | 0.40× | 11.46 | 29.10 | 2.5× |
| ctx2 | llvm | 1.09 | 0.42 | 0.39× | 12.62 | 40.48 | 3.2× |
| ctx3 | vpr | 1.63 | 0.73 | 0.45× | 3.46 | 21.06 | 6.1× |

**隔离方向判据 FAIL（前端口径），上报**：4 条异 trace 单跑 mpki 全部异常漂移
（2.5×–6.1×）、IPC 全部跌到单跑 0.39×–0.46×。但这不是 asid/地址空间串扰（7.2 已确认
asid 隔离完整），而是**共享分支预测器**的前端串扰：

- `hashed_perceptron` 声明 `capability=shared`（单实例，`branch/hashed_perceptron/
  hashed_perceptron.h:54`）；权重表索引折入 `asid_word`（权重按 asid 隔离），但
  **全局历史 `ghist_words` 是单一 shared folded-shift-register**（`:33`），所有
  context 的分支结果都 push 进同一条移位寄存器（`.cc:71-73`）——4 条异 trace 的历史
  互相污染，预测率暴跌。
- 次生（未深验）：`last_result` 是单一成员（`.h:47`），多 context 交错下
  predict→update 配对可能被别的 context 覆盖、权重更新用错索引。

这是**前端口径固有串扰（建模变量）**，不是 V0 锁定的 asid/va_to_pa 地址空间串扰。
`hashed_perceptron` 自述「share it safely」仅指权重不撞，不覆盖全局历史共享的串扰。
归入「意外/偏差」上报（§8），V1 不修（方向性判据，领导者审）。

### 7.5 PRF 足迹核对：两组 4 线程合计 ≤ 256

- fotonik3d×4：4×~47 ≈ 188 < 256（7.3 运行无 crash、无 `register_file_size too
  small`）。
- 异 trace×4：prf 足迹（prf40/prf48 边界，-i 200M 扫描值作量级代理）sqlite ≤40、
  omnetpp 41–48、llvm ≤40、vpr 41–48，合计 ≤176 < 256（7.4 运行无 crash）。256 足够，
  未自动放大。

---

## 8. V1.2 意外项（上报，不修）

1. **前端串扰（隔离方向 FAIL）**：共享 `hashed_perceptron` 全局历史让 4 条异 trace
   互污预测（mpki 2.5×–6.1×、IPC 0.39×–0.46×），同 trace ×4 更甚（mpki ×13）。这是
   前端口径建模变量，与 asid 隔离无关，V1 方向性判据成立（有争用）但量级需领导者审，
   关系 V2 是否要给 predictor 按 context 复制历史 / 权重。
2. **终止语义的 V3 影响**：无 per-context 停 fetch + 全体达标才结束 → 先达标者超调
   与「SMT-IPC 相对速度」成正比（异 trace 组 vpr 超调 2.9M、近 2×）。V3 的 trace-EOF
   下折、以及未来「谁达标谁停 fetch」若要落地，都在 §7.1 这条规则上改。

---

## 9. V1.4：12 个 unsupported singleton 能力表核销（只记录，不修）

能力判定机制：模块声明 `static constexpr smt_capability capability` 即 opt-in；**不
声明者默认 `unsupported`**（`inc/modules.h:280-295`），guard 在 `num_contexts>1` 时抛
「keeps its state in namespace-scope globals」（`inc/modules.h:316`）。12 个默认
unsupported = cbp6×4 + perfect/ideal×8。逐条核实「真·全局单例」还是「保守 guard」：

| 模块 | 类别 | 状态存哪 | 判定 |
|---|---|---|---|
| cbp6_tagescl64 | branch | namespace-scope 全局表（头注释自述） | **真·全局单例** |
| cbp6_tagescl192 | branch | namespace-scope 全局表 | **真·全局单例** |
| cbp6_runlts_norv | branch | namespace-scope 全局表 | **真·全局单例** |
| cbp6_ddtage | branch | namespace-scope 全局表 | **真·全局单例** |
| perfect_branch | branch | 无状态（读 `ctx().input_queue.front().branch_taken`） | 保守 guard |
| perfect_btb | btb | 无状态（读 `ctx().input_queue.front().branch_target`） | 保守 guard |
| perfect_direct | btb | 成员表 `direct_predictor`（asid_word 折入索引） | 保守 guard |
| perfect_indirect | btb | 成员表 `indirect_predictor`+`return_stack` | 保守 guard |
| perfect_return | btb | 同上（`btb_impl<group::ras>`） | 保守 guard |
| ideal_btb | btb | 成员表，同上 | 保守 guard |
| ideal_btb_pibtb | btb | 成员表，同上 | 保守 guard |
| ideal_btb_pibtb_pras | btb | 成员表，同上 | 保守 guard |

判定说明：

- **cbp6×4 = 真·全局单例**：vendored 竞赛预测器把表放在 namespace-scope 全局，所有
  instance 共享；guard 的拒绝**准确且必要**（`cbp6_tagescl64.h:11-14` 等自述）。
- **perfect/ideal×8 = 保守 guard**：状态是**成员表**（`btb_impl` 持有 `ras`/`direct`/
  `indirect` 成员，`inc/perfect_group/perfect_group.h:246-248`），非 namespace-scope；
  `direct/indirect` 索引折入 `asid_word`（`:119,163,169`），理论上 `shared` 可行。但
  guard 的拒绝**仍是正确**的，因为两处残留：
  1. `btb_impl::prediction` / `perfect_branch` / `perfect_btb` 用 `intern->ctx()`
     （=`contexts.front()`）读 input_queue 做 branch-type 判定，**不认识 context**
     （`:257`）——多 context 下会读错槽位，须改 `contexts[asid[0]]` 才能 opt-in。
  2. `return_stack::calibrate_call_size` 有个 `static int num_times_returned_backwards`
     （`:212`）——只是 WARNING 节流计数器（限 10 条），不影响预测结果，但严格说仍是
     function-local 单例。

**结论**：12 个拒绝全部**成立**（cbp6×4 必要、perfect/ideal×8 保守但保护了 latent
`ctx()` 缺陷）。V1 要用的 `hashed_perceptron`（`capability=shared`，
`hashed_perceptron.h:54`）与 `basic_btb`（`capability=shared`，`basic_btb.h:23`）
都不在这 12 个里，**不被阻挡**；但 hashed_perceptron 的 `shared` 是 §7.4 那类「权重按
asid、历史共享」的前端串扰口径，与这里「安全共享」的承诺边界已在 §7.4/§8.1 记清。
