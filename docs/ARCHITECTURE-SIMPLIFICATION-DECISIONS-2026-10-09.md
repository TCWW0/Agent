**架构精简决策与实施依据：2026-10-09**

状态：五组架构精简已完成并通过本轮全项目构建和完整回归。九项决策及应用约束保持；实际删除、测试迁移、保留证据和验证见第 11 节，阶段交付总结见第 12 节。

评审基线：`my_agent` 分支 `dev/tool-system`，提交 `a1b7df3`。整理本文前工作区干净。本文中的消费者与调用关系结论来自该基线的静态源码检查，实施时应重新核实。第 4–8 节的“当前”描述评审基线，最新实际合同见第 11 节及 Runtime 架构说明。

相关材料：[上一阶段总结](REFACTOR-MILESTONE-2026-10-09.md)、[当前 Runtime 合同](RUNTIME-ARCHITECTURE.md)、[上一阶段实施方案](RUNTIME-ARCHITECTURE-REFACTOR-PLAN.md)、[设计讨论所依据的文章](../../sum-of-simple-things.md)。文章位于父工作区，单独检出 `my_agent` 时可能不可用。本文已完整记录本轮决策，不依赖该文章才能实施。

**1. 本轮目标与范围**

用户当前的目标是精简已有设计：删除无意义字段和状态，收缩混乱的关系、重复协议和跨层传递，展开无独立职责的调用绕行，使已有主线更容易理解和维护。

判断一个结构是否值得保留，要回答：它承担什么必要工作？删除后丢失的是行为约束，还是仅少了一次转发、一份副本、一条需要调用者记住的约定？

本轮以工具解析与执行、上下文计算、Core 续接调用链、UI 用量投影为主要范围。此前评审提出的持久化、Provider 入口扩展、网络物理取消、通用调度以及 UI 新展示能力，不列入本轮交付。现有 Provider 统计旁路等相邻问题也不自动并入；修改应服务于本文已确认的精简目标。

收益用实际删除的字段、状态、执行路径、重复计算和隐含约定说明。行数作为辅助信息，不能以拆文件、改名或新增包装层代替精简。

**2. 已确认的九项决策**

| 编号 | 决策 | 实施含义 |
| --- | --- | --- |
| D1 | 以现有应用行为和必要约束为兼容边界 | 允许修改内部公共头文件、消息结构、构造参数和函数接口；不为无人使用的旧接口保留兼容层 |
| D2 | 用更早、更强的构造约束消除内部协议缺失 | 让“Host 没提供解析结果”的消息无法正常构造；未知工具、非法参数、过期事件等外部或异步失败继续显式处理 |
| D3 | 无生产消费者的数据直接删除 | 不为未来可能的统计或诊断保留字段；正在使用的诊断入口属于真实消费者 |
| D4 | 测试随内部合同迁移，并遵守 TDD | 保留行为覆盖，替换依赖旧结构的断言和注入方式；由类型消除的状态改用构造约束验证 |
| D5 | 工具执行只保留一个入口 | 统一经过 ToolRuntime 的注册 handler；测试使用注册的假 handler，移除 Host 的备用执行入口 |
| D6 | UI 用量按当前 Model 构建快照 | 删除长期保存的派生用量及重复投影；编辑状态、时间采样、速率历史等跨帧数据仍由 UI 保存 |
| D7 | 上下文接口按现有用途收缩 | 删除无调用者路径和多余转发重载；测试便捷组装放在测试辅助代码中 |
| D8 | 经证明冗余后，调整重复信息的归属 | 先核实同源关系、生命周期和消费者，再决定由集合或上层对象统一持有 |
| D9 | 以减少信息绕行为标准平铺调用链 | 优先删除纯转发、丢失身份后重新查找的调用；保留集中表达完整业务转换的函数 |

其中 D3 的理由是没有使用场景的数据会增加理解负担；D6 的理由是用量派生值可以在需要时从当前 Model 获得，新鲜度由投影时机保证。未来确有需求时再讨论 UI 快照或缓存，不预先保留无消费者结构。

