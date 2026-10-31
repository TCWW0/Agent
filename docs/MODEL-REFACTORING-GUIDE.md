# Model 基础重构指导（历史记录）

> 状态：第 1–19 节保留 2026-09 的设计与实施历史，已由 2026-10-09 的五切片重构替代。
> 当前合同见 [Runtime 架构](RUNTIME-ARCHITECTURE.md)，实施范围见 [重构方案](RUNTIME-ARCHITECTURE-REFACTOR-PLAN.md)。
> 历史章节中的 d/s、StreamState、operation map/retire/supersedes、零 ID 兼容及散装配置均不再是实施要求。

本文档记录 `Model` 基础重构的共同约束、实施顺序和验收标准。它的目的不是一次性设计理想 Core，而是先恢复状态的可读地形，再利用编译错误和测试失败逐步暴露、梳理现有关系。

本文件保留历史证据；当前实现约束以 Runtime 架构说明为准。

## 1. 背景

当前 `Model` 顶层字段数量看似不多，但只有以下三项能直接表达系统的主干：

```cpp
Phase phase{Idle{}};
Thread thread{};
Profile profile{Profile::Write};
```

其余字段混合了配置、请求快照、上下文投影、统计、压缩控制、防重标记、ID 分配和表示层信号。它们的具体定义与维护协议散落在多个头文件、reducer、Host、UI 和测试中。因此，当前的主要问题不是字段数量，而是打开 `model.hpp` 后无法获得稳定的整体方向；每个字段背后都连接着一套隐式协议。

本轮参考 `agentty` 的 Model 编排方式：在同一处展示领域状态、当前会话状态和临时状态，使 `model.hpp` 成为系统地图。参考的是这种可读形状和迁移方法，不要求复制 `agentty` 的所有字段归属，也不把它视为终局答案。

## 2. 核心原则

### 2.1 先恢复状态地形，再做语义消债

第一轮只重新编排已有字段，不删除状态、不合并概念、不改变状态机行为。字段进入 `Domain`、`StreamState` 或保留在 `Model` 顶层后，真实访问范围会通过编译错误显现；后续语义重构应以这些证据为依据。

### 2.2 编译器找显式引用，测试找隐式协议

- 编译错误负责列出所有直接依赖旧字段布局的代码。
- 测试失败负责暴露字段之间没有被类型表达的同步协议。
- 不使用兼容 accessor、引用别名或宏掩盖旧访问。
- 不以全局盲目替换代替对字段归属的判断。

### 2.3 第一轮严格保持生产行为

第一轮不得顺带改变：

- `Msg` 或 `Cmd` 的语义；
- queue、permission、compaction、usage 的行为；
- Host 的调度和并发方式；
- provider 请求内容；
- UI 展示与交互；
- 序列化或持久化格式。

测试代码允许同步重构，但业务断言与覆盖的状态转换不得减少。

### 2.4 不用新的过度封装替换旧的过度封装

第一轮保持 aggregate 与 public fields，不增加 constructor、getter/setter 或通用 facade。只有当多个调用点反复维护同一不变量时，后续才为该不变量增加一个有语义的操作。

一个抽象只有在至少满足下列一项时才值得保留或新增：

1. 它使非法状态不可表示；
2. 它消除了多个调用者重复维护的协议。

## 3. 第一轮目标形状

> 历史旧布局：本节记录首轮迁移启动时采用的临时形状，不是当前类型定义；其中 `ContextBudgetSnapshot` 等旧名已由第 18 节记录的 request-preparation/admission 布局替代。

第一轮采用以下临时布局：

```cpp
struct Domain {
    Thread thread{};
    Profile profile{Profile::Write};

    std::optional<ContextManagementConfig> context_management_config{};
    std::optional<AcceptedCompaction> accepted_compaction{};
    std::string provider_name;
    std::string provider_model;
};

struct StreamState {
    std::optional<PendingPermission> pending_permission{};

    StreamTelemetry stream_telemetry{};
    UsageState usage{};
    std::optional<ContextBudgetSnapshot> context_budget{};
    ContextProjectionLedger projection_ledger{};

    std::optional<RequestFingerprint> last_failed_compaction_source{};
    std::optional<ContextBlockDetail> last_failed_compaction_detail{};

    std::deque<QueuedMessage> queued_messages;
    IdSequence ids{};
};

struct Model {
    Phase phase{Idle{}};
    Domain d;
    StreamState s;
    std::uint64_t revision{0};
};
```

这是迁移布局，不是最终所有权声明。特别是：

- `accepted_compaction` 后续很可能进入 `Thread`；第一轮不做该语义迁移。
- `pending_permission` 后续可能被 `AwaitingPermission` 吸收；第一轮保留双份表达。
- `phase` 是整个 Model 的状态机判别器，而不是流调用的局部状态，因此保留在顶层。
- `revision` 暂时作为未归类字段保留，以便明确暴露其后续去向。
- usage/context 的内部类型第一轮不压平。

第一轮不为了模仿 `agentty` 而额外改名。例如只有一个活动 Thread 时继续使用 `d.thread`，不改成 `d.current`。

