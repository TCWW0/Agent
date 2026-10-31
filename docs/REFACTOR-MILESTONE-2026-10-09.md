# 架构收缩阶段总结：2026-10-09

本文记录上一阶段快照，其验证数字属于上一阶段；后续架构精简及本轮验证见[精简决策与实施记录](ARCHITECTURE-SIMPLIFICATION-DECISIONS-2026-10-09.md)。

本阶段完成 Runtime 架构收缩的全部五个切片，并将此前尚未提交的 UI 遗留路径清理一起保存为阶段快照。当前已有可运行、通过完整回归的本地 Agent 主线，后续适合通过真实任务发现缺口，再按小切片扩展能力。

## 1. 快照范围与代码位置

| 项目 | 当前状态 |
| --- | --- |
| 仓库 | `/root/agent_learning/my_agent` |
| 分支 | `dev/tool-system` |
| 阶段提交的父提交 | `15245b9`（`refactor(runtime): unify tool call identity`） |
| 参考源码 | `/root/agent_learning/agentty`；借鉴具名转换、生命周期数据组织和执行输入快照 |
| 当前构建目录 | `/root/agent_learning/build`；Debug / Ninja / GCC 15 / C++26 |
| 本阶段范围 | 已有 UI 清理、Runtime 五切片、消费者迁移、行为测试、架构文档及本总结 |

本文件随阶段提交一起入库；可用 `git log -1 --oneline -- docs/REFACTOR-MILESTONE-2026-10-09.md` 定位该快照。构建目录和 `/tmp` 验证日志属于本机工作产物，不随提交保存。

Runtime 实施前，工作区已经有 UI 文件和测试删除，以及 README、CMake、TODO、`list_dir.cpp` 等修改。本阶段保留并提交这些工作；下文单独说明其来源，避免把此前的 UI 删除计入 Runtime 重构成果。

## 2. 已完成的 Runtime 重构

### 2.1 Core 与 Host 的职责收缩

Core 继续只有纯转换入口 `update(Model, Msg) -> Step{Model, Cmd}`，每次返回一个命令。Model 保存值，线程、共享取消标志、inbox 和唤醒资源归 Host。单会话只有一个逻辑活动操作，工具顺序执行。

Host 删除 operation 映射、配套 mutex、退休协议，以及处理消息时对终态分类和 Core Phase 的回读。现在只保留一个 `CurrentOperation` 取消句柄槽位：开始新工作先作废旧句柄，精确匹配的取消命令作废当前句柄，NoCommand 保留槽位。请求准备在读取动态依赖前就作废旧句柄，因此准备失败也不会留下旧操作继续生效。

worker 自持执行输入及资源所有权。普通流和压缩流共用取消检查、异常转换、终态去重和缺失终态处理的私有执行函数；各自保留事件翻译。没有新增 TaskExited、effect batch 或通用任务管理器。

主要代码：[Host 声明](../include/my_agent/runtime/async_host.hpp)、[Host 实现](../src/runtime/async_host.cpp)、[Cmd 与 Step](../include/my_agent/runtime/agent.hpp)。

### 2.2 操作身份与迟到结果隔离

取消与用量统一使用 `OperationRef = variant<StreamRef, ToolCallRef, CompactionId>`。StreamRef 包含 TurnId 和 StreamId，ToolCallRef 保留 assistant MessageId 和 ToolExecId。原来的 OperationId / OperationIdentity / OperationKind / OperationOwner、hash 和拼装辅助函数删除。

Core 删除零 ID 兼容路径，拒绝零值、旧身份及已退出阶段的结果。Host 过滤取消后的回调，Core 再拒绝取消前已经入队的迟到结果。原始 ProviderEvent 可以尚未绑定身份，由 Host 根据命令快照绑定后再投递；这一点与直接进入 Core 的消息不同。

主要代码：[身份类型](../include/my_agent/runtime/identity.hpp)、[消息类型](../include/my_agent/runtime/msg.hpp)、[Core 转换](../src/runtime/agent.cpp)。

### 2.3 请求意图、准备结果与配置

RequestPurpose 与散装 ID 改为三选一 RequestIntent：队列新回合、工具续接、当前请求重试。PreparingRequest 只保存准备代次和意图。准备结果需要匹配代次与当前队首；通过准入后才提交队列消息，新请求和续接创建 assistant，重试复用目标 assistant。

PreparedRequestSnapshot 缩为最终请求、预算判断、校准回执。完整 ContextProjection 留在准备算法内部。`canonical_request(Thread)` 从同一份 Thread 读取消息和已接受压缩，删除可误配独立 Request 与 Thread 的公共投影入口；压缩规划的两个公共重载合并为一个入口。

RequestPolicy 将配置、provider name、model 组合为构造时校验、只读访问的值，沿用已有校验规则。Model、消息和执行快照使用该值，删除 Host 为配置更新单独复制 Model、处理校验异常的分支。

