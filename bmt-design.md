# BMT 设计定案（bmt-design.md）

> 项目转向：从「IBM 式运行时模式切换 vSMT4」改为复现 **Balanced Multithreading**
> （Tune et al., MICRO-37 2004）。固定物理槽（C_phys=2 或 4）+ 更多虚拟线程位
> （C_virt），长延迟 load 触发整线程换出（寄存器入 IRB）与换入（从 IRB 恢复）。
> 最终目的：在这核上支撑沙箱——每个沙箱 = 一个虚拟位。
>
> 本文件是 B0 侦察与设计定案的落点。判据 = 保真度（论文行为/数字能否复现），
> 逐位锁（三把 N=1 + 锁 4）是全程门槛；BMT 新机制默认关闭时必须逐位等于现状。
> 只读侦察 + 设计，不改机制代码。

---

## 0. 复现目标与判据

| 论文数字 | 复现判据 |
|---|---|
| BMT-2/4 vs SMT-2 @ 4 线程 +26% 加权加速比 | 方向 + 量级复现，不苛求同值；给机理解释 |
| C_virt/C_phys ≈ 2 最优（2/6 回落） | 扫 C_virt∈{2,3,4}（上限 4 = trace 数），形状对 |
| load 检测阈值 25→35 周期全线更优 | 本地阈值扫描出现「更长的等出并行第二 miss 更优」的单调段 |
| 固件切换 ~60 周期；firmware vs instant 两档 | 两档旋钮对比，firmware 按分步时序合计 |

已定参数（leader 拍板，不再议）：只做 SMT2/SMT4 物理槽，主打 BMT-2/4（2 物理 + 4 虚拟）；
trace 维持 4 条（sqlite/omnetpp/llvm/vpr，全 v2）；不做运行时模式切换；V3/V4/V5 线
（smt-vsmt4 分支）冻结封存。

---

## 1. 虚拟上下文层（B0.4 第 1 项）

### 1.1 数据结构

现状：`O3_CPU::contexts` 是 `std::vector<core_context>`，长度 = `num_contexts`
（物理槽数 C_phys），每个 `core_context` 私有持有前/后端缓冲、ROB、LQ/SQ、RAT、
input_queue、预测器状态、stats。`do_cycle`（src/champsim.cc:60-74）按
`trace_index[core][context]` 一对一地把 trace 喂进 `ctx.input_queue`。

BMT 加一层，**物理槽与虚拟位分离**：

- `contexts`（物理槽，C_phys 个）保持 `std::vector<core_context>` 不动——所有
  每 cycle 的流水线迭代（fetch/decode/dispatch/schedule/retire）仍遍历物理槽，
  **零改动**。物理槽是「管道透镜」，被换入换出的虚拟位轮流复用。
- 新增 `virtual_thread` 池（C_virt 个），每个记录一个虚拟位的**持久**状态：
  - `virtual_id`（0..C_virt-1）；
  - `trace_slot`（它绑定哪条 trace 的位置索引，即 `trace_index` 的键）；
  - `input_queue`（换出时未取指的 trace 残量，从物理槽 `core_context` 迁入）；
  - `saved_rat`（IRB 寄存器存档，见 §4；是架构寄存器映射快照，非数值）；
  - `saved_pc`（架构 PC，换出时存档，换入时恢复取指起点）；
  - 预测器私有状态（见 §5）；
  - stats + `empty`/`finished` 标记（EOF 语义，见 1.3）。
- 新增映射表 `slot_map[C_phys]`（`int`，-1 = 空槽）：物理槽 i 当前绑定哪个
  `virtual_id`。

### 1.2 trace 挂虚拟位而非物理槽