## 4. 实施方法

### 4.1 批次 0：建立基线

实施前：

1. 记录当前分支与工作树状态，保留用户已有修改。
2. 完整构建并运行当前测试，确认重构前基线。
3. 若基线已有失败，单独记录；不得把既有失败归因于本轮重构。

### 4.2 批次 1：引入 `Domain`

1. 在 `model.hpp` 中增加 `Domain`。
2. 只移动本节目标形状中属于 `Domain` 的已有字段。
3. 根据编译错误逐个修复生产代码和测试访问。
4. 不提供 `Model::thread` 等旧路径的兼容层。
5. 完整构建并运行测试。

本批次应保持 `Model` 仍可作为 aggregate 使用，避免把布局迁移扩大成对象生命周期重构。

### 4.3 批次 2：引入 `StreamState`

1. 在 `model.hpp` 中增加 `StreamState`。
2. 移动其余 session/runtime 字段。
3. 根据编译错误逐个修复访问。
4. 保留 `phase` 与 `revision` 的顶层位置。
5. 完整构建并运行测试。

### 4.4 处理编译错误的规则

每个错误都应回答：

- 该调用点读取的是持久/领域事实，还是当前 session 状态？
- 它是否只需要 `Domain` 或 `StreamState`，却接收了整个 `Model`？
- 它是否正在同步修改多个字段？若是，记录为后续不变量候选。
- 它是否揭示了 Host、UI 或测试对 Core 内部布局的非必要依赖？

第一轮只修复路径并记录结论，不顺带完成所有候选重构。

## 5. 生产代码的边界规则

### 5.1 暂不拆分文件

`agent.cpp` 可以暂时保持现状。文件大小不是本轮问题；在所有权清楚之前继续拆文件只会增加导航和接口成本。

### 5.2 暂不收窄所有函数参数

第一轮可以继续传递 `Model&`。迁移完成后再根据实际访问情况分为：

- 只使用 `m.d`：候选参数 `Domain&`；
- 只使用 `m.s`：候选参数 `StreamState&`；
- 读取顶层 `phase` 或同时操作两者：真正的顶层协调函数。

参数收窄应作为后续独立批次，避免与字段搬迁混杂。

### 5.3 不增加兼容路径

禁止为了减少当前修改量而增加：

```cpp
Phase& Model::phase();
Thread& Model::thread();
```

也禁止引用成员、宏别名或同时维护新旧字段。编译错误正是本轮所需要的依赖清单。

## 6. 测试同步重构策略

测试属于当前关系网络的一部分。生产布局变化后，如果测试只机械追随字段路径，旧耦合仍会被完整保留。因此，测试在保持功能覆盖的前提下可以同步整理。`phase` 作为全局状态机判别器继续使用 `model.phase`，不应为了分组而藏入局部状态容器。

### 6.1 测试错误分类

遇到测试编译或运行错误时，先判断：

1. **行为测试**：关心输入事件、后继状态或 `Cmd`。继续通过 `update`/`dispatch` 验证行为。
2. **中间状态测试**：必须从 streaming、permission、tool 等状态开始。允许使用少量有语义的场景构造器。
3. **布局测试**：只验证旧字段路径或内部表示，没有独立业务价值。可以删除或并入行为测试。

不得通过放宽断言、更新错误预期或删除独立错误路径来换取测试通过。

### 6.2 场景构造器

当同一种合法复杂状态在多个测试中重复手工组装时，可以提供类似：

```cpp
auto model = scenario::streaming(...);
auto model = scenario::awaiting_permission(...);
```

构造器应满足：

- 名称表达业务场景，而不是字段集合；
- 默认生成合法且最小的状态；
- 只暴露测试真正需要变化的参数；
- 优先作为单个测试文件的局部 helper；出现多个真实消费者后再共享。

禁止建立带几十个可选 setter 的万能 `TestModelBuilder`，否则只会复制出一个测试版 Model。

### 6.3 可合并的测试

- setup 与断言完全相同、只改变一个输入的用例，可改成参数化或表驱动测试。
- 被拆成多个碎片的同一生命周期，可以增加完整 scenario 测试。
- permission identity、迟到事件、profile 差异等规则适合表驱动。

合并后仍应能从失败信息中识别具体输入与失效规则。测试数量可以下降，行为覆盖不能下降。

### 6.4 测试与生产修改同行

每个生产结构批次应同时提交受影响的测试修改，保持提交结束时完整构建和测试通过。不得先用兼容层让生产代码通过，再把测试债务留给未知的后续批次。

## 7. 第一轮之后的已知候选工作

以下问题已经识别，但不属于第一轮：

1. 将 `accepted_compaction` 移入 `Thread`，使 wire projection 与对话历史同寿命。已在第 12 节记录。
2. 将 request-scoped budget、projection、fingerprint 移入相应 phase。
3. 合并 `last_failed_compaction_source/detail` 等平行 optional。已在第 13 节记录。
4. 消除 `AwaitingPermission`、`pending_permission` 与 transcript tool call 的重复事实。
5. 消除 `AsyncHost` 与 `Model` 对 context config/provider identity 的镜像。
6. 让 `Cmd` 携带执行所需的完整快照，减少 Host 回读 Core 状态。
7. 将 `revision` 移出领域 Model。
8. 最后处理 usage/context 内部的重叠观测、累计值和派生值。
9. 根据真实访问范围收窄函数参数，再决定文件边界。
10. 消除 `CompactionPlan::compaction_id` 与重复保存的 `OperationIdentity`。已在第 14 节记录。