**3. 精简过程中必须保持的约束**

- Core 仍为 `update(Model, Msg) -> Step{Model, Cmd}`，Model 持有值；线程、取消资源、inbox 和时钟归执行层或 UI。
- 单会话只有一个逻辑活动操作，工具顺序执行，队列遵守 FIFO。
- 审批与执行报告继续匹配完整身份。`ToolCallRef` 和 `ToolRef` 各自承担调用归属及绑定校验，平铺消息时保留这些职责。
- 保留 Host 的取消回调过滤，以及 Core 对已入队迟到结果的拒绝。逻辑取消不新增物理中止或副作用回滚保证。
- 请求准备代次、当前队首匹配、请求快照、预算准入、压缩验证和用量接受一次等约束保持。
- `latest_request` 与 `observed_context` 保留各自语义：本次用量不可用时，仍可保存上次真实观测并标记过时。
- 真实用量与估算值继续区分；UI 数据归属调整保持既有展示语义。
- 有独立生命周期的启动快照与当前配置不因字段相似而合并。

内部结构变化需要迁移消费者；应用行为或上述约束若需改变，应作为新的设计取舍讨论。

**4. 工作组一：工具解析合同**

当前 [msg.hpp](../include/my_agent/runtime/msg.hpp) 的 `ToolCallResolution` 有 `UnresolvedTool`、`ToolBinding`、`ToolResolutionError` 三个分支。[Host](../src/runtime/async_host.cpp) 的正常路径总会执行解析并产生成功或失败，Core 仍为缺失结果维护一个阻塞分支。

已确认方向：进入 Core 的已解析工具调用必须显式携带成功或失败结果，消除“没有解析结果”第三态。未知工具对应明确解析失败，继续记录失败且不能进入审批或执行。

实施时选择最小的构造约束即可；只把三分支 variant 改成两分支，若仍可默认构造出假成功结果，不能认为已经实现 D2。也不能仅删除 Core 分支，却保留能够省略解析结果的入口。具体类型、构造函数或工厂形式留给实施验证，不要求建立通用验证框架。

测试落点：公开消息构造约束和 `update`。迁移 [TS005 测试](../tests/ts005_resolved_stream_calls_test.cpp) 中的缺失解析结果场景，保留未知工具失败、错误身份忽略和正常绑定执行的行为覆盖。对类型已经排除的输入，不继续保留运行时处理分支来服务旧测试。

**5. 工作组二：工具执行入口与结果消息**

当前 [AsyncHost](../include/my_agent/runtime/async_host.hpp) 同时持有 ToolRuntime 和可选 `ToolExecEffect`，执行时判断是否绕过 runtime。真实入口使用 ToolRuntime；部分测试注册占位 handler，再用第二个入口替换执行逻辑。[ToolRuntime](../src/tool/tool_runtime.cpp) 与 Host 还分别实现了异常转换。

已确认方向：测试与应用都通过注册 handler 执行。迁移取消、阻塞、异常、迟到结果测试后，删除 `ToolExecEffect` 类型、构造参数、成员、捕获和选择分支。统一异常转换责任前，应区分 handler 异常与 Host 自身执行错误，保留仍有必要的防护。

以下项已查明没有生产消费者，可作为明确删除项：

| 项目 | 当前成本 | 删除范围 |
| --- | --- | --- |
| `ToolExecutionReport.elapsed_ms` | Host 读取时钟、计算时长、随消息传递 | 字段及专门服务于它的计时和转换 |
| `ToolExecutionReport.model_visible_output_bytes` | 计算输出长度；失败时额外格式化错误文本 | 字段及无消费者的计算 |
| `ToolExecutionError` | 未使用的协议类型 | 类型与相关无用声明 |

删除统计后，评估将 `ToolExecOutput{call, report{ref, result}}` 平铺为直接携带 `call`、`ref`、`result`。基线中 report 没有独立生产消费者；实施时复核后可以删除该包装。调用身份与绑定身份继续用于校验。

