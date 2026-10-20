# vSMT4 V2 笔记（槽内弹性：ROB 分母 mode → active）

> 分支 `smt-vsmt4`，起点 `82e0b223`（tag `vsmt4-v1`）。V2 = 差距③ 行为层复现：ROB
> 分母从「按座位数 mode」换成「按实际活跃数 active」。只动 ROB 一个资源。
> 本文记 V2.1 的 active 抖动画像（V3.5 去抖窗口设计的输入）与 V2 后续的实测。

---

## 1. V2.1：active 抖动画像（N=4 两组，每变化点采样）

口径：`configs/sample.toml` + `configs/vsmt4-n4.toml`（num_contexts=4，prf=256），
`-w 1M -i 3M`。`active` = 每周期末 ROB 非空的 context 数（V1.5 埋的字段），只采样
不处理（TEMP 每变化点 dump，已回退）。ROI 边界：fotonik 暖机止于 cycle 1201136、
ROI 4889832；hetero 暖机止于 1479565、ROI 8137219。

### 1.1 active 时间加权分布（ROI 内）

| active | fotonik3d×4（4.89M cycles） | hetero×4（8.14M cycles） |
|---|---|---|
| 4 | 58.87% | 63.58% |
| 3 | 31.65% | 30.63% |
| 2 | 8.28% | 5.38% |
| 1 | 1.14% | 0.40% |
| 0 | 0.06% | 0.01% |

**active=4 只占 ~59–64%**。一个 context 的 ROB 空掉（active=3）占 ~31%，两空 ~5–8%。
说明「ROB 非空」这个信号在满负荷下也**频繁掉档**，不是干净的「4 槽全占」。

### 1.2 掉档事件统计（active<4 的极大区间）

| | fotonik3d×4 | hetero×4 |
|---|---|---|
| 掉档区间数 | 207 587 | 266 948 |
| 区间时长 min / median / mean / max（cycle） | 1 / 7 / 9.7 / 534 | 1 / 6 / 11.1 / 1079 |
| ≤5 cycle（瞬时抖动） | 82 615（40%） | 132 926（50%） |
| ≤50 cycle | 206 965（99.7%） | 260 540（97.6%） |
| >500 cycle（真长 drain） | 1 | 188 |
| min-active 深度 {0/1/2/3} | 1354/15574/57644/133015 | 434/8776/58357/199381 |
| 未满负荷（active<4）总周期占比 | **41.13%** | **36.42%** |
| 落在 ROI 末 10% 的区间占比 | 10.0% | 10.7% |

### 1.3 结论（喂 V3.5 去抖窗口）

1. **掉档不是集中在尾部**：两组末 10% 只占 ~10%（≈ 均匀），掉档贯穿全程。掉档来源
   是**前端瞬时 stall**（共享 hashed_perceptron 污染 → mpki 32.8 → 频繁 mispredict
   flush + I-cache miss），不是「线程跑完」。
2. **绝大多数掉档是浅而短**：min-active=3（单 context 短暂空 ROB）占 64%（fotonik）/
   75%（hetero）；时长 median ~6–7 cycle，≤50 cycle 占 97.6–99.7%。这是 V3.5 要
   滤掉的「瞬时抖动」。
3. **真长 drain 稀少但存在**：fotonik 仅 1 个 >500，hetero 有 188 个 >500——后者是
   异 trace 组各线程**先后跑完**的尾部 drain（vpr 4.16M 先完、sqlite 8.14M 最后）。
   去抖窗口必须保住这类（不能把「真空闲」也滤成「还活跃」）。
4. **给 V3.5 的窗口量级参考**：median 6–7 cycle、mean 9.7–11.1 cycle。窗口取 ~8–16
   cycle 可滤掉约一半瞬时抖动；要滤掉 99% 得 >50，但那会吞掉真实的中短 drain。
   具体窗口须 V3.5 用 V2.4 的欠载场景（截短 trace 的确定性 drain）标定，不要只看
   满负荷抖动分布。