这些候选项应逐项实施，每项单独说明源事实、派生值和生命周期，不组成一次大爆炸式重写。

## 8. 提交与验证节奏

建议提交序列：

1. `model: group persistent state under Domain`
2. `model: group live state under StreamState`
3. `tests: consolidate model scenario setup`（仅在确有重复时）
4. `docs: record follow-up ownership findings`（若实施中发现新的稳定结论）

每个提交必须：

- 可独立编译；
- 通过受影响测试；
- 在合并前运行完整测试，且相对记录的既有基线不新增失败；
- 不包含无关格式化和无关工作树修改；
- 能清楚说明是布局变化、测试去耦，还是后续语义变化。

## 9. 第一轮完成标准

生产代码：

- `Model` 顶层只剩全局状态机 `phase`、`d`、`s` 和暂留的 `revision`。
- 不再存在旧的 `model.phase`、`model.thread`、`model.usage` 等访问。
- 没有兼容 accessor、引用别名或重复字段。
- `Msg`、`Cmd`、状态转换和 Host 行为保持不变。
- 完整构建通过；完整测试相对重构前基线无回归。

测试代码：

- 所有受影响的旧字段访问都已迁移。
- 未放宽断言、改变输入和预期，或删除行为覆盖。
- 若迁移错误暴露出重复的复杂 Model 拼装，再用表达合法业务场景的局部 helper 整理；不为追求形式统一而强行合并测试。
- 若合并或删除测试，原行为覆盖必须有明确承接者。
- 断言仍描述业务结果、身份匹配和状态转换，而不是新字段路径本身。

过程产物：

- 记录仅依赖 `Domain`、仅依赖 `StreamState` 和跨二者协调的函数。
- 记录总是成组写入的字段，作为下一轮不变量重构依据。
- 记录 Host、UI、测试对 Model 内部表示的越界依赖。

## 10. 明确延后的决策

以下问题不阻塞第一轮，也不得在缺少使用证据时提前定案：

- `revision` 最终属于 UI、runtime 还是被删除；
- `pending_permission` 是否完全进入 phase；
- context config 最终由 Domain、Host 还是一个完整设置记录拥有；
- usage 是否需要独立深模块；
- `Domain`/`StreamState` 是否最终变为私有封装；
- 是否继续沿用 `update(Model, Msg) -> Step` 的按值接口。

第一轮的成功不是回答这些终局问题，而是让它们在更清楚的状态地图上被逐项回答。

## 11. 第一轮实施记录

2026-09-27 完成第一轮布局迁移：

- `Model` 顶层只保留 `Phase phase`、`Domain d`、`StreamState s` 和 `revision`。
- 生产代码与测试中的旧字段路径已全部迁移，没有增加兼容层。
- 除 `model.hpp` 的结构定义外，将新增的 `.d` / `.s` 路径机械还原后，所有受影响文件都与迁移前逐字节一致；本轮未混入语义变化。
- 测试审计未发现断言、输入、预期、用例或 helper 被削弱或删除，因此本轮没有为了合并而合并测试。
- `cmake --build build --parallel 4` 通过。
- `ctest --test-dir build --output-on-failure` 共发现 569 项：3 项失败、9 项按配置跳过，失败集合与重构前基线一致。既有失败分别是 HTTP timeout 测试无法创建监听 socket，以及两项 calculator 测试预期 6 个工具但实际得到 7 个工具。

## 12. Accepted compaction 与 Thread 同寿命

2026-09-27 完成一个行为保持的所有权切片：