测试落点：`ToolRuntime::execute` 和 AsyncHost 的 dispatch、消息泵及 Model 观测边界。重点保留标准与非标准异常转换、审批、取消上下文传播、拒绝不匹配报告，以及旧结果不污染下一操作的验证。

现有测试参考：[工具报告](../tests/ts006_tool_execution_report_test.cpp)、[Host](../tests/async_host_test.cpp)、[工具循环](../tests/tool_loop_test.cpp)、[UI 循环](../tests/ui_loop_test.cpp)。

**6. 工作组三：上下文计算接口与中间数据**

明确的删除或收缩方向如下，证据集中在 [context.cpp](../src/runtime/context.cpp)、[上下文类型](../include/my_agent/runtime/context_types.hpp)、[投影类型](../include/my_agent/runtime/context_projection.hpp) 和 [工具裁剪类型](../include/my_agent/runtime/tool_replay.hpp)。

| 候选 | 基线证据 | 处理方向 |
| --- | --- | --- |
| `ContextUsageAnchor` 及准入函数的可选参数 | 现有应用和测试调用均未传入；有效校准来自 Ledger → Projection | 删除类型、参数及专属分支 |
| `SessionCalibration.sample_count` | 只递增，没有读取者 | 删除无效累计状态 |
| `project_context` 六个公共重载 | 多数仅补默认参数并转发 | 按生产用途收缩；测试便利组装归测试辅助代码 |
| `ContextBudgetAssessment.quality / estimated` | 构造时表达同一判断，生产 UI 使用 estimated | 复核消费者后保留一个表示 |
| `ToolReplayProjection.items` | projected_text 与最终 Request 的输出重复，明细主要由测试读取 | 测试迁移至最终请求后删除无消费者报告及组装逻辑 |

工具裁剪仍需保留预算、分配、裁剪标记以及真正用于算法的摘要或策略 fingerprint。不能把报告类型中的所有字段都视为无用：有些信息参与当前计算，应收回局部变量或保留其必要结果。

另外两项需要进一步证明，不作为无条件删除承诺：

- segment 上的估算器版本、策略版本，与 Projection、Receipt、Anchor 的外层版本重复。检查是否存在合法混合版本集合；若始终同源，则统一由集合持有，并删除逐段复制和重复比较，保留内容身份校验。
- 其他预算回退字段、压缩启动快照及完整性检查，必须追踪实际用途后再处理。不能用“当前默认值一样”代替生命周期和依赖证明。

测试落点：请求准备公开入口、保留的上下文投影/准入公开边界，以及压缩规划和验证入口。检查最终 Request、准入判断和压缩接受结果；不为每个私有步骤新增测试接口。

迁移裁剪测试时保留预算限制、UTF-8 完整性、近期结果优先、裁剪标记、确定性和原始历史不被修改等已有行为覆盖。校准与压缩测试继续检查配置或内容改变时的行为。

现有测试参考：[上下文准入](../tests/context_admission_test.cpp)、[上下文维护](../tests/context_maintenance_test.cpp)、[工具裁剪](../tests/tool_replay_test.cpp)、[压缩](../tests/compaction_test.cpp)、[Core 用量与请求测试](../tests/agent_loop_test.cpp)。

**7. 工作组四：Core 调用链平铺**

在 [agent.cpp](../src/runtime/agent.cpp) 中，`kick_pending_tools` 已取得当前 assistant，工具结算后只将 turn_id 传给 `start_continuation`；后者反向扫描历史找回 assistant，再构造续接意图。基线中该函数只有一个调用点。

已确认方向：使用已经掌握的准确身份，消除“已知对象 → 丢失身份 → 重新搜索”的绕行。核实正常可达状态与异常状态测试后，直接以 assistant 的 turn_id、id 构造续接意图，评估删除扫描帮助函数及其无必要兜底。

核实重点：是否存在合法状态要求选择同一 turn 中另一个更晚的 assistant；已有对象的使用是否跨越容器修改或状态转换。若语义不等价，先保留原行为并记录原因。

`finish_turn → drive → start_request_preparation` 集中了回合结束、FIFO 推进和准备规则，可以继续保留。平铺以减少信息绕行为依据，不追求最少函数数量。