`trace_index` 的键从 `(core, context)` 改成 `(core, virtual_id)`：`do_cycle` 的喂流
循环改为，对每个物理槽 i 取 `v = slot_map[i]`，读 `traces[trace_index[core][v]]`
喂进 `contexts[i].input_queue`。`v == -1`（空槽）则不喂。trace 流本身（tracereader）
每条 trace 一个，换出时**自然暂停**（不读它），换入时从断点续读——tracereader 持
位置，无需回退。V3.4 的「input_queue 归档 + 空 map 重建」正是这层语义的单次版本：
BMT 把它泛化成「归档到虚拟位记录」而非「就地冻结」。

### 1.3 EOF 语义

- 虚拟位 trace 到 EOF → 该虚拟位跑完（`empty`），此后不再换入；其物理槽经 LRR
  重新绑给下一个待跑虚拟位。
- **尾段「3 挤 2」如实定义**：4 虚拟位跑 2 槽时，任一虚拟位 EOF 即释放一个槽，
  剩 3 个虚拟位正常争 2 槽（欠载 + 换入继续）。论文无尾段问题（负载同步跑完）；
  我们显式承认尾段就是普通欠载（活虚拟位 < 物理槽），不是特殊态。
- 度量口径（B0.5）：weighted speedup 只在**满载共相窗口**统计（所有 C_phys 槽都
  有活虚拟位且都未 EOF），尾段（活虚拟位 < C_phys）单独计「欠载段」不入分母，
  避免「3 挤 2」把单线程 IPC 分母污染掉。具体：per-virtual 位 speedup = 该位
  ROI cycles（单跑分母）÷ 该位共相段 cycles；尾段 cycle 不计入任何位的 speedup 分母。

---

## 2. 换出序列（B0.4 第 2 项）

触发：物理槽 i 上虚拟位 v 执行到一条 load，issued 后 **N 周期未完成**（阈值检测，
§B0.2 给候选）。序列：

1. **取消 load（内存请求续飞）**：flush 该 load 及其后的 in-flight 指令；已发往
   缓存/DRAM 的请求**不取消**（channel 里续飞，完成后照常返回）。值无建模，所以
   「数据写回 IRB 寄存器」退化为「请求完成暖缓存」——换入后重新执行该 load 命中已
   预热缓存（见 §4.3 的 value-less 简化）。
2. **scoped 冲刷**：`reset_frontend_RAT()`（core_context.cc:68-73，frontend←backend）
   回滚到架构态；清空 ROB / DISPATCH / DECODE / DIB_HIT / IFETCH 缓冲与 LQ / SQ；
   释放 in-flight（未提交）指令占用的物理寄存器（`backend_RAT` 之外的那些）。
3. **rsave 到 IRB**：把架构寄存器映射（backend_RAT 快照）写入 `virtual_threads[v].saved_rat`
   （§4），然后释放 backend_RAT 里的物理寄存器回共享 free pool；存档 PC。
4. **腾槽**：`slot_map[i] = -1`；v 的 input_queue 残量迁入 `virtual_threads[v]`。

复用点（B0.1 已确认）：`reclaim_registers()`（core_context.cc:83-98）的「遍历
backend_RAT 释放全部物理寄存器」是 save 的原语——BMT 把它改成「先快照 RAT 再释放」，
释放部分照抄；`reset_frontend_RAT()` 是 scoped 冲刷的 RAT 回滚原语。V3.0 的
`drain_complete()` 判定（ROB/缓冲/LQ/SQ 全空）与 scoped 冲刷逻辑可 cherry-pick；
但 BMT 的换出是**主动冲刷**（不等自然排空），所以复用的是「判定哪些结构该清」的
结构清单，不是「等待排空」的时点。

---

## 3. 换入序列（B0.4 第 3 项）

触发：空槽 + 有可换入虚拟位。

1. **LRR 选择 + 活锁防护**：`virtual_round_robin` 从 C_virt 池里选下一个
   `!empty && !finished` 的虚拟位（LRR = least-recently-run，论文用 LRU 变体，我们
   用轮转近似）。**活锁防护**：一个虚拟位换入后必须 **≥1 条提交** 才允许再次换出
   ——否则「换入→立刻触发换出→再换入」空转。论文的对应是 switch-on-miss 只在有
   进度时才发生。