- `Thread` 现在同时拥有原始 `messages` 与可选的已接受 `AcceptedCompaction`。摘要是成功 compaction 的提交结果，不能从原文确定性重算，因此保留为记录；compaction 验证前的候选仍由 validation result 表达。
- 正常请求投影通过 `project_request_with_compaction(Request, Thread)` 从该 `Thread` 读取已提交记录；completed-history、compaction planning 与 `canonical_request` 也只接收 `Thread`。这消除了把一个 `Thread` 与另行传入、可能属于别的 `Thread` 的 accepted record 独立组合的接口。
- 候选验证仍通过 `compaction.cpp` 内部 helper 显式投影，因为 candidate 尚未成为 Thread 的事实；helper 不对外公开。验证成功后才写入 `thread.accepted_compaction`，后续请求投影随 Thread 读取它。
- 投影仍有一个未由类型约束的 join：`Request` snapshot 必须来自同一 `Thread`，并可带有尚未提交的尾部消息；调用者仍可能错误地把 Thread A 的 Request 与 Thread B 传给投影入口。若 accepted boundary 不在 Request 中，当前实现回退并返回原 Request；这不能排除 Request/Thread 误配。将 snapshot 与 Thread 绑定应延后到 request preparation/Cmd 完整快照切片，本轮不扩大此边界。
- 进行中的 `Compacting` plan、pending summary 和 failure bookkeeping 继续留在 Phase/StreamState；本切片不改其身份、生命周期或失败处理。
- `Thread` 随 `Model` 按值复制时，transcript 与 accepted record 一并复制。代码库没有 Thread 序列化、持久化或独立 restore 路径，也没有同一 Model 中切换 Thread identity 的路径。请求准备仍可更新 provider identity；与原先放在 Model 上时一样，本切片不因 provider 变化清除或重建 accepted record，也不新增 provider 切换规则。
- Compaction 与 update 行为测试保留原断言和输入；compaction 测试 setup 现在把已接受记录放进 Thread。未增加只验证字段布局的测试。
- 未改变 Msg/Cmd、provider wire 格式、Host 调度或 UI 行为。剩余候选包括将 request scoped budget/projection/fingerprint 放入 phase、合并失败 bookkeeping、处理 permission 镜像、Host/Model 配置镜像、Cmd 快照及 revision/usage 归属。
- 验证：`cmake --build build --parallel 4` 通过；compaction 与 agent_loop 聚焦测试 33/33 通过；全量 CTest 569 项中 557 项通过、9 项跳过、3 项失败，失败集合与既有基线相同：HTTP timeout 测试 `PostHonorsReadTimeout`（`listen_fd == -1`），以及两项 calculator 测试预期 6 个工具、实际为 7 个；`git diff --check` 通过。

## 13. 合并 compaction retry failure bookkeeping

2026-09-27 完成一个行为保持的 session-state 切片：

- `StreamState::last_failed_compaction` 是失败 compaction retry policy 的唯一记录，类型为 `FailedCompactionAttempt`。其中 `source_digest` 可缺失（例如手动 planning failure 无法产出 source digest），`detail` 始终存在；类型不再允许 source 与 detail 作为互不相关的两个 optional 分别更新。
- 该记录只属于 `StreamState`，描述当前 runtime 的同源自动重试抑制与 context failure 说明；它不属于 `Thread`、`Domain` 或 `Compacting` phase。`Compacting` 仍只保存活动 plan、trigger、pending summary 和 usage 接受状态。
- 各失败入口直接写入完整记录；成功 compaction 清除整条记录。自动 retry suppression 从同一记录读取 source digest 与 detail。薄的 `remember_compaction_failure` 已删除，`finish_compaction_failure` 保留，因为它仍负责退出活动 compaction、drive 队列和必要时返回 cancel 命令。
- 活动 compaction 被 `CancelActive` 取消时仍按一次 compaction failure 记录其 source digest 和 `CompactionFailed` detail；随后 queued head 的 preparation 会因同 source 被抑制而继续原请求路径。`ClearQueue` 不创建失败记录，也不改动 Thread 上已接受的 compaction；planning failure 缺少 source digest 时的记录规则、stale event 与 operation identity 检查均保持不变。
- 仍延后的债务包括 request snapshot 与 Thread 的类型绑定、CompactionPlan/Validation 接口整理、queue drive 调用链、permission 镜像与 context/usage 状态归属。没有增加 `CompactionState` 嵌套或重命名 `StreamState`。
- 验证：`cmake --build build --parallel 4` 通过；`ctest --test-dir build --output-on-failure -R '^(agent_loop|compaction)\.'` 的 33 项全通过；全量 CTest 569 项中 557 项通过、9 项跳过、3 项失败，失败仍是既有基线中的 HTTP `PostHonorsReadTimeout`（`listen_fd == -1`）和两项 calculator 工具数（预期 6、实际 7）；`git diff --check` 通过，旧字段名及 `remember_compaction_failure` 在代码中无引用。

## 14. CompactionPlan identity 唯一来源

2026-09-27 完成一个行为保持的 identity 去重切片：

- `CompactionPlan` 只保存 `compaction_id`。对外 operation identity 由该 ID 派生，不再在 plan 中保存可独立失配的第二份 `OperationIdentity`；`matching_operation` 与接收 operation identity 的 `validate_compaction` overload 已删除。
- 外部事件身份仍在 reducer 边界校验：`CompactionFinished.compaction_id` 必须匹配活动 plan；compaction `RequestUsage.operation` 必须等于从活动 plan 的 `compaction_id` 派生的 `compaction_identity`。usage 不匹配时活动 compaction 不变，也不入账。
- `validate_compaction` 仍验证 plan 的 compaction ID 有效，以及 compaction request 与 source/config 一致；request 自身身份不匹配仍返回 `CompactionFailureKind::Identity`。它不再接收或校验来自事件的 operation identity，因为事件身份只属于 reducer seam。
- 删除 `APlanCreatesExactlyOneCompactionOperation`（只断言重复字段互相一致）以及篡改 `plan.operation` 的实现耦合断言。身份覆盖迁移到 `update(Model, Msg)`：既有 stale `CompactionFinished` 用例覆盖错误 compaction ID；新增 RequestUsage 行为用例保持 fingerprint 正确，分别只改变 operation value、kind 或 owner，验证活动 phase 与 usage 均不变。request identity mismatch、schema、digest、no-gain 等 validation 覆盖继续保留。
- 本切片不改 Msg/Cmd、Host、provider wire 或 UI 行为；不涉及 validation result 表示、queue/canonical helper 或后续其他身份债务。
- 验证：`cmake --build build --parallel 4` 通过；compaction 与 agent_loop 聚焦测试 33/33 通过；全量 CTest 569 项中 557 项通过、9 项跳过、3 项失败，失败与既有基线相同（HTTP `PostHonorsReadTimeout` 的 `listen_fd == -1`，以及两项 calculator 工具数预期 6、实际 7）；`git diff --check` 通过；旧 `plan.operation`、`matching_operation` 及 validate operation overload 均无引用。