---

## 2. V2.2 ROB 分母换血（mode → active，本 Stage 唯一代码改动）

### 2.1 改动点（file:line）

| 位置 | 内容 |
|---|---|
| `inc/core_builder.h:49` | `enum class rob_partition { equal, active };` 新增 `active` 枚举值 |
| `src/static_environment.cc:203-206` | `configured_rob_partition` 接受 `"active"`；拒收值改报 `must be 'equal' or 'active'` |
| `inc/ooo_cpu.h:184,191` | 声明 `rob_capacity()`（已有）+ 新增 `rob_occupied()`（Σ 占用） |
| `src/ooo_cpu.cc:576-577` | `rob_capacity()` 新增 `active` case：`ROB_SIZE / std::max<std::size_t>(1, active)` |
| `src/ooo_cpu.cc:584-591` | 新增 `rob_occupied()`：`Σ_k size(contexts[k].ROB)` |
| `src/ooo_cpu.cc:621` | dispatch 循环加入双门②：`rob_occupied() < ROB_SIZE`（聚合防超卖） |
| `src/ooo_cpu.cc:74` | operate() 末断言：`CHAMPSIM_ASSERT(rob_partition_ != active \|\| rob_occupied() <= ROB_SIZE)`（仅 active 模式守卫守恒） |
| `configs/vsmt4-n4.toml:28` / `configs/vsmt4-lock4.toml:33` | 两组 V2 跑加 `rob_partition = "active"` |
| `test/python/test_operational_config_errors.py` | 新增 `test_rob_partition_accepts_active`；拒收消息同步 |

### 2.2 双门语义（防超卖）

`active` 是 operate() 末（`src/ooo_cpu.cc:66`）刷新的**上一周期**值，故分母滞后
dispatch 一个周期：上一周期 active=1 时，本周期另一 context 首条指令入 ROB 会让
`active` 仍为 1，单门①（`size(c.ROB) < rob_capacity()`）会给它 `ROB_SIZE/1` 的全额
额度。若只有门①，两 context 各可逼近 `ROB_SIZE`，聚合 > `ROB_SIZE`。门②
`rob_occupied() < ROB_SIZE` 把聚合占用（含本周期已准入）锁在预算内，超容量只停**新
准入**、从不清既有（在飞指令正常退休）。

门②在 `equal` 下恒真（每 context 定额 `ROB_SIZE/num_contexts` 已守恒总量），逐位零
行为；`rob_occupied()` 无副作用，断言与门②均不改变任何已记录结果。断言**只挂在
`active` 模式**：`equal` 守恒由定额结构性保证，且单测（`252-rob-execution-prefix.cc`
等）用 builder 默认 `rob_size` 直接注入 ROB 条目（绕过 dispatch），per-context 定额
反映不了，故不加无条件断言。

### 2.3 审计：ROB 是唯一刚性切分

全仓 grep `contexts.size()` / `num_contexts`，确认**只有 `rob_capacity()` 把单一预算
按 context 数切分**（`src/ooo_cpu.cc:575` equal case）。其余共享资源都不是固定
per-context 定额：

- **寄存器堆**（`shared_prf`）：共享池，用 `count_free_registers()` + Σneed 水位线
  守恒，不切分。
- **调度器**：共享 `SCHEDULER_SIZE` 带宽，`occupied < capacity` 每周期守恒 + 轮转，
  非 per-context 定额（V5 弹性项）。
- **LQ/SQ/DIB/DISPATCH_BUFFER/DECODE_BUFFER**：`core_context` 每 context 全尺寸复制
  （`inc/core_context.h:91-92`），非切分。
- **前端 fetch/decode/DIB、退休 RETIRE_WIDTH**：共享带宽 + 轮转，非定额切分。

