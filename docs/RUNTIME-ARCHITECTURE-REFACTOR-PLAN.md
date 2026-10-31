**Runtime 架构收缩方案：纯 Core、串行执行、明确的数据合同**

日期：2026-10-08；实施完成：2026-10-09。状态：全部五个切片已实施，用户已授权包含身份统一、用量归属及配置校验。

当前代码合同见 [Runtime 架构](RUNTIME-ARCHITECTURE.md)，验证与实际删除项见 [验证记录](RUNTIME-REFACTOR-VALIDATION.md)。以下保留已确认方案作为实施依据。

本方案基于当前工作区，包含上一轮 UI 遗留路径清理后的代码状态。对照版本为本地 `agentty` 的 `89e481d1`（2026-09-11），`my_agent` 的提交基线为 `15245b9`（2026-09-28）；工作区已有未提交修改，实施时在这些修改之上继续。

1. **目标与已确定的约束。**

   目标是让读者沿着“事件 → 状态转换 → 一个命令 → 执行 → 结果事件”理解一次请求，并减少必须跨模块记住的约定。用删除字段、分支和重复协议来衡量收缩效果。

   已经讨论确认的设计如下：

   | 项目 | 确定的合同 |
   | --- | --- |
   | Core | 纯状态转换；Model 保存值，执行资源由 Host 持有 |
   | 并发 | 一个会话只有一个逻辑活动操作；工具按顺序执行 |
   | 取消 | 立即逻辑取消；旧任务可以尚未退出，但不得更新当前会话 |
   | Host | 一个取消句柄槽位；已取消任务依靠自身持有的资源安全收尾 |
   | 命令 | 一次转换返回一个 Cmd；开始新工作时先作废旧句柄 |
   | 替代关系 | 删除跨 Phase/Cmd 传递的 supersedes |
   | 事件身份 | Core 严格校验身份；删除零 ID 兼容，迁移相关测试 |
   | 请求意图 | 用新回合、工具续接、重试三选一的数据结构替换散装字段 |
   | 准备结果 | 只返回最终请求、预算判断、校准回执 |

   身份统一、用量归属和配置校验已随全部五切片获得实施授权。功能范围沿用现有队列、权限、重试、压缩、用量显示和 provider 行为。

2. **从 agentty 借鉴的具体内容。**

   | 本地实现 | 可借鉴之处 | 在 my_agent 中的落点 |
   | --- | --- | --- |
   | [phase 转换函数](/root/agent_learning/agentty/include/agentty/domain/session.hpp:311) | 用有名字的转换集中迁移状态和相关数据 | Core 的准备、接纳请求、推进工具、结束与取消入口各自完成完整转换 |
   | [按领域组织的 update](/root/agent_learning/agentty/src/runtime/app/update.cpp:41) | 顶层推进路径容易找到，细节有明确归属 | 保留一个 update 入口；用量算法从 agent.cpp 移入自己的纯模块 |
   | [launch_stream](/root/agent_learning/agentty/src/runtime/app/cmd_factory.cpp:714) | 启动后台工作时明确捕获执行输入 | Cmd 自包含；Host 和 worker 使用命令快照 |
   | [Active 的生命周期数据](/root/agent_learning/agentty/include/agentty/domain/session.hpp:74) | 同一生命周期的数据一起出现、一起退出 | 请求意图、流身份和校准回执归入对应 Phase |

   采用这些做法时保留 my_agent 已确定的边界。agentty 的 Active 保存共享取消令牌，CancelStream 会直接操作它；my_agent 把取消令牌留在 Host。agentty 的工具调度支持并行批次，my_agent 使用单个逻辑活动操作。my_agent 继续拥有事件循环和数据化 Cmd，Maya 继续负责渲染。

   agentty 本身也有分散状态，例如 Phase 外的 compacting 标志，以及需要了解很多业务的 cmd_factory。参考源码用于验证具体设计选择，整体结构以本轮约束为准。