## 15. Compaction validation 只能表达成功或失败

2026-09-27 完成一个行为保持的 validation 结果切片：

- 删除可同时包含 `accepted` 与失败分类的 `CompactionValidation`，改为 `std::expected<AcceptedCompaction, CompactionValidationFailure>`。错误值只保存 `CompactionFailureKind kind` 与 `rejection_reason`；`CompactionFailureKind::None` 不再存在，不能表示“成功值缺失但没有错误”或“成功和失败同时存在”。
- 每个拒绝分支直接返回 `std::unexpected(CompactionValidationFailure{...})`，薄的 `reject` helper 已删除；成功时直接返回候选 `AcceptedCompaction`。`agent.cpp` 通过 `if (!validation)` 进入原失败转移，仍使用原 `detail_for_validation_failure` 映射，成功时提交 `*validation`。
- 所有错误 kind 与 rejection reason、判定次序、request 投影与 NoGain 规则保持不变。`config.validate()` 仍按原样抛错；planning 的 `CompactionPlanFailure` 是另一套 `std::expected`，没有合并或改变。
- 公开 validation 协议测试现在通过 `has_value()`/解引用检查成功，通过 `error().kind` 检查原有失败分类，并保留失败 reason 的诊断输出；schema、fence、size、coverage/digest、request identity、NoGain 与 projection 用例均保留，没有新增布局测试。
- 验证：重新生成本地 CMake build files 后，`cmake --build build --parallel 4` 通过；compaction 与 agent_loop 聚焦测试 33/33 通过；全量 CTest 569 项中 557 项通过、9 项跳过、3 项失败，失败与既有基线相同（HTTP `PostHonorsReadTimeout` 的 `listen_fd == -1`，以及两项 calculator 工具数预期 6、实际 7）；`git diff --check` 通过。初次 build 因旧生成的 gtest discovery 脚本并行共用 JSON 文件而失败，重新配置后成功；该问题与 validation 代码无关。

## 16. drive 是 queued dispatch 的唯一入口

2026-09-27 完成一个行为保持的 queued dispatch 收敛切片：

- `drive(Model&, optional<OperationId>)` 是队列调度的唯一入口，并只保留一个权威 guard：phase 必须为 `Idle` 且 queue 非空。满足后，它直接从队头构造 draft/canonical request、应用 output budget、追加 queued user message、分配 preparation ID、切换到 `PreparingRequest` 并返回 `PrepareRequest`；`start_queued` 与 `prepare_queued` 已删除。
- 队头在 `RequestPrepared` 接受前仍留在 queue；只有事件匹配当前 queued preparation 且预算获准后才出队并进入 transcript。`supersedes` 同时复制进 `PreparingRequest` 与 `PrepareRequest`，并在后续 `StartStream` 中保持不变。
- `ResumeQueue` 对 Blocked phase 先恢复 Idle 再调 `drive`：有队列时准备当前队头，无队列时保持 Idle 并返回 `NoCommand`。取消 compaction 后仍通过 `finish_compaction_failure` 写入 retry failure 并驱动队列；Submit、finish turn、成功 CompactionFinished 和其他队列推进调用点维持原行为。
- `ClearQueue` 的既有策略边界本轮明确记录但未改变：它仅在 queue 非空时生效；会取消 `RetryCurrent` preparation；若手动 compaction 开始后才有新项入队，ClearQueue 仍会因当前 Compacting 分支不检查 trigger 而取消该 compaction。是否按 trigger 区分清理策略属于后续产品/状态机决策，本切片不扩大。
- reducer 行为测试锁定取消 compaction 的 operation ID 穿透 `PrepareRequest.supersedes` 到 `StartStream.supersedes`，并覆盖 ContextLimit Blocked 有队列时准备队头、无队列时只解除阻塞。既有 queue、context failure、compaction 断言保持；未新增布局测试。
- 验证：`cmake --build build --parallel 4` 通过；agent_loop 与 ts006 聚焦测试 33/33 通过；全量 CTest 571 项中 559 项通过、9 项跳过、3 项失败，失败集合与既有基线相同（HTTP `PostHonorsReadTimeout` 的 `listen_fd == -1`，以及两项 calculator 工具数预期 6、实际 7）；`git diff --check` 通过，`start_queued`/`prepare_queued` 无引用。