2. **绑槽**：`slot_map[i] = v`。
3. **rrestore**：从 `saved_rat` 为每个已映射架构寄存器分配**新的**物理寄存器（共享
   free pool 取）、填元数据、重建 frontend_RAT == backend_RAT（§4）；恢复 PC 与
   预测器私有状态。
4. **恢复取指**：把 v 的 input_queue 残量迁回 `contexts[i].input_queue`，取指从 PC 续。
5. **switch-when-ready 门控**：换入**本身**立即进行（LRR 选位 + 绑槽 + rrestore 的
   时延就是 §6 的切换成本），但**取指**被门控到「该虚拟位所有换出时续飞的 load 都
   返回」——全等内存时换入但门控取指，避免 load 还没回就把线程重新跑起来造成语义
   裂缝（论文 switch-when-ready 的对应物）。

---

## 4. IRB 建模（B0.4 第 4 项）

### 4.1 模拟器里是数组，但延迟/端口要建模

IRB = `std::vector<irb_entry>`，每个虚拟位一个 `irb_entry`。**内存上即时访问**，但
rsave/rrestore 的**时延与端口带宽按论文建模**：10 周期流水化访问、4 端口。
62 架构寄存器（论文 Alpha 31 int + 31 fp）→ 4 端口 → ceil(62/4)=16 拍 rsave + 16 拍
rrestore ≈ 32 拍 + 开销，合到论文 ~60 周期的固件总账里（§6）。我们的 RAT 是 256 项
（x86-64 全寄存器名 + 256 上限），但只 rsave「已映射」的项（V2.5 已证明全排空后每
线程只 hold 已提交映射），故有效成本仍按「映射项数 / 4 端口」计，不按 256。

### 4.2 rsave/rrestore 在 trace 驱动模拟器里的表达（回传清单第 5 项，最非显然）

两条路，**推荐事件模型**：

- **方案 A：伪指令注入**。把 rsave/rrestore 写成注入流水线的合成指令序列（像真
  firmware 的 mov 流）。**否决**：ChampSim 是指令流来自 trace 的模拟器，寄存器操作
  数、重命名、提交都由 trace 指令驱动；注入合成指令会撕破 trace 驱动不变式、污染
  RAT/ROB/提交计数，且我们根本没有寄存器**数值**可搬（见 4.3）。
- **方案 B：事件模型（推荐）**。rsave/rrestore 是核上的一段**多周期事件**：换出后
  「rsave 事件」在 N 拍里把 RAT 快照写进 IRB 数组（消耗建模的端口带宽 + 时延，槽在
  此期间 stall），换入时「rrestore 事件」反向。不产生任何指令，不碰 trace、不改
  retired 计数。这正对应论文把 IRB 访问当硬件机制（4 端口、10 周期流水化）而不是
  指令序列。

    firmware vs instant 两档就落在事件时延上：instant = 1 拍（原子），firmware =
    按论文分步（rsave 16 + rrestore 16 + 检测/冲刷/选位开销 ~28）合计 ~60 拍。

### 4.3 value-less 简化（结构差异，照实记录）

ChampSim 的 `physical_register` 只存元数据（`arch_reg_index` / `producing_instruction_id`
/ `valid` / `busy` / `context`，register_allocator.h:16-25），**不存寄存器数值**。
所以 IRB 存的是**架构寄存器映射快照**（arch → 元数据），替代论文的 62×8 字节数值。
后果：rsave = 快照 RAT 映射 + 释放物理寄存器；rrestore = 按快照重新分配物理寄存器 +
重建 RAT。数值、ALU 结果、load 数据（除 CHAMPSIM_TRACE_MEMORY_VALUES 的 load 值）都
不在建模范围，因此「取消 load 后数据写回 IRB」在语义上等价于「请求续飞暖缓存，换入
后重执行 load 命中」——方向与论文一致（长延迟被换出隐藏），机制（数值搬运）被折叠。

