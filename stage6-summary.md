# Stage 6 总结（多核 num_cpus=2：落地、测试、归档）

> 代码基线：`~/ChampSim2` 的 `smt-stage6-multicore` 分支（叠在 Stage 5 顶端 `16d0ca59` 上，HEAD=`ff64f82e`）。
> 姊妹文档：`stage3-summary.md`、`stage4-summary.md`、`stage5-summary.md`（均在本仓库根）。
>
> **多核能力已实现并测试通过，但按 leader 指示不做多核、方向转 vSMT4，本分支归档。** 本文件是 Stage 6 的终版沉淀记录，覆盖 6.1 → 6.6 全链；多核能力保留为可回捡的历史（recoverable history），不合入 SMT2/vSMT4 主线。

---

## 1. 目标

ChampSim2 从单核（`num_cpus=1`）推进到双核（`num_cpus=2`），6.1~6.5 全部落地、测试全绿（最终 `make test`：1033 用例 / 1013 通过 / 0 失败 / 20 跳过，断言 118725 全过）；按 leader 「不做多核」的指示，本阶段以归档收口，多核能力保留为可回捡的历史。

---

## 2. 分步清单（commit 表）

| 步 | 做了什么 | commit | 新增测试 | 红转绿 |
|---|---|---|---|---|
| 6.1 do_cycle 稳定排序 | `std::sort` → `std::stable_sort`（`src/champsim.cc`）：libstdc++ 对 >16 个元素不保稳定，双核机器 operable ≥18，等时遍历顺序必须确定。作废旧 `9ab9eed9`、重开为 cherry-pick 版 | `b4a35874` | —（N=1 下 10 个 operable 插入排序本已稳定，由三把锁实跑锁死：8dcc/9bb1/8dcc 逐字节不变） | — |
| 6.2 ittage per-core 键控 | `shared()` 从 `array<impl, max_contexts>` 升为 `array<array<impl, max_contexts>, num_cpus>`，`core_of(intern_)` null-safe 取核号，索引 `[core][context]`：消除 context-only 键控的跨核别名（隐患 B） | `df4b1735` | —（186 的 `intern_==nullptr` 路径由 `core_of()` 兜住，全绿） | — |
| 6.3 501 补 cpu1 键 | `501-static-environment.cc` 的「Every parameter」键清单从硬编码 cpu0 改为按 `defs::num_cpus` 循环生成；N=1 下与原 142 键 golden 逐元素按序相等（`if constexpr` 钉死），N=2 自动扩到 249 键把 cpu1 全部 pin 住。「先有守卫，再翻旋钮」 | `d3f2521a` | 501 内新增等价断言（+1） | — |
| 6.4 翻 num_cpus=2 | `inc/defs.h:23` 一行：`num_cpus 1 → 2`。单核对照按新准则通过（架构逐位、cycles +28~30 = 0.002% 时序残差且确定性可复现）；核间对称三层容差全过（结构 0 / 事件 ≤1% / 时序 ≤5%，实测 IPC skew 0%、cycles skew 0.011%） | `db02cae8` | —（验证靠 run-accept 对照跑，非单测） | 601 转红（2 用例 / 3 断言，单核假设被打破，留 6.5a） |
| 6.5a 601 守卫 + 604 裁决 | 601 fixture 给 `cpu1_ptw` 也设 `model=fixed`（+1 行）；新建 604 裁决「detailed 下两核 PTW 是否互相顶替」→ **否**：数据页按 `{asid,vaddr}` 键、页表页按 `{asid,level,index}` 键均不顶替，唯一共享项 CR3 根无害（PTE 内容按请求 asid 重算），`CHECK(CR3_addr==CR3_addr)` 作哨兵钉住共享事实 | `e917dda1` | `604-multicore-ptw.cc`（3 CHECK） | 601 的 2 红用例 / 3 红断言全部转绿（1027/1005/2 → 1028/1008/0） |
| 6.5b 187 跨核隔离 + 605 红线 | 187 钉 ittage 跨核隔离（两 core 同 ip/context/asid 训不同 target，各自读回——键控回退成 context-only 立刻红）；605 端到端红线用例：真 `static_environment` 两核喂同内容合成 trace，asid {0,0}/{1,1}，断言同一虚页落到**不同**物理页 | `39e26b70` | `187-ittage-cross-core-isolation.cc`（2 用例 8 REQUIRE）+ `605-multicore-same-trace.cc`（1 用例 2 REQUIRE + 3 CHECK） | —（1028/1008 → 1031/1011，+13 断言分毫不差） |
| 6.5c 空 trace 兜底 | `bulk_tracereader::operator()` 在 `refill()` 后若 buffer 仍空，返回合成空标记 `ooo_model_instr{max, input_instr{}}`（asid {max,max}，与任何位置烙印记录互斥），不再对空 deque `front()`（UB）；`eof()` 随即变真 → do_cycle 停喂 → phase 完成判定走 eof 收口。guard 对 ≥1 条记录的 trace 是死分支，正常读取路径逐字节不变。另在 `main.cc` 注释钉死 -i 回卷契约（注释-only） | `ff64f82e` | `901-tracereader-empty.cc`（2 用例 6 REQUIRE：0 字节 / 不足一条记录两种零记录形态） | 空/截断 trace 从 UB（段错误）转绿（1031/1011 → 1033/1013，+6 断言） |