测试落点：`update`。验证工具批次结算后续接正确 assistant 所在回合，审批拒绝与执行失败仍按既有规则推进，队列和迟到结果隔离保持。复用 [agent_loop_test.cpp](../tests/agent_loop_test.cpp) 与 [tool_loop_test.cpp](../tests/tool_loop_test.cpp) 的行为场景。

**8. 工作组五：UI 派生状态收缩**

当前链路是：`ui_loop` 调用 `project_model_usage`，将结果存入 `UiState.status`；`project_screen` 再次调用同一投影，并创建仅承载状态栏数据与速率历史的临时 UiState，传给 `status_config`。

已确认方向：用量在屏幕构建时从当前 Model 统一投影，删除 UiState 中长期保存的派生用量。缩窄 `status_config` 参数，消除为调用它而组装临时 UiState 的过程。

实施时区分三种信息：

- Model 已拥有的事实：请求、回合和上下文用量及其质量；需要时投影。
- UI 必须跨帧保存的信息：编辑状态、时间采样、速率历史；保留原归属。
- 外部提供的展示配置与 telemetry：依据真实用途安排输入，保留既有显示语义；本组不自动扩展为 Provider 观测协议重构。

测试落点：`project_screen`。同一 UI 输入配合更新后的 Model，应直接得到最新用量；同时保留 unavailable、last、stale 的区分，以及预算估算不能覆盖真实观测的行为。原状态栏测试按保留接口或屏幕边界迁移。

代码参考：[ui_loop.cpp](../src/ui/ui_loop.cpp)、[screen.cpp](../src/ui/screen.cpp)、评审基线中的 `status_bar.cpp`（已删除）、[UiState](../include/my_agent/ui/ui_state.hpp)。测试参考：[状态栏](../tests/status_bar_test.cpp)、[屏幕](../tests/screen_test.cpp)、[UI 循环](../tests/ui_loop_test.cpp)。

**9. TDD、切片与实施顺序**

用户已确认上述测试边界与验收方向。既有技能约束要求测试放在已约定的公开边界，并按行为逐个推进；不要为内部辅助函数或被删除的结构建立新接缝。

执行规则：

1. 实施前核实 HEAD、工作区、构建方式和适用仓库指引；取得本次可比较的构建与测试基线。上一阶段的 533 项通过记录不代表本轮验证结果。
2. 对新增或收紧的公开合同，一次选择一个行为，先写或迁移失败测试，确认失败原因，再完成最小实现并验证通过。构造约束可以通过编译期检查验证。
3. 对纯删除和内部等价整理，在已有行为测试通过的基础上重构。需要补充既有行为保护时先补测试；这种测试可能在旧实现上已经通过，不应伪称为一次红绿循环。
4. 不先批量重写全部测试，再批量修改实现。每个小切片完成消费者迁移与相关验证后再继续。
5. 测试使用已知输入、独立预期和公开结果。避免断言私有函数调用次数，或为证明字段已删除而添加与实现结构镜像的测试。
6. 删除旧测试之前，说明其行为由哪个保留/迁移测试覆盖，或由哪项构造约束取代。测试数量不是本轮验收指标。

建议依赖顺序：工具解析合同 → 工具执行入口与报告 → 上下文计算收缩 → Core 调用链 → UI 派生状态。每组仍需拆成小的行为切片；工作组之间可按实际依赖调整，不以一次性大改完成。

**10. 验收与后续记录**

每个切片至少记录：

| 记录项 | 内容 |
| --- | --- |
| 原关系 | 谁产生、谁消费、为何冗余或绕行 |
| 实际删除 | 字段、类型、分支、辅助函数、执行入口或重复计算 |
| 保留约束 | 删除后由哪个位置或类型负责保证 |
| 测试迁移 | 原行为映射到哪个公开边界；构造约束替代了哪个运行时状态 |
| 验证 | 实际运行命令、结果、失败或环境限制 |
| 未删除候选 | 发现独立用途、生命周期不同或证据不足的原因 |