3. **目标结构与阅读主线。**

   ```mermaid
   flowchart LR
       I[用户输入与配置入口] -->|有效 Msg| C[Core update]
       M[Model 值状态] --> C
       C -->|新 Model| M
       C -->|一个自包含 Cmd| H[AsyncHost]
       H -->|同步准备| P[请求准备与预算计算]
       P -->|RequestPrepared 或失败| Q[Inbox]
       H -->|启动后台工作| W[Provider 或工具]
       W -->|Host 绑定操作身份| Q
       Q -->|串行处理 Msg| C
       M --> U[UI 只读投影]
   ```

   | 数据或决策 | 唯一归属 |
   | --- | --- |
   | 当前有效操作、队列是否推进、权限和重试选择 | Core 的 Phase 与转换函数 |
   | 对话历史、工具调用结算状态 | Thread |
   | 当前请求配置 | Domain 内经过校验的 RequestPolicy 值 |
   | 后台工作、取消标志、inbox、唤醒与线程资源 | Host / WorkPool |
   | 预算与投影算法 | 现有 request_preparation / context / compaction 纯模块 |
   | 请求、回合、会话统计的更新规则 | UsageState 的纯操作 |
   | 速率时钟、动画与呈现 | UI |

   一次普通请求的阅读顺序固定为：Submit 入队 → Core 创建 PreparingRequest 并发出 PrepareRequest → Host 作废旧句柄并准备完整请求 → Core 校验准备代次及预算 → 提交队首、创建 Streaming 并发出 StartStream → Host 执行并绑定事件身份 → Core 接收增量、结算结果、推进工具或结束回合。

4. **统一操作身份，保留有意义的类型区分。**

   推荐用一个有明确分支的 OperationRef 替换 OperationId 和 OperationIdentity：

   ```cpp
   struct StreamRef {
       TurnId turn_id;
       StreamId stream_id;
   };

   // 沿用现有 ToolCallRef { assistant_message_id, tool_exec_id }。
   using OperationRef = std::variant<StreamRef, ToolCallRef, CompactionId>;
   ```

   StreamRef 同时用于 Streaming、StartStream 及相应的 Core 流事件。工具权限、执行与结果继续使用同一个 ToolCallRef。压缩事件继续使用 CompactionId。需要接受不同种操作的位置，使用 OperationRef 包装这些同一份身份值。

   PreparationId 继续单独表示准备代次：准备当前是同步计算并向 inbox 投递结果，仍可能在结果被处理前收到配置更新或 ClearQueue。它不占用后台操作槽位。

   删除 OperationKind、OperationOwner、OperationIdHash，以及分散的 stream_identity、compaction_identity、compaction_operation_id 等拼装规则。RequestUsage 的 turn_owner 从 StreamRef 推导，压缩没有用户 turn；取消和用量不再使用两套 operation 格式。

   ID 的唯一性范围保持为当前 Model / Host 的线性会话，计数器不复用已发出的 ID。工具的完整身份继续包含 assistant 消息锚点，不能退化为只比较 tool_exec_id。

   Core 拒绝缺失、为零或与活动 Phase 不匹配的异步身份。用量还需匹配请求 fingerprint，并满足“活动请求第一次接受”的条件。OperationRef 中的工具分支不能被当作模型请求用量接受。

   ProviderEvent 是尚未绑定会话身份的原始结果。现有 provider/fake 可以继续生成这些原始结果，由 Host 在进入 Core 前绑定身份。删除的是 Core 对无身份事件的接受分支。此次不增加平行的一套 provider DTO 层。

5. **把 AsyncHost 收缩为消息泵与串行执行器。**

   推荐保留现有 AsyncHost 类和 WorkPool，以一个槽位替换 operations_：

   ```cpp
   struct CurrentOperation {
       OperationRef ref;
       std::shared_ptr<OperationState> state;
   };
   std::optional<CurrentOperation> current_operation_;
   ```

   OperationState 只保存执行侧所需的取消状态。槽位表示最近启动操作的取消入口；它不参与判断 Model 是否活动，也不承诺对应 worker 尚未退出。

   | Cmd | Host 的动作 |
   | --- | --- |
   | PrepareRequest | 先作废并释放旧槽位，再读取动态输入并同步准备；结果投递 inbox |
   | StartStream / StartCompaction / RunTool | 先作废旧槽位，再建立新句柄并启动工作 |
   | CancelOperation | 身份匹配时标记取消并清空槽位；不匹配时忽略 |
   | NoCommand | 保留槽位，无执行动作 |

   后台任务捕获 state、inbox 和所需执行依赖的共享所有权。取消后 Host 可以放弃引用，任务结束时自然释放自己的引用。正常完成后的一个轻量句柄可以保留到下一次替换。

   槽位的创建、替换、取消仅发生在 owner thread。shutdown 使用 WorkPool 已有的共享停止信号，各 worker 和回调已有该信号的检查；shutdown 不再遍历或访问 operation 槽位。核实并保留现有跨线程 shutdown 调用能力后，删除 operations_mutex_。

   process_msg 收敛为 update → 提交新 Model → 执行 Cmd。删除对 StreamFinished、CompactionFinished、ToolExecOutput 的退休分支，以及读取新 Phase 判断工具句柄是否能退休的逻辑。is_quiescent 仍按 Core 的业务状态判断等待条件，含义与后台任务是否全部退出区分开。

   保留两道隔离：Host 过滤取消后的回调，Core 拒绝 inbox 中已经排队的过期事件。先提交新 Phase、使旧身份失效，再执行新命令；先作废旧句柄，再启动新工作。

   Stream 与 Compaction 共用一个私有 provider 执行骨架，集中保留异常转失败事件、正常终态去重、无终态返回时补错和停止信号检查。两条路径各自负责已有的事件翻译规则。这个帮助函数留在 async_host.cpp 内。

   不增加 TaskExited 消息、后台任务登记服务、通用 effect 列表或新的调度层。逻辑取消的保证覆盖会话状态隔离，已经发生的工具副作用和卡住任务的物理回收仍遵循现有行为。