即 V5 延迟的「LSQ/前端/调度器弹性」确为独立资源，本次只动 ROB，一变量一变量来。

### 2.4 N=1 退化校验（PASS）

`active ∈ {0,1}` → `max(1, active) = 1` → 公式 = `ROB_SIZE/1` = 原式。实测：baseline
（N=1 fotonik3d）加 `--set ooo_cpu.cpu0.rob_partition=active` 的归一化 sha256 =
`8dcc3227...`（与 equal 基线逐位一致），IPC 2.34，stderr 0 次断言。**active 模式在
N=1 下逐位等于 equal**。

---

## 3. V2.3 锁复跑（V2.2 后立即，协议见 V2 开工指令）

口径：锁文件 `$HOME/SMT-experiment/stage1/vsmt4-v2.3/`。归一化 sha = stdout
`grep -vE 'Simulation time:| runs /|\.champsimtrace'` 后 `sha256sum`。二进制 = V2.2
（`23e974f0`），跑 N=4 前已回退 TEMP active-transition dump、重建至干净态。

### 3.1 三把 N=1 锁（逐位，PASS）

| 锁 | sha256（norm） | 判定 |
|---|---|---|
| 锁 1 baseline | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 逐位一致 |
| 锁 2 prf64 | `9bb10576f07f5e12983d7711a01ad2811be871b9f76b9f155ce46986a659ea56` | 逐位一致 |
| 锁 3 sched64 | `8dcc32274d22bdbfc687873ce9a30654dd81bd3694a1f8d2f613bdc2202094f6` | 逐位一致 |

与 V1 基线（`vsmt4-v1-summary.md` §1）逐位相同。active 模式在 N=1 下退化为
`ROB_SIZE/1`，三个锁全部复现 V1 值。

### 3.2 锁 4（N=2 fotonik3d×2）三项协议

| 协议 | 结果 | 判定 |
|---|---|---|
| ① ×2 确定性 | `ca19795f57b008f4cab12a55e6bcb0adcec1fc996007bfe02836af7bec7c9491` ×2 | PASS（逐位） |
| ② 全跑 sha vs V1 | `ca19795f...` ≠ V1 `71b7dd7a...`；equal 对照 = `71b7dd7a...`（逐位复现 V1） | 变化确属 active，见下 |
| ③ 稳态共驻窗口 IPC | active=2 段分母恒 256 = equal 定额，行为逐位同 V1 | 模型级 PASS，见下 |

**①** 两跑逐位一致（`lock4-run1.norm` == `lock4-run2.norm`）。

**②** 变化量：cycles 2828581 → **2826242**（**−0.083%**），ctx1 指令 3014764 →
3002387。equal 对照（当前二进制 `rob_partition=equal`）归一化 sha =
`71b7dd7a172c1f38849d84fc36b2ae8c7d479773a87b781906f06a2020eea77d`，**逐位复现 V1
值**，证明变化**唯一**来自 `active` 分母。断言（`rob_occupied() <= ROB_SIZE`）全程
0 触发。

**② 的关键偏差（对 V1.5 存档的修正）**：锁 4 协议②假定「对照 V1.5 存档：稳态
299/340 采样恒 2、尾部 drain 掉 1→0」，即 active<2 只在尾部。**per-change dump
推翻了这一前提**。`lock4-active-dump` 全程逐变化点记录 active，ROI 内统计：

| 量 | 值 |
|---|---|
| ROI cycles（active 跑） | 2826242 |
| active<2 区间数（ROI 内） | 118 770 |
| active<2 总周期 | 512 196（**~18.1%** of ROI） |
| 区间时长 min / median / mean / max | 1 / 4 / 4.3 / 169 |
| ≤5 cycle（瞬时抖动） | 90 042 段 |
| 6–50 cycle | 28 720 段 |
| >50 cycle | 8 段 |
| 落在 ROI 末 10% 的 active<2 | 48 499（仅 **9.5%**） |