主要代码：[阶段与意图](../include/my_agent/runtime/phase.hpp)、[请求准备](../src/runtime/request_preparation.cpp)、[配置值](../include/my_agent/runtime/context_types.hpp)、[压缩与 canonical request](../src/runtime/compaction.cpp)。

### 2.4 用量更新集中

用量模块统一 token 规范化与累计规则。Core 负责验证活动身份、fingerprint 和阶段内是否已接受用量，再调用纯更新方法。普通请求更新当前 turn 与 session；压缩只更新 session 和 compaction 累计，不覆盖普通请求和真实上下文观测。

结构相同的 RequestUsageState / ObservedContextUsage 合并为 UsageObservation，删除 `turn.latest_request` 镜像；当前 turn 的质量从最近普通请求的归属推导。`latest_request` 和 `observed_context` 仍保留，因为缺少新用量时，需要同时表达“新请求不可用”和“较早真实观测已过时”。

主要代码：[用量状态](../include/my_agent/runtime/usage.hpp)、[用量更新](../src/runtime/usage.cpp)、UI 用量投影（后续精简已并入 [screen.cpp](../src/ui/screen.cpp)）。

### 2.5 数据布局及无消费者字段清理

Model 现在按 `phase / domain / session` 组织，阶段值和用量分别位于独立文件。旧 `d/s`、StreamState 名字及兼容别名删除。具名转换将准备、接纳请求、推进工具、完成和取消的主线集中起来。

实际删除的冗余字段还包括 Model::revision、StartStream.target_message_id、Streaming 的重复 fingerprint、CompactionTrigger，以及没有读取者的压缩输入统计和部分 projection 诊断字段。Core 所需 assistant 锚点、PreparationId、阶段内 usage_accepted 等仍保留。

只统计 Runtime 头文件和源码，相对实施前工作区净减少 484 行，已计入新增的 phase / usage 文件，未计入此前 UI 清理。行数只反映规模；拆文件和改名属于阅读组织，真正减少的复杂度来自重复协议、字段与执行路径的删除。Phase 仍有七种。

完整删除对照见 [Runtime 重构验证与删除清单](RUNTIME-REFACTOR-VALIDATION.md)。

## 3. 一并提交的既有 UI 清理

此前的工作删除了旧 `Frame -> view -> maya_projection` 路径、旧 Markdown scanner、input layout、style 和旧 status bar 组装接口，以及对应库目标、测试和 Painter 重载。

当前 UI 主线是 `Model + UiState -> ScreenConfig -> Maya -> Painter`。UiState 独立保存编辑及 UI telemetry 状态；本轮同步迁移了 Model 字段、请求意图和用量投影的消费者，并补齐 Core 链接依赖。项目继续自行驱动事件循环，Maya 负责渲染。

旧路径删除不代表其展示能力已经全部迁入新路径：现行助手正文仍使用 PlainText，工具输出完整显示。Markdown、工具输出显示上限和 transcript 缓存是否需要实施，仍是 [TODO](../TODO.md) 中的待评估事项。

## 4. 代码库目前处于什么阶段

当前已经具备本地 Agent 的完整主循环，而非只有架构骨架：

- 入口连接本地 Ollama；终端使用 TUI，非 tty 使用行式 REPL。
- 支持 FIFO 消息队列、流式输出、工具顺序执行、权限审批、错误处理与重试。
- 支持上下文预算准入、自动与手动压缩、provider 用量归属和真实用量校准。
- SessionServices 注册 7 个工具，覆盖 calculator、read、list_dir、memory、skill 和文档检索。
- UI 使用语义屏幕路径，具有输入编辑、滚动、队列与权限呈现。
- Anthropic / OpenAI 的传输与协议实现已有测试，但当前 main 入口仍只接 Ollama。

本阶段的成果是让已有能力共用更明确的状态和执行合同。它没有新增并行任务、持久化恢复或云端 provider 选择入口，也没有承诺生产级任务终止能力。

当前应以 [Runtime 架构](RUNTIME-ARCHITECTURE.md) 为实现合同。[旧 Model 指南](MODEL-REFACTORING-GUIDE.md) 和 [旧队列状态机设计](QUEUE-TURN-STATE-MACHINE-DESIGN.md) 已注明被替代部分；其历史结构不应重新作为新增实现的约束。

## 5. 验证结果与边界

| 验证 | 结果 |
| --- | --- |
| Runtime 实施前完整构建 | 通过 |
| 实施前沙箱内完整回归 | 519 项：508 通过、9 跳过、2 失败 |
| 按五个切片编译及相关行为测试 | 已完成；详细结果见验证文档 |
| 实施完成后的全项目构建 | 通过，包含 REPL、UI、provider、工具、测试和 benchmark 目标 |
| 实施完成后的沙箱外完整回归 | 533/533 通过，0 失败、0 跳过，23.43 秒 |
| 与 Runtime 实施前工作区的用例集合比较 | 519 项全部保留，新增 14 项行为测试 |
| 阶段提交整理时复核 | 增量构建无待构建目标；diff 空白检查与总结文档本地链接检查通过 |