6. **让 Core 的转换携带完整语义。**

   PreparingRequest 改为准备代次加请求意图：

   ```cpp
   struct QueuedTurnRequest { TurnId turn_id; MessageId queued_message_id; };
   struct ToolContinuationRequest { TurnId turn_id; MessageId previous_assistant_id; };
   struct RetryRequest { TurnId turn_id; MessageId target_assistant_id; };
   using RequestIntent = std::variant<
       QueuedTurnRequest, ToolContinuationRequest, RetryRequest>;

   struct PreparingRequest {
       PreparationId preparation_id;
       RequestIntent intent;
   };
   ```

   QueuedTurnRequest 准入后才提交队首。ToolContinuationRequest 的 previous_assistant_id 用于准备失败时记录错误，成功时创建新 assistant 消息。RetryRequest 成功后复用原 assistant 消息。原有分支行为保持，调用者不再传枚举及若干零 ID 占位参数。

   Streaming 保存 StreamRef、target_message_id、ContextProjectionReceipt 和 usage_accepted。fingerprint 从 receipt 读取，删除同一 Phase 中的重复副本。原 RequestPurpose 在 Streaming 中仅用于判断是否允许回滚空的新回合；替换为明确的可选 rollback_user_message 锚点，保留现有“收到 provider 上下文拒绝且尚无输出才回滚”的条件。准备完成后不继续携带整个 RequestIntent。

   Core 中准备请求、接纳请求、推进待执行工具、结束回合和取消操作各有一个完整的转换入口。每个入口集中修改相关 Phase / Thread / queue 并返回对应 Cmd。提取函数的标准是它完成一项明确转换，避免只把一两行赋值换成间接调用。

   start_continuation 当前两次逆向寻找同一 assistant 消息，合并为一次定位。Permission 与 ToolExecOutput 沿用 ToolCallRef 的严格关联和已有工具结算规则。

7. **缩小请求准备接口，关闭独立快照与 Thread 的配对入口。**

   ```cpp
   struct PreparedRequestSnapshot {
       Request request;
       ContextBudgetAssessment budget;
       ContextProjectionReceipt receipt;
   };
   ```

   prepare_request_snapshot 内部补齐动态 system prompt、工具、输出预算，完成投影和准入计算，并生成 receipt。Core 读取上述三个结果；完整 ContextProjection 保留在计算模块及其测试中。

   同一个快照中的 request、budget、receipt 在同一次准备计算中生成，Core 接受该代结果后直接发出相应请求。Host 执行 StartStream 时继续直接使用命令里的完整请求。预算 UI 诊断仍来自已有 assessment；真实上下文用量仍来自 provider observation。

   当前 project_request_with_compaction(Request, Thread) 的唯一生产调用者先从 Thread 构造 Request，再把两者一起传回。将这段构造与已接受压缩的应用收敛到 canonical_request(const Thread&) 内，删除这个可误配的双输入公共接口；压缩候选验证所需的 Request + candidate 帮助函数仍为 compaction.cpp 私有实现。

   队列中的未提交消息在 canonical request 构造后，由 Core 根据 RequestIntent 加入草稿。准备结果与当前会话的关联依靠 PreparationId、当前意图及队首身份校验。CompactionPlan 继续保存启动时的验证输入，不在完成时回读新配置。