---

## 5. 预测器状态（B0.4 第 5 项）

论文：per-虚拟位复制 GHR/RAS，换入换出时切换。我们现状：分支预测器是
`branch_module_pimpl` 单实例，**共享** `ghist_words`（全局历史单份），BTB 已用
`context`/`asid` 双键（impl_update_btb / impl_btb_prediction 带 `context_index` +
`address_space_id`，ooo_cpu.h:518-577）。

设计：GHR/RAS 按 **virtual_id**（不是物理槽 `context_index`）键控复制——每个虚拟位
私有一份 GHR/RAS，换入时切到该虚拟位的副本。这复用 BTB 的 `context` 键控先例，但
键从物理槽改成虚拟位。**标注（实现后需专项对比验证）**：这与 8.8× 研究线相关（预测
器私有化对 SMT 收益的贡献），换入换出会冷启动预测器，可能抵消一部分 BMT 的 load
隐藏收益——必须做「共享预测器 vs per-虚拟位预测器」的 A/B 对比，而不是默认 per-位。

---

## 6. 切换成本参数化（B0.4 第 6 项）

单旋钮 `bmt_switch`：

| 档 | 语义 | 数值 |
|---|---|---|
| `instant` | 换出/换入原子，无时延 | 1 拍 |
| `firmware` | 按论文分步时序合计 | ~60 拍（rsave 16 + rrestore 16 + 检测/冲刷/选位/门控 ~28） |

firmware 的 ~60 拍在事件模型里就是 §4.2 的「事件时延」，槽在此期间 stall（不取指、
不派发），但对兄弟槽透明（其余槽照常跑）。默认 `instant` 使 BMT 机制关闭时逐位等于
现状（无任何路径改动），firmware 是开启 BMT 后的对比臂。

---

## 7. 附录 A：B0.2 负载画像（本地参数）

### A.1 N=1 各 trace 单跑 load 延迟分布（ROI，-w 1M -i 3M）

| trace | IPC | L1 命中 | L2 命中(占L1 miss) | LLC 命中(占L2 miss) | DRAM miss(占所有 load) | LLC miss 延迟 | 主存访问/1k |
|---|---|---|---|---|---|---|---|
| sqlite | 0.80 | 91.9% | 33.7% | 8.2% | 1.98% | 103.77c | 4.96 |
| omnetpp | 0.95 | 55.8% | 98.8% | 81.9% | 0.07% | 86.94c | 0.16 |
| llvm | 1.09 | 96.8% | 84.7% | 15.6% | 1.36% | 127.06c | 2.42 |
| vpr | 1.63 | 98.0% | 15.2% | 1.1% | 0.61% | 97.72c | 1.00 |

名义层级（config）：L1D=5c、L2C=10c（→ L2 hit ~15c）、LLC=20c（→ LLC hit ~35c）、
DRAM miss ~90–127c（实测 miss_latency 含排队）。

### A.2 N=2 / N=4 下的移动（聚合，cache 计数是 num_cpus=1 的共享计数）

| 场景 | 聚合 L1 命中 | 聚合 LLC miss 延迟 | 聚合主存/1k |
|---|---|---|---|
| N=2 sqlite+omnetpp | 73.1% | 104.0c | 2.77 |
| N=2 llvm+vpr | 96.2% | 125.92c | 1.56 |
| N=4 全四 | 81.4% | **130.62c** | 2.71 |

移动信号：LLC miss 延迟从单跑 ~90–127c 升到 N=4 的 130.62c（DRAM 排队，+~26% 相对
sqlite 单跑）。命中率是聚合加权，不逐 trace 分解（受限于共享计数）。

### A.3 阈值候选（对应论文「L2 延迟 + 余量」）