## 17. 删除无信息量的单调用转发

2026-09-27 完成一个行为保持的 helper 收敛切片：

- 保留公开 `canonical_request(const Thread&)`，并将它作为唯一请求 draft 实现：直接从 `Thread::messages` 构造 `Request`，再按同一 `Thread` 的 accepted compaction 投影。匿名 `make_request_draft` 已删除，生产调用点直接使用公开入口。`async_host` 中已有的公开入口行为测试继续覆盖这一合同。
- 删除 `summary_marker()`，把原样的 marker、prompt version、换行拼接内联到 `summary_message()`；删除 `compaction_prompt()`，把原样 prompt 构造内联到 `compaction_request_for()`。字符串、预算、delimiter、role、tools 与 source 消息策略均未改变。
- 删除标准是：单调用转发没有独立策略，调用者必须多学一个名字，却没有获得可复用或可理解的边界。保留的 `summary_message()` 明确表达摘要是仅用于 wire 的合成 User 消息而非 transcript；`compaction_request_for()` 则统一清理 tools、设置预算与 prompt、添加 source delimiters，并由 planning 和 validation 复用。这些入口承载可复用的策略，因此不内联。
- 没有新增脆弱的字符串快照测试；本切片复用 canonical request、compaction planning/validation 和 reducer 行为 seam。
- 验证：`cmake --build build --parallel 4` 通过；`ctest --test-dir build --output-on-failure -R '^(compaction|agent_loop|async_host)\.'` 共 47 项、46 通过、1 项为既有 async_host calculator 工具数失败；全量 CTest 571 项中 559 项通过、9 项跳过、3 项失败，失败集合与基线相同：HTTP `PostHonorsReadTimeout`（`listen_fd == -1`）及两项 calculator 工具数（预期 6、实际 7）。`git diff --check` 通过；`make_request_draft`、`summary_marker`、`compaction_prompt` 均无引用。

## 18. Request preparation 的所有权与生命周期

2026-09-27 完成 request-preparation 语义切片。以下是当前布局与行为记录；第 3、7、10 节中被后续切片替代的候选项保留作历史背景，不再代表当前状态。

### 唯一事实来源

- `Domain::context_management_config` 是必有值，是请求准入、normal/compaction output cap 与 compaction 校验的唯一 context policy。`Domain::provider_name` 和 `provider_model` 是当前请求身份的唯一来源。`AsyncHost` 不再保存这些字段的镜像，也不再有独立 context-config setter；Host 只执行 reducer 交付的命令快照。
- `SetRequestPolicy` 原子更新 policy 与 provider identity。若当前处于 `PreparingRequest`，reducer 从最新 Model 重建该 job，发出新 `PreparationId`；旧代的成功和失败事件都因 ID 不匹配而被忽略。流已经进入 `Streaming` 后，更新 policy 不取消、不重算或替换该流。
- 动态 system prompt 的 provider 仍由 Host 持有，因为它可能读取会话文件。设置 provider 不改变 `Model::revision`。准备 seam 接收 optional prompt：无 provider 时保留 draft 原 prompt；provider 返回空串时把 prompt 明确覆盖为空串。

### 请求快照与状态生命周期

- `PrepareRequest` 是自包含的执行命令，携带 draft request、provider name/model、policy、projection ledger、`PreparationId` 与可选 superseded operation。它不携带 `TurnId`；Host 不回读 Model 来补字段。`PreparingRequest` 只保留当前代次、purpose、队列/assistant 锚点与 supersedes，不保留 request。
- `prepare_request_snapshot` 是纯准备 seam：注入 runtime tools 和可选 system prompt，按 policy 写入 normal output cap，生成 context projection 与 admission assessment。结果是 `PreparedRequestSnapshot { projection, budget }`。准入 assessment 不重复存 fingerprint；fingerprint 属于 projection。
- `RequestPrepared` 和 `RequestPreparationFailed` 只以 `PreparationId` 关联活动 job。准备成功进入 streaming 后，只保存 `ContextProjectionReceipt` 供 exact usage 校准；`Streaming` 和 `StartStream` 不保存完整 projection snapshot。预算 assessment 只在准入结果及可选 `Blocked::budget` 诊断中有用。
- Blocked queue preview 可以展示保存下来的 budget estimate。主 context count/bar 的事实来源则始终是 `ObservedContextUsage`；`project_model_usage` 会丢弃调用者传来的 context count，再从该 provider observation 投影 UI gauge。估算准入值不会冒充已观测用量。
- 准备失败按 purpose 收敛：`QueuedTurn` 保留队首并进入 `ExplicitResumeRequired` Blocked；`ToolContinuation` 与 `RetryCurrent` 把错误写入对应 assistant message，结束当前回合并继续 drive 队列。

### Compaction plan 合同