8. **集中用量更新，删除能够推导的镜像。**

   将用量值类型和更新规则移入 usage.hpp / usage.cpp。Core 负责校验事件属于当前活动请求，并维护该 Phase 的 usage_accepted；通过校验后交给统一的规范化与累计逻辑。

   | 状态 | 处理方式与原因 |
   | --- | --- |
   | session totals、compaction totals | 保留，两者统计范围不同；压缩计入 session，不计入用户 turn |
   | 当前 turn ID 与 totals | 保留；新回合准入时重置 |
   | latest_request | 保留，表示最近一次普通请求的可用或不可用用量 |
   | observed_context | 保留；最近请求缺少用量时，仍需展示较早的真实观测并标记 Stale |
   | turn.latest_request | 删除；由 latest_request 的 StreamRef.turn_id 是否匹配当前 turn 推导质量，无匹配时为 Unavailable |
   | 两种相同布局的观察结构 | 用一个 UsageObservation 值类型表达，保留两个语义不同的存储位置 |
   | 每个活动请求的 usage_accepted | 留在对应 Phase，防止当前请求重复累计 |

   普通请求与压缩共用 token 字段规范化及 totals 累加实现。UsageState 提供开始回合、记录已接受观测、标记请求缺少用量、标记旧上下文观测等少量有业务含义的纯操作。exact usage 对 projection ledger 的校准继续调用现有纯算法。

   UI 从这些事实投影展示值。保持 Exact / Last / Stale / Unavailable 的已有区别，估算准入值不会成为真实用量。此次不建立无限增长的请求账本或全局去重表。

9. **把配置校验放到值的构造边界。**

   推荐将现有 provider_name、provider_model、ContextManagementConfig 组成一个 RequestPolicy 值，构造时执行现有校验，并只提供只读访问。默认构造保持现有合法默认配置；不增加 provider/model 字符串的新约束。

   Domain 保存一份 RequestPolicy；SetRequestPolicy 携带这个有效值；PrepareRequest 和 CompactionPlan 按需要捕获同一个配置值。构造边界调用现有 validate，继续用现有异常及调用方诊断方式报告非法配置。值在 dispatch / update 之前构造，失败不会先移走 Host 的 Model。

   因此，删除 AsyncHost 为 SetRequestPolicy 单独复制 Model 的特例。Core 处理有效配置的状态转换。准备中更新配置仍生成新的 PreparationId，旧成功和失败结果都被拒绝；Streaming 和 Compacting 已经捕获的配置不被后来更新替换，后续请求读取最新配置。

   这是对现有配置三元组和特殊调用约定的替换。算法级 ContextManagementConfig 的独立校验测试仍然有效；不对含有内存分配的整个 update 标注 noexcept，也不引入通用错误总线。

10. **文件布局与明确删除项。**

    建议最终的核心阅读路径如下；新增文件用于承接已有类型与规则。

    | 文件 | 职责 |
    | --- | --- |
    | runtime/identity.hpp | 强类型 ID、StreamRef / ToolCallRef / OperationRef |
    | runtime/phase.hpp（新增） | Phase 及各阶段独有的数据、请求意图 |
    | runtime/model.hpp | Domain、SessionState、Model 的组合 |
    | runtime/agent.hpp、agent.cpp | Msg → 状态转换 → Cmd 的唯一入口与流程 |
    | runtime/async_host.hpp、async_host.cpp | 消息泵、取消槽位、实际执行与事件绑定 |
    | runtime/request_preparation.hpp / .cpp | 完整请求准备与窄返回值 |
    | runtime/usage.hpp / .cpp（新增） | 从 Model 中迁出的统计值类型与纯更新规则 |
    | runtime/context_types.hpp | 现有预算配置与 RequestPolicy 有效值 |
    | runtime/context.cpp、compaction.cpp | 保留现有估算、压缩规划及验证算法 |

    Model 保留顶层 phase，将 d / s 改为 domain / session；StreamState 改名 SessionState，因为它保存的是队列、ID 序列和会话统计。只做一次消费者迁移，删除旧字段名。stream telemetry 保留在 session：现有 UI 在流结束后还会读取最后一次采样，不能仅因为名字就移进 Streaming 后销毁。

    agent.cpp 先按完整状态转换和事件类别组织；本轮不建立多个共享整个 Model 的子状态机，也不照搬 agentty 的大型 internal.hpp。只有 Usage 独立出纯模块，已有 preparation / context / compaction 继续承担其算法职责。

    | 删除项 | 删除依据 |
    | --- | --- |
    | operations_、operations_mutex_、retire_operation | 单槽位取消入口，无退休协议 |
    | process_msg 中的终态分类与 Phase 回读 | 结果有效性由 Core 决定，Host 不再据此回收注册项 |
    | 所有 supersedes 字段及传递参数 | 新工作统一替换旧句柄 |
    | 零 ID 兼容分支 | Core 只接受明确身份 |
    | OperationId / OperationIdentity 及配套 kind、owner、hash | 一个分支明确的 OperationRef |
    | RequestUsage.turn_owner | 从操作身份派生 |
    | RequestPurpose 与 PreparingRequest 的散装锚点 | 有明确数据的 RequestIntent |
    | Streaming.request_fingerprint 副本 | 从该流 receipt 读取 |
    | turn.latest_request | 从单份 latest_request 与 turn 身份推导 |
    | Model::revision | 当前生产代码只有写入，没有读取；UI 使用自己的 TranscriptRevisionTracker |
    | StartStream.target_message_id | Host 无消费者，目标消息锚点仍保留在 Streaming |
    | CompactionTrigger 及 Phase/Cmd 的 trigger 字段 | 当前生产代码仅保存和传递；手动与自动入口的实际条件仍保留 |
    | project_request_with_compaction(Request, Thread) 公共入口 | 构造 Request 与读取 accepted compaction 使用同一 Thread |
    | SetRequestPolicy 的 Host 复制分支 | 校验在有效配置值构造时完成 |