论文 L2=14c、阈值 25c（余量 11）→ 35c（余量 21，等出并行第二 miss）。我们把「最后
一级 cache」对齐到 LLC：**阈值 = LLC 命中(~35c) + 余量**。候选 **46–56 周期**
（35 + 11 到 35 + 21），上界仍远低于 DRAM miss（90–130c），保证 LLC miss 一定触发、
LLC hit 不触发。B2.1 在此区间扫阈值，验证「更长的等出并行第二 miss 更优」的单调段。

### A.4 主存访问率（决定 BMT 收益上限）

我们 4 trace 均值 2.14/k（区间 0.16–4.96）；论文 Base 2.5/k。**同量级、略低**，
故 BMT 收益方向可复现，量级会打折（omnetpp 近零 DRAM 拉低均值；三级 cache vs 论文
两级、主存延迟 ~100c vs 论文 500c 都压缩可隐藏窗口）。

---

## 8. 附录 B：B0.3 基线差异审计

### B.1 对比公平性建议（leader 定）

论文的 SMT 基线自带 flush-on-miss（Tullsen & Brown 2001），因此「miss 检测与冲刷不算
BMT 额外成本」。我们基线无此机制。

建议：**给 SMT-2 对照组也加冲刷（flush-on-miss）**，使两臂对称——SMT 臂有 miss 冲刷、
BMT 臂有 miss 换出，检测/冲刷成本两边同记，BMT 只多出「换出换入的 IRB 存取 + 上下文
切换」这一真正增量。否则 BMT 的 +26% 会混入「SMT 没有的冲刷成本」而不可比。

理由：论文把冲刷视为**两臂共有**的前置；不把 BMT 的检测/冲刷记为 BMT 成本，而是
先给 SMT 基线补上同款冲刷，再对比。第二选项（把检测/冲刷记 BMT 成本）会系统性
**压高** BMT 相对 SMT 的劣势，偏离论文口径。推荐第一选项。

### B.2 结构性差异清单（文档活）

| 维度 | 论文（Alpha/SMTSIM） | 我们（ChampSim2） |
|---|---|---|
| ISA / 寄存器 | Alpha，31 int + 31 fp = **62** | x86-64 全寄存器名，RAT **256** 项 |
| 存储层次 | 2 级：L2=14c，主存=500c | 3 级：L1D=5c / L2C=10c / LLC=20c，DRAM ~90–130c |
| 流水线 | 22 级 | ChampSim OoO（fetch→decode→dispatch→sched→exec→retire） |
| 模拟器 | SMTSIM（有值执行） | ChampSim2 trace 驱动（**无寄存器数值**，见 §4.3） |
| 负载 | 论文 SPEC（Base BMT-4/6 2.5 主存/1k） | sqlite/omnetpp/llvm/vpr，2.14 主存/1k |
| 切换 | 固件 ~60 周期 | 事件模型 ~60 拍（§6） |

主存延迟从 500c 缩到 ~100c 是关键差异：论文里单 miss 占 500c 的可隐藏窗口，我们只
有 ~100c，BMT 的单次换出隐藏的周期少，但主存访问率接近，总隐藏量方向一致、量级打折。

---

## 9. B0.5 风险核对

- **V3.3 suspended=finished 未带入**（确认）：V3.3 把「phase 完成判据里 suspended 视为
  finished」（commit 8755dc67 主题「teach the phase-completion predicate that a
  suspended context is finished」）。cherry-pick drain/suspended 机制时**只取
  `drain_complete()` 结构清单 + scoped 冲刷 + suspended 门控取指**，不取「suspended →
  完成判据」这条；BMT 里换出的虚拟位是**可恢复**的，只有 trace EOF 才是 finished。
- **4 trace 上限**：C_virt 扫描只能到 4（BMT-2/2、2/3、2/4）。2/6 **砍掉或标受限**——
  同 trace 复读会引入缓存复用假象（V4.3 踩过）。建议 C_virt 上限 = trace 数 4。