完成标准：

- 本文明确收缩项已实施并迁移消费者；条件候选已给出删除或保留的证据。
- 没有为旧接口留下无消费者的兼容别名、备用执行路径或重复状态。
- 应用行为及第 3 节约束得到相关测试保护，全项目构建和完整回归已执行并如实记录。
- 更新实际受到影响的架构说明，区分本轮完成项与上一阶段记录；本文状态根据真实实施进度更新。
- 结果能说明减少了哪些必须同时理解的关系；不以减少测试或堆叠新抽象换取表面上的简短。

本轮讨论已通过 grilling 完成九项取舍，并确认了测试边界。剩余字段归属与调用等价性核实属于实施前的代码事实检查；在既定行为和约束内可据证据决定。若发现需要改变用户可观察行为或保留约束，再提出具体取舍。

**11. 本轮实际实施记录**

实施起点 HEAD 为 `a1b7df38c021cbaa75ee01c8c4ace9cf113c345e`（`dev/tool-system`）；实施期间未切换提交。开始时只有 `.gitignore` 的决策文档例外和本文件未提交，两者均保留。适用父目录、项目及修改目录没有 `AGENTS.md` 或 `CONTEXT.md`。已阅读 TDD 技能及其 tests/mocking 说明，并沿用第 4–8 节已确认接缝。

本轮使用 `/root/agent_learning/build`（Debug / Ninja / GCC 15 / C++26，本地 Maya）；项目内 `build/` 是较旧构建目录，尝试时触发重新配置，之后停止这次重复构建，未将其部分输出算作基线或验收。日志统一写入 `/tmp/my-agent-simplification-*`。

本轮基线全项目构建通过（`cmake --build /root/agent_learning/build -j 4`）。沙箱内完整回归：533 项，523 通过、9 跳过、1 失败（HTTP socket 权限）；按权限要求沙箱外重跑：533/533 通过、0 跳过，23.69 秒。这是本轮重新运行的结果。

| 切片 | 原关系和实际删除 | 保留约束与测试迁移 |
| --- | --- | --- |
| 工具解析 | 删除 `UnresolvedTool` 和 Core 的无解析结果阻塞分支；两分支解析结果由消息构造函数强制显式传入，并拒绝零绑定/空 variant 的假成功 | 编译期拒绝默认构造和四参数省略结果构造，允许显式绑定/解析失败。旧 `UnresolvedBoundaryFailsClosedWithoutExecution` 由这些构造检查取代；未知工具、原始字段、完整流身份和顺序执行测试保留 |
| 单一工具执行 | 删除 `ToolExecEffect` 类型、Host 第三构造参数、成员、worker 捕获和选择分支 | TS006、Host 的取消/阻塞/准备失败以及 UI PTY 批准/拒绝改为注册假 handler；从参数、取消上下文、最终工具输出和屏幕观察。先在旧接口仍存在时迁移并通过，再删除备用入口 |
| 工具结果 | 删除 `ToolExecutionReport`、`elapsed_ms`、`model_visible_output_bytes`、`ToolExecutionError`，以及时钟采样、时长转换、输出长度和额外失败格式化 | `ToolExecOutput` 直接携带 call/ref/result；TS005、TS006、tool_loop 迁移。完整调用身份、绑定 ToolRef、取消/迟到结果隔离保留；原统计名称不进入历史的结构断言删除，最终结果与续接断言保留 |
| 上下文闲置值 | 删除 `ContextUsageAnchor`、准入第四可选参数及分支、`SessionCalibration.sample_count`、预算 `quality` | 没有调用者传 anchor；校准仍来自 ledger/receipt；生产 UI 使用 estimated；请求准备、准入、维护、压缩和 Core 行为测试通过 |
| 裁剪报告 | 删除 `ToolReplayItem`、`ToolReplayProjection`、`ContextProjection.replay` 和内部 `ReplayResult`，以及明细组装、重复文本副本和只为报告汇总的 token/byte 计算 | 裁剪直接返回最终 Request；预算/分配保留局部变量，摘要在裁剪标记需要时计算，策略 fingerprint 继续组成投影版本。全部裁剪测试先迁至最终 Request 并在删除前通过；UTF-8、预算、近期优先、小结果、确定性、ephemeral、provider 请求体和原历史保护均保留 |
| 投影入口与版本 | 六个公共 project_context 重载缩为生产使用的完整入口；删除私有 make_projection 转发；两个实际使用的测试便利组装移至测试辅助文件；删除 segment 的 estimator/policy 版本及逐段赋值/比较 | 唯一 manifest 在一次调用内生成；原代码用同一外层版本遍历覆盖全部段，receipt→anchor 整集合复制，没有合法混合来源。外层 provider/model/版本校验、逐段 kind/stable_id/content fingerprint 保留；配置/内容失配保护在删除前后均通过 |
| 重复预算值 | 删除 projection 的 calibrated_input_tokens、anchor input/output，budget 的 realistic input、anchor input/output，准入的零值多层回退和规划中零值重算；删除 CompactionPlan.summary_output_budget 副本 | 投影总会从 manifest/framing 生成 projected input（先校准，命中 anchor 时替换）；所有生产准入和规划使用这个结果。原始 realistic input 仍用于 receipt 校准。压缩请求的 max_output_tokens 与启动 policy 派生预算直接比较；请求 fingerprint、源摘要/覆盖边界及 no gain 检查保留 |