active<2 占总 ROI **18%** 且**贯穿全程**（末 10% 只占 9.5% ≈ 均匀分布，**非尾
concentrated**），是 ~11.9 万段 median 4 cycle 的**前端 stall 瞬抖**，不是「尾部
drain」。V1.5 存档的「稳态恒 2、尾部掉 1」是 **10K 周期采样伪像**：median 4 cycle
的掉档在 10K 采样下命中率 ≈ 4/10000，全程不可见；只有尾部掉档变长（8 段 >50
cycle，最长 169）才被采样撞见。所以**弹性在 ROI 全程被反复（短暂）触发**，非仅
尾部。

**③** 模型级验证：`rob_capacity()` 在 `active=2` 时 = `ROB_SIZE/2 = 256`，与 equal
定额逐位同值；故 active=2 的周期里分母不变、行为与 V1 逐位相同（equal 对照已证
V2 二进制在 equal 路径逐位 = V1）。变化**全部**源于 active<2 周期（分母 512≠256）。
注意：active<2 的**第一段在暖机期就出现**（cycle 2355 active=1，全跑第 3 条
transition；暖机止于 ~572871），故 V1/V2 在暖机期即播种分叉、进 ROI 前已非逐位；
ROI 内首个 active<2 是 572887（ROI 起 16 cycle）。分叉后即使回到 active=2，轨迹也
因早前弹性微扰级联偏移；故「稳态 IPC 与 V1 逐位一致」只在**首个 active<2（暖机
2355）之前的共驻前缀**严格成立，其后是 ≈0.08% 量级的级联差，非逐位。

### 3.3 N=4 前后对比（equal V1.6 vs active，同 config）

| config | 指标 | equal（V1.6） | active（V2.2） | Δ |
|---|---|---|---|---|
| fotonik3d×4 | cycles | 4 889 832 | 4 886 027 | **−0.078%** |
| fotonik3d×4 | 合计 IPC | 2.458 | 2.459 | ~0 |
| hetero×4 | cycles | 8 137 219 | 8 404 813 | **+3.29%** |
| hetero×4 | 合计 IPC | 1.895 | 1.850 | **−2.4%** |

**fotonik×4（同构）**：cycles −0.08%，中性。4 槽同 trace 近同步，掉档对称、回填
快，弹性微扰净趋零。

**hetero×4（异构）**：**回退 +3.29% cycles / −2.4% IPC**。per-context IPC：sqlite
0.3687→0.3569（−3.2%）、omnetpp 0.3762→0.3588（−4.6%）、llvm 0.4214→0.4050（
−3.9%）、**vpr 0.7287→0.7295（≈0）**。三个慢线程集体掉 3–5%，快线程 vpr 纹丝不动
——正是**再入场饥饿**（re-entry starvation）签名：弹性分母撤销了每线程**保底定额**
（equal 每槽恒 128），active 掉档时存活线程吃满 `ROB_SIZE/active`（如 active=3 →
各 170，Σ=510），第 4 个线程掉档后回填时只剩 2 个空位（门② `rob_occupied() <
ROB_SIZE` 卡死），直到有存活线程退休腾位；慢线程（内存 stall 频繁 → 频繁掉档）最
先被饿，快线程 vpr 全程吃饱。这是 V2 开工指令 V2.2「风险：活跃数抖动 → 容量抖动，
去抖属 V3、V2 先记录不处理」的**量化落地**，非 bug，不顺手修。

### 3.4 V2.3 结论

1. 三锁逐位 PASS，锁 4 确定性 PASS，equal 对照逐位复现 V1 → **active 是唯一变化源
   、N=1/N=2 稳态零回归**。