- **trace EOF 欠载段（3 挤 2）度量口径**：weighted speedup 只在满载共相窗口统计，尾段
  不计入分母（§1.3），避免欠载单线程 IPC 分母污染。
- **预测器私有化的收益/代价**：per-虚拟位 GHR/RAS 换入换出冷启动，可能抵消 load 隐藏
  收益，须 A/B 专项对比（§5），不默认 per-位。
- **寄存器映射快照的元数据完整性**：rsave 需快照足量元数据以正确重建 RAT；V2.5 的
  `count_free + count_busy == SIZE` 守恒不变式可扩成「free + busy + IRB 占用 = SIZE」，
  把 IRB 纳入守恒门（core_context.cc:98 的断言延伸）。

---

## 10. B0.1 现状对表（论文机制 → 我们的现状 → 缺口 → 复用方案）

| 论文机制 | 我们的现状 (file:line) | 缺口 | 复用方案 |
|---|---|---|---|
| 换出=保存寄存器 | V2.5 `reclaim_registers` core_context.cc:83-98；触发 ooo_cpu.cc:79-86 | 现在「释放」而非「写 IRB」 | 先快照 backend_RAT → saved_rat，再照抄释放循环 |
| 全流水线排空判定 | V3.0 `drain_complete()`（smt-vsmt4 分支，查 ROB/DISPATCH/DECODE/DIB_HIT/IFETCH/LQ/SQ，不含 input_queue） | smt-bmt 无此函数 | **可 cherry-pick** 结构清单；BMT 改成主动冲刷（不等自然排空） |
| scoped 冲刷（RAT 回滚） | `reset_frontend_RAT()` core_context.cc:68-73（frontend←backend） | 无清缓冲+释放 in-flight 的组合 | reset_frontend_RAT 照抄；补清缓冲 + 释放非 backend_RAT 的物理寄存器 |
| 换入=恢复取指位置 | V3.4 input_queue 归档（smt-vsmt4，残量就地冻结/归档 + 空 map 重建） | 单次版本，未泛化到多虚拟位 | 泛化成「归档到 virtual_thread 记录 + 换入迁回」 |
| 换出线程不占 ROB 份额 | V2.2 `rob_partition=active` rob_capacity() ooo_cpu.cc:592-612（ROB_SIZE / max(1,active)） | 换出后 active 计数正确性待确认 | 换出=ROB 清空 → 自动不占 active；确认 LQ/SQ 也纳入 active 判据（现只数 ROB 非空） |
| 断言体系 | PRF 守恒 free+busy=SIZE ooo_cpu.cc:104；ROB 总容量守恒 ooo_cpu.cc:96 | 未覆盖 IRB | 扩成 free+busy+IRB_占用=SIZE |
| （勿带入）suspended=finished | V3.3 完成判据把 suspended 视 finished（commit 8755dc67） | — | **不 cherry-pick 该判据**；BMT 换出可恢复，仅 EOF=finished |

**cherry-pick 清单**（从 smt-vsmt4 → smt-bmt）：
1. `drain_complete()` 的「结构清单」（去掉「等待」语义，保留「哪些缓冲/队列构成排空」）。
2. `suspended` 门控取指的形态（initialize_instruction_impl 开头 `if (suspended) return`），
   BMT 换成「换出槽不取指 / 空槽不取指」。
3. scoped 冲刷里「清缓冲 + 释放 in-flight 物理寄存器」的循环。

**不 cherry-pick**：`run_mode_state_machine()` 的 QUIESCE/DRAIN/RECONFIGURE 五态机、
`mode` 静态计数、V3.3 suspended→finished 判据、V4.2 stall_remaining、V5 的权重/槽位。
这些是「运行时模式切换」线的机制，BMT 是「固定物理槽 + 虚拟位换入换出」，判据不同。