版本/内容行为保护测试最初把测量输入 100 直接当成最终预算，运行旧算法实际得到 102；核实是既有校准后的 framing 差值 +2 后修正独立预期并通过。没有修改该行为，也不将此测试算作新功能的红绿循环。真正的构造合同红测试因旧消息允许默认和省略结果构造而编译失败，然后实现后通过；最后又以消息构造测试确认空 variant 不抛错的红结果，增加零绑定构造检查后通过。

| 后续切片 | 原关系和实际删除 | 保留约束与测试迁移 |
| --- | --- | --- |
| Core 续接 | 删除 start_continuation、反向历史扫描及其兜底；当前 assistant 的 turn_id/id 直接进入准备意图 | 合法串行状态中，另一 assistant 只在结算后的请求接受时创建；Submit 只入队，不会插入历史。ID 在改变 Phase/复制请求前取值，没有跨容器修改悬空引用。先加强既有工具续接测试的最终 StreamRef/回合断言并通过，再删除扫描；审批、执行失败、FIFO、迟到隔离继续覆盖 |
| UI 用量 | 删除 StatusBarInput 的八个派生用量字段、project_model_usage、ui_loop 重复投影、临时 UiState 及其速率历史副本；删除仅承载旧投影的 status_bar.cpp 和库目标 | status_config 直接接收当前 Model、展示/telemetry 输入和速率历史。五个状态栏测试先迁至 project_screen 及解码后的屏幕，再删除旧投影。新增相同 UiState 下更新/移除 Model 观测的行为保护；unavailable/last/stale、预算不覆盖实测和宽度降级保持 |
| 最终消费者审计 | 删除 anchor 的 operation/request_fingerprint 镜像及 record_exact_usage 第五参数；删除预算 effective_context_limit/output_budget/safety_margin/trigger_input_limit/remaining_headroom 及无消费者计算 | 实际生产只使用 projected/hard input、estimated、triggered、admitted 和两个 debug 字段；限制和输出 cap 由配置/最终 Request 持有。预算测试先改断言已知 hard limit 与最终请求 cap；屏幕只移除无用输入值，输出断言保留。原 anchor 身份断言迁到真实 latest_request 观测；Core 的完整身份、fingerprint 和接受一次校验保持 |

**保留候选及证据**