2. 锁 4 变化 −0.08%，N=4 同构 −0.08%，量级一致且近零：**满负荷共驻下弹性近中性**。
3. **异构满负荷回退 +3.3%**：弹性撤销保底定额 + 门② → 掉档线程再入场饥饿。这是
   V2 规格已预告的「抖动容量」现象，去抖（V3）前就是这个代价；也是 V2.4 欠载验收
   的关键对照——**满负荷短抖是弹性的代价区，持续欠载才是弹性的收益区**（V2.4 用截
   短 trace 制造持续欠载去测收益）。

---

## 4. V2.4 欠载验收（截短 trace → EOF → 持续欠载）

口径：`configs/sample.toml` + `configs/vsmt4-n4.toml`（num_contexts=4, prf=256,
rob=512, rob_partition=active）+ `--set sim.trace_repeat=false`，`-w 1M -i 3M`，
`--heartbeat-frequency 250000`。截短 trace = fotonik3d 前 2M 记录
（`fotonik3d-2M.champsimtrace`，128 MB，暖机 1M + 量测 1M 后 EOF）。equal 对照 =
`--set ooo_cpu.cpu0.rob_partition=equal`。

前置改动：在 V2.2「本 Stage 唯一代码改动」之外新增一个**驱动级旋钮**
`sim.trace_repeat`（默认 true = 现行 -i 回卷契约；false = 截短 trace EOF 掉干而非
回卷）。这是 V2.4 制造持续欠载的前提——`-i` 与回卷耦合，截短 trace 没有它就回卷、
永不到 EOF。零默认行为：N=1 baseline 锁复跑逐位一致（`8dcc3227...`）。属回传清单
第 7 项偏差申报。

### 4.1 参考点（单/双线程）

| 参考 | 口径 | IPC |
|---|---|---|
| standalone prf256 | N=1 fotonik3d，rob512 | 2.65 |
| standalone prf256 rob128 | N=1，ROB 定额 128 | 2.481 |
| standalone prf128（baseline 锁） | N=1，默认 prf | 2.34 |
| N=2 ref（co-fotonik-dual-v15） | fotonik3d×2 | ctx0 1.061 / ctx1 1.066（合计 2.127）|

register-file 敏感度（N=1 fotonik3d 4M，rob512，`-w 1M -i 3M`）：prf
48/64/96/128/192/256 → IPC 0.706/1.778/2.216/2.34/2.58/2.65。**fotonik3d 对 prf
相当敏感，prf256 才饱和**——这是 4.5 根因分析的关键。

### 4.2 场景 A（ctx0/1 full + ctx2/3 截短）

| config | cycles | ctx0 / ctx1 / ctx2 / ctx3 IPC |
|---|---|---|
| equal | 3 667 043 | 0.9437 / 0.8181 / 0.2725 / 0.2721 |
| active | 3 669 542（+0.068%） | 0.9464 / 0.8175 / 0.2725 / 0.2709 |

尾部（两存活线程，active=2 → cap 256 = equal N=2 定额）：aggregate heartbeat IPC
~2.13–2.22，per-thread ~1.1 ≈ N=2 ref 1.06（+3%）。**判 ② PASS**：欠载到 2 线程时
尾部回涨到 N=2 参考水平，弹性与 equal 定额逐位同值（V2.3 §3.2 ③ 已证）。

### 4.3 场景 B（ctx0 full + ctx1/2/3 截短）

| config | cycles | ctx0（幸存）IPC |
|---|---|---|
| equal | 2 536 217 | 1.183 |
| active | 2 536 393（+0.0069%） | 1.183 |
| active prf512 | 2 399 218 | 1.25 |

尾部幸存线程 heartbeat IPC（aggregate 8M→10M，只剩 ctx0）：**~2.2**（prf256）、
**~2.44**（prf512），对照 standalone **2.65**。**判 ① FAIL**：幸存线程未达
standalone，prf256 下差 ~17%、prf512 下仍差 ~8%。**equal 与 active 逐位一致
（Δ<0.01%）**——弹性 ROB 对欠载幸存线程的吞吐**几乎零作用**。

### 4.4 判 ④：容量 dump（active → cap）