至此 Stage 6 的三个行为改动（6.1 stable_sort / 6.2 ittage per-core / 6.4 num_cpus=2）每一个都有会红的测试锁住（分别为三把锁控制、187、601+604+605），加上 6.5c 的 901——每改一行都有判据。

---

## 3. 三个关键设计决策（+为什么）

### 决策一：单核对照验收准则收窄——「cycles 逐位」退役，改「架构逐位 + cycles ≤0.01% + 确定性」

翻 `num_cpus=2` 后，operable 从 10 个变 18 个，`do_cycle` 的 stable_sort 等时遍历顺序**必然**改变（这正是 6.1 要保证的确定性顺序，但它不再等于单核顺序），core0 的 cycles 从构造上就不可能逐位等于单核 build。「cycles 逐位」是三把锁时代「逐字节」门槛的误搬，正式退役。耐久验收准则（写死，Stage 6 所有单核对照照此，也留给后续任何翻编译期常量的阶段）：

1. **架构计数器逐位**：IPC、数据通路全链路（各级 cache/TLB/DRAM）、branch 六类 census、warmup——一个不能漂；
2. **cycles 容差 ≤0.01%** 且必须**确定性可复现**（观测 0.002%，留 5× 余量）;
3. 若 cycles 漂，必须证明「架构逐位、仅时序漂」（举证范例见 §4 的 prf64 判例）。

### 决策二：红线——负载必须「多程序、物理地址不重叠」

**负载必须「多程序、物理地址不重叠」；跨核共享物理地址的负载结果无效、不得引用。**

为什么：ChampSim2 没有一致性协议。trace-driven 下跨核共享物理地址不会崩、不会报错——它只会让两个核读到彼此陈旧的写入（stale read），把结果做得不现实地好。这是最危险的一类错误：静默地给出漂亮数字。红线原文逐字写进 605 测试注释；605 钉的是红线的「安全半边」（多程序物理不重叠机制成立：同一虚页在 asid {0,0}/{1,1} 下落到不同物理页），并显式声明它不测、也无法在单测里测「危险半边」（共享地址负载失效）——那是缺一致性协议的问题，不是可检的不变量。

### 决策三：601 修法选 option A（cpu1 也设 fixed），但 604 独立裁决，堵死「修测试静默盖住隐患」的口子

6.4 翻旋钮后 601 转红，根因是单核假设（详见 §4）。修法是给 fixture 的 `cpu1_ptw` 也设 `model=fixed`——但这有一个风险：如果红的真正原因是「detailed 下两核 PTW 互相顶替」，补 fixed 就是把测试修绿、把隐患盖住。所以 6.5a 把「修 601」和「裁决顶替问题」拆成两件事：601 补 fixed 只处理 fixture 对称，604 用三条断言独立裁决 detailed 模式——数据页不顶替（`{asid,vaddr}` 键）、页表页不顶替（`{asid,level,index}` 键）、唯一共享的 CR3 根无害（PTE 内容由 `finish_packet` 按请求 asid 重算，不读返回 data）。裁决通过，601 的修法才成立；604 同时留下 `CHECK(CR3_addr == CR3_addr)` 哨兵——将来若有人把共享根修成每核独立，这条 CHECK 会红，逼着显式面对，而不是静默改掉一个没人记得的硬编码。

---

## 4. 诚实发现

### 发现一：prf64 判例——「时序残差」的标准举证范例

prf64 压力点 L1I/ITLB 的 fetch 数逐位为 0 差异（503887 == 503887），但 cycles 照样 +30。这**切断了「多 fetch → 多拍」的因果链**：如果 cycles 漂来自多取指，fetch 逐位相同的 prf64 就不该漂；它漂了，就证明这 +30 拍不是数据通路/取指路径的 bug，而是 `num_cpus=1→2` 的结构性时序残差（每周期 do_cycle 的 stable_sort 等时遍历顺序、DRAM 625ps 慢钟 catch-up 的相对位置发生确定性微移）。这是判断「时序残差而非数据通路 bug」的决定性证据，后续任何单核对照遇到 cycles 漂，照此举证：先证架构逐位，再找一条「上游流量逐位相同、cycles 仍漂」的压力点切断因果链。

### 发现二：config-symmetry 更正——1.65% IPC skew 是配置不对称，不是核间串扰

核间对称验证初次跑出 1.65% IPC skew，几乎被误判为「翻旋钮引入核间串扰」。实查：`sample.toml` 只设了 `[ooo_cpu.cpu0]`/`[cache.cpu0_l1d]`，cpu1 没有任何覆盖 → 回落默认值（rob_size 352 对 512、fetch_width 6 对 8、basic_btb 1024 对 2048、dib 32/8 对 64/4、l1d 64/16 对 128/32）——两核根本不是同一台机器。给 cpu1 显式补齐七个键后重跑：三层容差全过（事件计数逐位一致、IPC skew 0%、cycles skew 0.011%）。教训：多核对照实验的第一道检查是 `[config]` 键集对称，配置不对称会伪装成一切「干扰」结论；先对齐 config，再谈串扰。