- Projection、Receipt、Anchor 与 Calibration 各自的外层 provider/model/estimator/policy 信息保留：准备回执、已接受观测和跨请求校准的生命周期不同，后续配置变化必须拒绝旧集合。删除的是同一集合逐段复制的版本；配置/内容失配测试验证行为。
- ContextBudgetAssessment.incremental_input_tokens/exact_anchor_reused 保留：screen.cpp 的环境开关 debug 路径实际读取，展示 delta 与 anchor hit/miss。估算仍只用于准入/诊断，不覆盖真实用量。
- make_compaction_plan 的可选 next_budget 保留：手动压缩实际没有准备 assessment，需要计算；自动维护由 Core 传准备结果，复用已算值并保留校准。删除的是生成 projected input 后的冗余零值重算。
- CompactionPlan.policy、source_request、compaction_request、next_request、source_digest、covered_through、before_tokens 和验证保留：启动配置冻结，源与发出请求各自校验，候选在待发请求上验证准入与净收益。ActiveCompactionKeepsItsCapturedPolicyAfterPolicyUpdate、源被篡改/请求被篡改/无净收益等测试保护；只删除同源同生命周期的输出预算副本。
- Host 的执行调用异常防护保留：注册 handler 异常由 ToolRuntime 转换，Host guard 针对调用本身在结果生成前的错误。取消回调过滤和 Core 已入队迟到结果拒绝承担不同时间窗口。
- finish_turn→drive→start_request_preparation 保留：分别完成回合结束、FIFO 推进和请求准备的业务转换，不是丢失身份后的纯转发。
- latest_request/observed_context 保留：缺失新用量时同时表达“本次 unavailable”和“较早真实观测 stale”，已有 Core 和屏幕行为测试保护。
- UiState 的编辑、LiveTokenRate、TokenRateHistory、外部展示配置和速率/耗时 telemetry 保留：这些有跨帧历史或独立外部来源；StatusProvider 的生产入口实际提供 model/limit/rate/count/generation/time，未将相邻 provider 统计协议重构并入本轮。

**切片验证**

构建统一使用 `cmake --build /root/agent_learning/build --target <相关目标> -j 3`（第一解析切片用 `-j 4`）；测试统一使用 `ctest --test-dir /root/agent_learning/build --output-on-failure [-j 4] -R '<对应前缀>'`。日志前缀均为 `/tmp/my-agent-simplification-`；迁移阶段先在旧实现上验证，删除阶段再次验证。

| 切片及日志后缀 | 测试前缀/边界 | 实际结果 |
| --- | --- | --- |
| resolution-red / resolution-green-build / resolution-tests | TS005 构造；ts005、ts006、agent_loop、tool_loop | 两个禁止缺失结果的 static_assert 编译红；实现后构建通过，70 项通过 |
| handler-migration / single-executor | ts006、async_host、ui_loop | 迁移与删除各 64 项通过 |
| tool-output | ts005、ts006、tool_loop、async_host | 构建通过，54 项通过 |
| context-unused | context_admission、context_maintenance、agent_loop、compaction、tool_replay | 构建通过，61 项通过 |
| replay-migration / replay-delete | tool_replay；再加上述上下文组 | 迁移 7 项通过；删除后 61 项通过 |
| calibration-protection / projection-api / context-values | 请求准备版本/内容保护；上下文组 | 保护 8 项通过；入口/版本删除及重复值删除各 62 项通过 |
| continuation-protection / continuation | tool_loop；tool_loop、agent_loop、ts006 | 原实现保护 18 项通过；删除后 65 项通过 |
| ui-migration / ui | status_bar、screen；再加 ui_loop、dock_layout、stream_rate | 原实现迁移 26 项通过；删除后 73 项通过 |
| empty-resolution-red-test / empty-resolution-green-tests | TS005 消息构造 | 先实际失败（空 variant 未抛错）；最小构造检查后 6 项通过 |
| budget-migration / final-consumers | 准入/准备；再加 compaction、agent_loop、status_bar、screen | 迁移 9 项通过；删除后构建通过，81 项通过 |

**最终验证与完成状态**