- `CompactionPlan` 捕获启动时的 `ContextManagementConfig` 与 `provider_model`。`validate_compaction(plan, summary)` 仅依赖这个 plan 和摘要，不回读后来变化的 Model。
- 在 compaction 运行时收到 `SetRequestPolicy` 只更新 Domain；活动 plan 的身份、准入参数和验证合同不变。完成后先按 plan 合同验证并提交 accepted compaction，再从当前 Domain 重新 drive/prepare 队列请求。直接构造 compaction plan 的 overload 也从 policy 取 normal output cap，避免 `Request` 的通用默认 cap 绕过 policy。

### Headless 边界与删除

- `HeadlessRunner` 已从生产头源、实现、CMake、README 与专属测试中硬删除。`tool_loop_test` 中三个只通过该 runner 驱动的用例也随之删除；没有保留兼容 API 或 runner 替代品。它们不是被迁移到另一套 runner 测试。测试总数因此应相对旧记录减少，验证比较按当前注册集合报告。

### 仍延后的债务

- `Request` 仍是独立快照，类型未保证它和传入 projection/Thread 属于同一会话；accepted-compaction boundary 之外仍可能发生 request/thread 误配。见第 12 节记录。
- `revision` 的最终归属未定；permission phase 与 transcript tool call 的重复镜像已由第 19 节解决；usage 内部的累计、最近一次观测与 context observation 尚未进一步压平。
- `ContextProjection` 仍在 preparation 结果中被完整传回 reducer，随后只留下 receipt；是否把 admission acceptance 再压成更窄的不可变结果，应基于后续真实消费者决定。
- policy 在 Preparing 时通过重建而不是原地修订 job；如将来准备工作变成真正异步，仍需保留 PreparationId 的代次校验，并审视 superseded preparation 的资源取消策略。当前 Host 只执行命令快照，没有该类配置镜像。

### 本切片验证

- 行为覆盖包括：PreparationId 新代次及旧 success/failure stale、Streaming 期间更新 policy 不取消、Queued/ToolContinuation/RetryCurrent 三种准备失败路径、Blocked budget UI 与 observed context gauge 的区分，以及活动 compaction 在 policy 更新后的 plan-contract 验证。
- 纯 seam 测试覆盖 optional system prompt：缺省 provider 保留 draft 值，显式空字符串覆盖 draft 值。
- 最终 `cmake --build build --parallel 4` 通过。聚焦运行 agent_loop、context_admission、compaction、async_host、status_bar、screen、view 共 115 项，114 通过；唯一失败仍是基线中的 async_host calculator 工具数预期 6、实际 7。
- 全量 `ctest --test-dir build --output-on-failure` 注册 564 项：553 通过、9 跳过、2 失败。两项失败是既有 HTTP `PostHonorsReadTimeout`（`listen_fd == -1`）与 async_host calculator 工具数（预期 6、实际 7）；相对旧基线的第三项 tool_loop calculator 用例随硬删除的 HeadlessRunner 测试路径一并移除。没有新增失败。
- `git diff --check` 通过。旧符号扫描在生产代码和测试中为零命中；文档命中只在本指南清楚标注的历史旧布局/硬删除记录中。`src/tool/list_dir.cpp` diff 仍仅删除用户旧改动中的 `<cctype>` 和 `<limits>`。

## 19. Tool-call permission 与 execution identity 收敛

2026-09-28 完成 Core permission/tool execution identity 切片：