### 发现三：601 根因——cpu1 的 detailed PTW 构造分配 CR3 根页，顶掉硬编码地址期望

601 硬编码「第一个未随机化数据页 = 0x100000」只在单核成立：fixture 没设 `cpu1_ptw.model`，num_cpus=2 下 cpu1 的 PTW 走默认 detailed 模式，其构造器用 `get_pte_pa` 从 vmem 空闲页表 pop 掉一页做 CR3 根，把期望地址顶到 0x101000。分类是「单核假设被打破」，不是 6.4 改坏——detailed PTW 分配根页表是正确行为（它需要一张根页去 walk），是测试没料到 cpu1 还会有一个 PTW。这个根因直接催生决策三的两段式修法。

---

## 5. N=1 三把锁 + 单核对照准则

**三把锁自 6.4 起只约束 `num_cpus=1` 的 build；`num_cpus=2` 是另一个 build，正确性改由「核间对称 + 单核对照」收口，不拿 `num_cpus=2` 的数去和 8dcc/9bb1/8dcc 比对判失锁。**（`num_cpus` 是编译期常量，翻 2 那一刻 num_cpus=1 的二进制不复存在——这是「合法失锁」，不是回归。）

锁值（`run-accept.sh`，6.1/6.2/6.3 每步实跑复验，逐字节不变）：

- baseline  `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6`  ipc 2.34
- prf64     `9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56`  ipc 1.778
- sched64   `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6`  ipc 2.34

耐久单核对照准则（6.4 定案，写死）：**架构计数器逐位一致 + cycles ≤0.01% 且确定性可复现 + cycles 漂时必须举证「架构逐位、仅时序漂」**。此准则独立于本分支的归档状态，适用于后续任何翻编译期常量（核数、context 数、块大小）的阶段。

---

## 6. 诚实边界（含后继债，逐条落地）

1. **共享 CR3 根 = option 2 已知缺陷（记录，不在本分支修）**：`static_environment.cc:291` 两个 PTW 都构造为 `.asid({0,0})`（Stage 2 single-context 时代的硬编码），num_cpus=2 下两核共享 CR3 根，cpu1 的顶层 walk 有一次跨核假读。604 已裁决翻译结果正确、无数据通路影响（PTE 内容按请求 asid 重算）。604 的 `CHECK(CR3_addr == CR3_addr)` 是钉住它的哨兵。**后继债：协作线程项目启动时一并改 asid 注入**（届时 `static_environment.cc:291` 按 trace 槽位注入对应 asid，604 哨兵转红、显式改判「!=」）——不在 vSMT4 主线单独修，因为 vSMT4 单核多 context 下 `.asid({0,0})` 语义无害。
2. **idle-trace 回卷 = -i 语义，非缺陷（文档债）**：`main.cc` 的 `repeat = simulation_given` 使短 trace 在给了 `-i` 时无限回卷直到指令数满足——这是 -i 的契约（trace 不够、指令数来凑），不是 bug，刻意不 guard。空 trace + -i 因此回卷而无进展，属用户错误、超出范围。`main.cc:373` 旁的注释已把语义钉死在代码侧（随 `ff64f82e` 进 git），本条是它的正式文档记录。若未来协作线程项目要让空 trace + -i 干净退出，那是加 feature，不是补债。
3. **多核「空核」场景未做（随多核方向一并归档）**：一个核喂空 trace、另一个核正常跑的 idle-core 配置没有端到端验证过。6.5c 的兜底机制（空标记 + eof 收口）已构成其前置——空 trace 的核会收到可识别的合成空标记、phase 完成判定走 eof 收口、不崩——但「支持空闲核」本身按 leader 指示不做，随多核能力一并归档为可回捡的历史。
4. **分支归档，不合入主线；两笔通用修复待 cherry-pick 到 vSMT4**：按 leader 指示，`smt-stage6-multicore` 归档、不合入 SMT2/vSMT4 主线。但其中两笔修复不是多核专属、是通用 bug 修复，vSMT4 开工时从本分支 cherry-pick：
   - `b4a35874`（6.1 stable_sort）：任何 operable >16 的 build 都需要确定性遍历顺序，与核数无关；
   - `ff64f82e`（6.5c 空 trace 兜底）：单核喂空文件同样段错误，是真 UB 修复。

---

**Stage 6 至此全部收口。** 6.1 ✓(`b4a35874`) → 6.2 ✓(`df4b1735`) → 6.3 ✓(`d3f2521a`) → 6.4 ✓(`db02cae8`) → 6.5a ✓(`e917dda1`) → 6.5b ✓(`39e26b70`) → 6.5c ✓(`ff64f82e`) → 6.6 本文件归档。