基线的 calculator 失败是断言期待 6 个工具，而生产注册表已经有 7 个；已同步数量和名称断言，执行与 schema 断言保留。HTTP 读超时失败来自沙箱拒绝创建 socket；获准在沙箱外运行后通过，最终真实 TLS、本地 Ollama 和 HTTP 超时测试均实际执行。

这里的“没有删除基线用例”以已有 UI 清理后的工作区为基准。相对父提交，阶段快照仍包含此前对旧 UI 专用测试的删除，两者不矛盾。

构建和完整回归是在本次总结整理前、同一份实现上完成的。本次提交整理仅增加总结及文档入口；没有进一步修改实现。完整记录见 [验证文档](RUNTIME-REFACTOR-VALIDATION.md)。

仍存在以下实际限制：

- 取消是逻辑取消，不撤销工具已发生的副作用，也不强制杀死后台任务。
- 隔离工具线程可能晚于 Host 退出；卡住的共享池 provider 仍可能阻塞析构 join。
- 业务静止不等于所有 worker 已退出。
- ID 唯一性限定于当前线性会话，未实现跨进程恢复和持久化身份。
- 请求准备仍在 owner thread 同步执行；尚无测量证据要求将其异步化。
- 网络集成测试依赖相应权限、本地 Ollama 和外部 TLS 服务；本阶段未重新开展完整的人工 TUI 验收。

五个 Runtime 切片没有未完成项，最终回归没有已知失败；上述限制与后续能力不应写成已经解决。

## 6. 推荐的下一步

### 第一优先级：用真实任务验收当前主线

建议下一轮先做一次有记录的真实使用验收，保持当前架构边界，并围绕具体问题补最小修复。优先使用 Ollama 完成“浏览项目目录 -> 读取源码 -> 检索文档 -> 得出回答”的任务，再覆盖：

| 场景 | 观察和验收重点 |
| --- | --- |
| 流式期间连续提交多条 prompt | 严格 FIFO，输出不串 turn，队列提示与实际执行一致 |
| 工具审批批准、拒绝及活动操作取消 | 工具结算和续接符合预期，旧结果不污染下一回合 |
| 降低上下文预算并持续对话 | 压缩或阻塞可理解，失败不破坏历史，估算与真实用量明确区分 |
| 长 transcript、长工具输出、resize 和退出 | 可读性、滚动行为、交互延迟和退出耗时可接受 |

产物应是任务记录、可复现问题和必要的行为测试，而不是先增加一轮架构抽象。若需要性能优化，先测量请求准备耗时、transcript 构建/绘制开销，再决定异步准备、缓存或显示上限。

### 随后的功能建议：最小会话历史持久化切片

如果真实使用没有暴露更高优先级问题，建议下一项功能考虑保存并重开已结算的会话历史，使当前内存中的工作结果能够保留。这比继续按文件长度拆分 agent.cpp 更直接地改善使用价值。

该方向尚未设计和实施。应先明确保存格式、兼容版本、恢复后的 ID 分配，以及未结算工具调用的处理；首个切片宜围绕已结算历史闭环，不把运行中恢复、后台任务重启和副作用重放混入其中。验收重点是保存/重开一致、错误可见，以及继续对话时身份不冲突。

Markdown、工具输出显示上限、云端 provider 入口和更完整的命令栏可作为后续产品选择，依据真实任务需求排序。当前没有证据要求引入多操作调度、通用任务管理器、effect batch 或 Model 中的执行资源。

## 7. 继续阅读代码的顺序

1. [model.hpp](../include/my_agent/runtime/model.hpp)、[phase.hpp](../include/my_agent/runtime/phase.hpp)、[identity.hpp](../include/my_agent/runtime/identity.hpp)：状态归属和身份。
2. [agent.cpp](../src/runtime/agent.cpp)：drive -> start_request_preparation -> accept_prepared_request -> kick_pending_tools / finish_turn / cancel_active。
3. [async_host.cpp](../src/runtime/async_host.cpp)：三步消息处理、取消槽位和 worker 输入快照。
4. [request_preparation.cpp](../src/runtime/request_preparation.cpp)、[compaction.cpp](../src/runtime/compaction.cpp)、[usage.cpp](../src/runtime/usage.cpp)：预算、压缩与统计规则。
5. [screen.cpp](../src/ui/screen.cpp)、[painter.cpp](../src/ui/painter.cpp)、[ui_state.hpp](../include/my_agent/ui/ui_state.hpp)：当前唯一 UI 路径。