b-active stderr 全程逐变化点记录 `[CAP] cycle active cap`（TEMP 埋点，已回退）。
unique (active,cap) 计数：`active=1 cap=512` ×11 673、`active=2 cap=256` ×40 850、
`active=3 cap=170` ×101 766、`active=4 cap=128` ×69 761、`active=0 cap=512` ×4 560。
**判 ④ PASS**：尾部明确出现 `active=1 cap=512`（ROB 分母确达 SIZE/1），弹性分母
本身工作正常。

### 4.5 结论：ROB 弹性必要但不充分（判 ① 失败根因）

1. **分母侧（④）PASS、分子侧（①）FAIL**：ROB 容量确达 SIZE/1，但幸存线程吞吐
   **未随之上涨**。欠载下 equal 与 active 逐位一致（Δ<0.01%），说明幸存线程的瓶颈
   **不是 ROB**，而是另一个共享资源。
2. **主因 = 寄存器堆粘滞**：`shared_prf` 共享池按「新写退休才释放」规则运行，线程
   掉干（EOF）后**不再产生新写 → 其架构寄存器永不复用**。3 个死线程各自扣住其 prf
   配额，幸存线程在 prf256 下只剩 ~115 可用寄存器。证据：prf512 把幸存尾部从 2.2
   抬到 2.44。V2.2 §2.3 审计把寄存器堆记为「共享池 + 水位线守恒，不切分」，
   **漏记了这条粘滞**——即回传清单第 6 项的「漏网刚性点」。
3. **残余 ~8%（2.44 vs 2.65）**：prf512 下仍差 8%，归因于另一非弹性共享资源，最
   可能是 4 路同 trace 共驻对共享 hashed_perceptron 的污染（perfect_branch 因
   「namespace 全局态、单 core/context 专用」无法用于多 context 隔离验证）。
4. **结论**：V2 只动 ROB 一变量，**POWER 14.2 欠载不坍缩（under-commit
   non-collapse）没有复现**。欠载收益要求寄存器堆（以及可能的取指/预测仲裁）同样
   弹性化——这正是 V5 推迟的范围，本次给出量化下限：**弹性 ROB 对欠载幸存线程的
   吞吐贡献 ≈ 0**，收益被寄存器堆粘滞全部吃光。

---

## 5. V2.5 线程完成寄存器回收（粘滞 PRF 缺陷修复）

### 5.1 缺陷与修复定位（定性）

V2.4 场景 B 判① FAIL 的根因是**共享 PRF 粘滞**：释放规则是「同一架构寄存器的新写
退休后才释放旧物理寄存器」，线程 EOF 掉干后不再产生新写 → 其最终映射（fotonik3d
实测 **45 个提交态物理寄存器/线程**）永久扣住。3 个死线程扣住 135/256，幸存线程在
prf256 下只剩 ~115 可用（standalone 需 192–256 才饱和）。

- **性质：lifetime 修复，不是 PRF 弹性**（弹性 = V5.5 硬上限/总量缩放）。存活线程的
  分配策略（水位线/allocate/release）完全不动。
- **行为对齐依据**：POWER 14.2 欠载不坍缩；EOF = 工作完成 = 槽位空 = 零占用。真硬件
  空闲线程的寄存器结构未公开 → 记为开放项，按行为对齐、不虚构结构。

### 5.2 机制

触发 = 该 context 的 trace EOF（`sim.trace_repeat=false` 才可能）+ **全流水线排干**
（ROB + 全部前端缓冲 + LQ/SQ 全空）。「排干」比「ROB 空」更严格：EOF 后 input_queue/
IFETCH/DECODE/DIB_HIT/DISPATCH 仍向 ROB 送数，且 dispatch 可能被持有聚合预算的兄弟
线程卡住；仅在 ROB 空时就回收会把尚未改名指令的寄存器提前放掉。排干后
frontend_RAT == backend_RAT（= 提交态架构映射），每个非 -1 的 backend_RAT 条目都是
该线程独占的一个物理寄存器。