- `cmake --build /root/agent_learning/build -j 3`：全项目构建通过，包含 REPL、UI、provider、工具、所有测试和 benchmark。日志：`/tmp/my-agent-simplification-final-build.log`。
- `ctest --test-dir /root/agent_learning/build --output-on-failure -j 4 --timeout 90`：使用与本轮基线相同的沙箱外执行环境，**535/535 通过，0 失败、0 跳过，23.61 秒**。TLS、本地 Ollama 流与工具调用/未知模型、HTTP 超时及 PTY 测试实际执行。日志：`/tmp/my-agent-simplification-final-tests.log`。
- 用例名称对比：533 个基线名称中，3 个被替换；缺失解析结果场景由编译期构造约束及空 variant 构造测试取代，另两个 TS006 名称随 handler/结果合同迁移。除此之外基线用例全部保留。另增请求准备配置/内容失配与屏幕当前 Model 观测行为保护。最终列表：`/tmp/my-agent-simplification-final-test-list.json`。
- `git diff --check` 通过；本轮变更的三个架构文档本地链接全部可解析。`.gitignore` 的原有例外保持，无旧接口兼容别名、备用执行路径或新的生产包装层。
- 实际环境限制：沙箱拒绝网络 socket，基线因此有 HTTP 失败和网络跳过；已在获准环境实际重跑并通过。构建仍有依赖 nlohmann/json 使用 `std::is_trivial` 的既有弃用警告，无构建失败。本轮没有追加人工 TUI 会话验收；屏幕渲染、PTY 和完整自动回归已运行。

五组工作均完成，本轮范围没有未完成事项或未通过测试。更新了 Runtime 当前合同及本文件实施记录，并在上一阶段总结中明确其历史快照身份；上一阶段的 533 项结果未作为本轮验收。既有逻辑取消、会话内身份和配置启动快照边界保留，未扩展功能或增加通用包装/调度层。

**12. 阶段交付总结**

本阶段在 `dev/tool-system` 上完成 D1–D9 的实施：让已有事实有明确归属，让调用者直接使用已有结果与身份，删除要求多层同步维护的副本和备用路径。应用行为与必要约束保持，交付范围为代码、测试迁移、当前架构说明、本决策及实施记录，以及让本文件入库的 `.gitignore` 例外。

| 主线 | 阶段交付结果 |
| --- | --- |
| 工具解析 | 消息必须显式携带绑定或解析失败；构造时拒绝零绑定，删除缺失解析结果第三态及其 Core 分支 |
| 工具执行 | 应用和测试统一执行注册 handler；结果消息直接携带调用身份、绑定身份和结果，删除备用入口、报告包装及无消费者统计 |
| 上下文 | 投影只保留一个完整生产入口；裁剪直接生成最终请求；删除闲置参数、重复预算值和逐段版本副本，保持校准、准入与压缩验证 |
| Core 续接 | 使用当前 assistant 的准确身份构造续接意图，删除反向历史查找及兜底，保持回合结束、FIFO 和准备转换 |
| UI | 构建屏幕时读取当前 Model 用量，删除长期保存的派生字段、重复投影和临时中转；编辑与速率历史继续跨帧保存 |

保留项均有当前消费者或独立生命周期证据：外层版本与启动配置快照、手动压缩所需预算计算、UI debug 字段、Host 与 Core 各自时间窗口的取消防护，以及 unavailable/last/stale 用量语义。详细证据见第 11 节。

新增或收紧的构造合同完成失败测试到最小实现的验证；等价删除在既有行为测试保护下推进。迁移后的测试主要检查最终请求、执行结果和屏幕输出。本轮全项目构建通过，完整回归 **535/535 通过，0 失败、0 跳过**。沙箱 socket 限制通过获准环境重跑解决；未追加人工 TUI 验收，已有屏幕与 PTY 自动验证通过。本阶段范围没有遗留事项。

本节随阶段提交入库，可用 `git log -1 --oneline -- docs/ARCHITECTURE-SIMPLIFICATION-DECISIONS-2026-10-09.md` 定位交付快照。构建目录和 `/tmp` 原始日志属于本机产物，不随提交保存；验证命令、结果和日志位置已记录于第 11 节。当前实现合同见 [Runtime 架构](RUNTIME-ARCHITECTURE.md)，[上一阶段总结](REFACTOR-MILESTONE-2026-10-09.md) 保留其历史边界。