11. **实施顺序：五个能够独立检查的切片。**

    | 顺序 | 完成内容 | 当步验证重点 |
    | --- | --- | --- |
    | 1. 严格身份校验 | 迁移直接调用 Core 的测试，删除零 ID 兼容，集中校验当前与迟到身份 | 当前结果接受；零身份、错身份、迟到结果拒绝；旧工具报告不结算新工具 |
    | 2. Host 与操作身份收缩 | 同时统一 OperationRef、改为单取消槽位，删除旧 hash、重复 owner、supersedes 与退休逻辑，合并 provider 执行骨架 | 取消后马上开始新工作；已入队旧消息隔离；准备失败后旧任务仍被取消；shutdown 行为 |
    | 3. 请求状态与配置 | RequestIntent、窄快照、RequestPolicy、canonical_request 单输入 | 新回合/续接/重试的成功和失败；配置重发准备；预算与实际请求一致 |
    | 4. 用量归属 | 独立 usage 纯模块，去重复更新及可派生镜像 | 一次累计、压缩统计范围、缺少用量、真实上下文与估算分开 |
    | 5. 阅读路径与清理 | phase/model 布局、命名迁移、删除无消费者字段、更新文档 | 编译所有消费者；UI 展示及交互；旧符号零残留；完整回归 |

    每个切片同步迁移当前调用者和测试，不保留旧接口别名、桥接重载或新旧两套执行路径。OperationRef 与单槽位在同一切片完成，避免先为即将删除的 map 再实现一套临时 hash。顺序用于缩小每次验证的范围，最终交付覆盖五个切片。

    实施前重新记录测试基线。此前观察到的 calculator 工具数断言与 HTTP 测试环境限制要单独核实，不能通过删除失败行为测试掩盖。保留工作区已有修改。

    复用现有 agent_loop、async_host、tool_loop、ts006、context_admission、context_maintenance、compaction、status_bar、screen 和 ui_loop 行为测试。测试中有效异步事件从启动命令取得身份；迟到事件保留原命令的旧身份。测试帮助函数不能将一个待验证的旧事件偷偷改绑到当前操作。

    按切片运行相关测试，最后构建全项目并执行一次完整 CTest。新增测试只补现有测试未覆盖的合同，例如准备失败前已经作废旧句柄、严格拒绝零身份、跨 turn 推导用量质量和非法配置构造不改变 Host 状态。字段重命名或搬移本身通过编译及既有行为测试检查。

12. **最终验收与交付。**

    阅读 AsyncHost::process_msg 可以直接看懂状态推进，不需要了解工具报告的接受条件。阅读取消流程可以直接看到 Core 撤销旧身份、Host 作废旧句柄，沿途不再传递 supersedes。理解请求准备只需知道请求意图、准备代次及三项返回值。一次用量观测的累计规则有一个入口。

    操作登记结构恒定为一个轻量槽位；TaskExited 和 effect batch 不在实施清单中。对话队列准入、动态 prompt 刷新、工具权限、请求重试、压缩失败抑制、取消后的迟到事件和用量显示保持已有合同，严格身份校验按已确认的决定收紧。

    交付代码修改、同步迁移的测试，以及一份与代码相符的简短架构说明和状态流转图。更新现有 MODEL-REFACTORING-GUIDE 与 QUEUE-TURN-STATE-MACHINE-DESIGN 中被替代的合同，明确历史记录和当前设计的区别。最终报告列出实际删除项、验证结果和仍存在的限制。

    五个切片已依次实施并运行相关测试。压缩规划的两个旧入口合并为一个可选 assessment 的入口；另删除经代码搜索确认仅写无读的 compaction_input_tokens 和 projection 诊断字段。shutdown 补上 owner 唤醒，保留跨线程调用。详见当前架构与验证记录。