动作 = 按 rename map 释放全部物理寄存器回空闲池、清空/invalidate 两套 RAT、context
永不再分配。断言（debug/release 常开）= **空闲池守恒**：`count_free + count_busy ==
REGISTER_FILE_SIZE`（busy ⟺ allocated，改名只发生在 ROB 内、无 squash 释放路径，守恒
严格成立）。回收完成线程 busy=0、对右端零贡献。默认 `trace_repeat=true` 下 EOF 永不
触发 → 回收路径是死代码（N=1/N=4 满负荷逐位零行为）。

### 5.3 改动点（file:line）

| 位置 | 内容 |
|---|---|
| `inc/core_context.h:128` | 声明 `void reclaim_registers();` |
| `inc/core_context.h:162-163` | 新增 `bool trace_eof = false; bool finished = false;`（+ 注释） |
| `src/core_context.cc:83-98` | `reclaim_registers()`：遍历 backend_RAT 逐个 `free_register`（带 producer 归属断言），再 `frontend_RAT.fill(-1)` / `backend_RAT.fill(-1)` |
| `inc/register_allocator.h:47` | 声明 `unsigned long count_busy_registers() const;`（守恒不变量注释） |
| `src/register_allocator.cc:3,36-39` | `#include <algorithm>`；`count_busy_registers()` = `count_if(…, busy)` |
| `src/champsim.cc:64-70` | do_cycle 读 trace 前：`if (trace.eof()) ctx.trace_eof = true;` |
| `src/ooo_cpu.cc:69-91` | operate() 内回收触发循环（排干条件 + `reclaim_registers()` + `finished=true`） |
| `src/ooo_cpu.cc:104-110` | operate() 末空闲池守恒断言 |

### 5.4 锁复跑 + N=4 满负荷回归（PASS，逐位）

口径同 V2.3 §3（归一化 sha）。回收路径在 `trace_repeat=true` 下是死代码，守恒断言
只读 → 全部逐位复现 V1/V2.3。

| 锁 | sha256（norm） | 判定 |
|---|---|---|
| 锁 1 baseline | `8dcc3227…94f6` | = V1 逐位 |
| 锁 2 prf64 | `9bb10576…ea56` | = V1 逐位 |
| 锁 3 sched64 | `8dcc3227…94f6` | = V1 逐位 |
| 锁 4 run1 == run2 | `ca19795f…c9491` ×2 | = V2.3 逐位，×2 确定性 PASS |
| n4-fotonikx4 | `aa5acbdb…b11b12` | = V2.3 active 逐位 |
| n4-hetero | `9c230993…e8c3d2` | = V2.3 active 逐位 |

### 5.5 场景 B / A 重跑（判①② 重判）

口径同 V2.4 §4（`sim.trace_repeat=false`，截短 trace）。TEMP 回收 dump 已回退、以
干净二进制重建后复现。

| config | cycles V2.4 | cycles V2.5 | Δ |
|---|---|---|---|
| b-equal | 2 536 217 | 2 519 365 | **−0.66%** |
| b-active | 2 536 393 | 2 474 252 | **−2.45%** |
| b-active-prf512 | 2 399 218 | 2 399 218 | **0（逐位）** |
| a-equal | 3 667 043 | 3 581 606 | **−2.33%** |
| a-active | 3 669 542 | 3 597 691 | **−1.96%** |

**幸存线程尾部 IPC**（aggregate 8M→10M，只剩 ctx0 的窗口 IPC = 窗口指令/窗口周期）：

| config | 尾部 IPC | 对照 |
|---|---|---|
| b-active（prf256 + 回收） | **2.368** | V2.4 无回收 ~2.2 |
| b-active-prf512（饱和上界） | **2.428** | V2.4 ~2.44，逐位复现 |
| b-equal（prf256 + 回收） | 2.247 | ROB 定额 128 仍封顶 |
| standalone prf256 | 2.65 | 残余上界 |