- `ToolCallRef{assistant_message_id, tool_exec_id}` 是 phase、permission event、RunTool 与 report 的唯一调用身份。父 assistant `Message` 持有 `MessageId`/`TurnId`；transcript `ToolCall` 持有执行 ID、provider string ID、name/args、binding 与 settlement status。删除 `ToolCall::turn_id`、`PendingPermission` 及 `StreamState::pending_permission`。provider ID 仍只用于 provider ingress、transcript call 和 wire encoding。
- `ToolExecId{0}` invalid；ID 只由当前 Model 的 `model.s.ids.next_tool_exec` 单调分配，在线性 Model/AsyncHost session lineage 中唯一且不复用。不承诺跨独立 Model/process、恢复或复制分支唯一；这种 scope 由 MessageId anchor 一起限定。
- conversation 模块的 const/mutable `resolve_tool_call` overload 先精确定位 assistant MessageId，再定位 message 内 ToolExecId；非法或重复 execution ID fail closed。context replay 改从父 Message 读取 turn ownership。stream 接受检查后、处理 resolution variant 前分配 ToolExecId，因此 binding/error/unresolved 三种 resolution 都保留既有 transcript 结果语义。
- 串行调度以 transcript emission order 的首个 Pending call 为准。Awaiting、Executing、approve/reject 和报告只传 ToolCallRef；RunTool 另外携带自足的 tool-ref/args snapshot。profile 在每次 scheduling 边界读取：当前 awaiting/executing 不会因 SetProfile 自动改判，后续 sibling 按新 profile 决策。Done/Failed/Rejected 后只推进到下一个 Pending，不越过较早的 gated call。
- Awaiting cancel 不产生 Host cancel 或 supersedes；settle 同一 assistant message 的所有 Pending sibling 后结束/推进回合。Executing cancel 使用 `OperationId{turn_id={}, kind=Tool, value=tool_exec_id}`。invalid call ref 的 approve/reject/report fail closed；cancel 仍离开活动 phase，仅可能按可定位的 owner message 结算 sibling，不搜索其他 message。
- AsyncHost 对 ToolExecOutput 先提交 Core reducer 验证，再决定 retire operation。若 reducer 拒绝 report 且当前 ExecutingTool 仍占用同一派生 operation key，Host 保留 cancellation state；被接受的当前 report 或 inactive/late report retire 相应 execution key。旧 report 不会结算已经开始的下一 execution。
- 当前 activity UI/REPL 消费自足 `ToolActivityProjection`，不按 ref 回查 transcript、不读取 Pending.binding；invalid activity 给出诊断，唯一可发出的 domain event 是 `CancelActive`，transcript navigation/quit 仍是本地 UI 控制。历史 transcript cards/revision signature 是独立的只读历史路径，直接消费 ToolCall 的 settlement status；没有把历史投影塞进 runtime permission header，也没有新增 UI 投影模块。`permission_effect_label` 单消费者 helper 已删除，effect display assembly 归入 activity projection；`screen` 一帧只计算一次 activity projection，并传给 permission、status 与 phase 子投影。
- TDD 红证据：最初加入 exact assistant+execution ref approval tracer 时，在实现迁移前因缺少 ToolCallRef/新 `.call` shape 编译失败；迁移完成后 `ToolLoopTest.ApprovalRequiresTheExactAssistantAndExecutionReference` 通过，并覆盖错误 message/exec 的 approve 与 reject 仍保持 phase/ref 和 Pending status。AsyncHost 生命周期 tracer 在临时“先 retire、后 Core validation”顺序下失败，证明 rejected current report 之后无法取消 operation；另一个 wrong-message/same-exec-key 报告用旧的 exact-ref retire 规则再次失败（`observed_cancellation == false`）。改为 Core-first，并在 Core 更新后、retire 决策前检查当前 phase 是否仍占用同一派生 Host operation key 后，两项 tracer 均通过。
- 行为测试覆盖 stale approve/reject、跨 assistant message 重用 provider ID、permission/execution cancel、wrong ToolCallRef/ToolRef、reject 后继续 batch、旧 report 对 next execution 无效、profile 边界、valid/invalid activity projection，以及删除 call.turn_id 后的 context replay。
- 验证：`cmake --build build --parallel 4` 通过；相关 focused CTest 172/172 通过；扩大的相关集合 196 项中 195 项通过，唯一失败是既有 async_host calculator 工具数；全量 CTest 注册 573 项，562 通过、9 跳过、2 失败。两项失败与切片前记录一致：HTTP `PostHonorsReadTimeout` 和 async_host `NativeCalculatorUsesSessionRuntimeAndContinues`。旧符号与 provider-id usage 审计为零违规；最终 `git diff --check` 通过。
- 本切片不触碰 canonical-request/context call graph 等跨 ownership 后续债务；也未 commit。工作区既有的 agent/async_host/compaction/list_dir include 删除均保留。


## 20. 2026-10-09：五切片架构收缩完成

第 12–19 节记载的中间合同已同步替换：

| 历史合同 | 当前实现 |
| --- | --- |
| Domain d / StreamState s / revision | Model::domain / SessionState session；无生产消费者的 revision 已删除 |
| OperationId / OperationIdentity、kind/owner/hash | OperationRef 的 StreamRef / ToolCallRef / CompactionId 三个分支 |
| Host operation 映射、Core-first retire、supersedes | 一个取消槽位；开始新工作先作废旧句柄；process_msg 无退休分支或 Phase 回读 |
| Core 接受零 ID stream 事件 | Core 严格校验；只有原始 ProviderEvent 可以尚未绑定身份 |
| RequestPurpose 与散装 preparation 锚点 | RequestIntent；准备代次和当前队首共同关联结果 |
| 完整 projection 跨准备边界、Streaming fingerprint 副本 | PreparedRequestSnapshot 的 request / budget / receipt；流从 receipt 读取 fingerprint |
| 独立 Request + Thread 的 accepted-compaction 投影 | canonical_request(Thread) 单输入；candidate 投影保持私有 |
| 散装 ContextManagementConfig/provider/model；Host 配置复制特例 | 构造时 validate 的 RequestPolicy；Core 只接收有效值 |
| 两种重复观察结构、turn.latest_request、分散累加 | UsageObservation；usage 纯模块集中规范化和累加；turn 质量从普通请求 owner 推导 |
| CompactionTrigger、StartStream.target_message_id、仅写诊断字段 | 删除；实际手动/自动入口、Core assistant 锚点和预算诊断仍由原有逻辑表达 |

队列、工具顺序、权限精确关联、重试、压缩失败抑制与用量质量保持原语义。原先 Host 退休测试现在直接验证“拒绝报告后依然可以取消”，不再把退休机制当作合同。逻辑取消允许旧 worker 尚未退出；shutdown 通过共享停止信号与唤醒工作，不读取 owner-only 槽位。

构建、各切片测试、完整回归和环境限制统一记录在 [验证记录](RUNTIME-REFACTOR-VALIDATION.md)。