**判①（场景 B 幸存尾部）改判 PASS**：prf256 尾部 2.368 vs prf512 上界 2.428，差距
**2.47% < 3%**。V2.4 无回收时该差距 ~10%（2.2 vs 2.44），回收把它关到判据内。
prf512 逐位复现 V2.4（`d516f087…`）是**归因自检**：回收对不缺寄存器的 prf512 零
作用，唯一变化源就是寄存器回收本身。

**判②（场景 A）重判 PASS（无回退，轻微增益）**：尾部（两幸存线程）aggregate
heartbeat IPC ~2.26–2.28（V2.4 ~2.13–2.22），仍 ≥ N=2 ref 2.127；回收让两幸存线程
从 166 共享寄存器回到满 256，整体 −2%。

### 5.6 寄存器 dump（回收前后）

回收触发时点逐线程 dump 空闲池（TEMP 埋点，已回退）：

```
b-active: [RECLAIM] ctx1 free=32→77  ctx3 free=80→125  ctx2 free=166→211  (各 freed=45, total 256)
```

- **每线程回收 45 个物理寄存器**（= 提交态架构映射；CLAUDE.md 的「45 register IDs →
  47 physical」是 2 目的地改名下的在飞峰值，排干后只剩 45 提交态）。
- **场景 B**：3 × 45 = 135 归还；空闲池轨迹 **32 → 211**。幸存线程可用寄存器从
  ~115（V2.4 估算 256−3×47）→ **~211 空闲 / ~256 全量**（仅自持 45 提交态）。
- 实测空闲池最低 32（比估算 115 更惨：幸存线程还压着 ~44 在飞未提交寄存器）。

### 5.7 守恒断言触发情况

全部 12 次运行（3 锁 + 锁4×2 + N=4×2 + 场景 5）stderr **0 次**「ChampSim assertion
failed」；回收内 producer 归属断言、空闲池守恒断言均 0 触发。守恒不变量
`free + busy == REGISTER_FILE_SIZE` 全程成立（无双重释放、无泄漏）。

### 5.8 惊喜/不确定项

1. **回收不区分 rob_partition**：它是寄存器 lifetime 修复，与 ROB 切分正交，故
   **b-equal 也变化**（−0.66%）。这是预期内但 V2.4 表未列出的新现象。
2. **回收「揭开」了 ROB 弹性的收益**：V2.4 §4.5 结论「弹性 ROB 对欠载幸存吞吐贡献
   ≈ 0」在回收后**不再成立**——post-fix 下 b-active vs b-equal = **−1.79%**（2 519 365
   → 2 474 252），寄存器粘滞被清掉后 ROB 分母收益重新可见。这是本修复最有价值的副
   产物：它证明 V2 的 ROB 弹性（V2.2）方向正确，只是被 PRF 粘滞盖住了。
3. **残余 2.47%（prf256 vs prf512）**：prf256 下幸存线程有 256−45=211 空闲，略低于
   独立 standalone 的 211+（同名、无兄弟污染）；prf512 给 467 空闲、彻底饱和。211 已
   在饱和边缘（prf 敏感曲线 192→2.58 / 256→2.65），故残余在判据内但非零。
4. **残余 ~8%（prf512 2.43 vs standalone 2.65）仍在**：归因于 4 路同 trace 共驻对共享
   hashed_perceptron 的污染，属 V2.5 范围外（同 V2.4 §4.5 点 3），未动。
5. **回收触发点实测在 cycle ~2.8M**（场景 B），即兄弟线程 EOF 后 drain 需 ~1.8M
   cycle（暖机止 ~1.2M + 量测 ~1.6M）才排干——「排干」条件比「ROB 空」晚很多，印证
   5.2 的严格触发必要性。


