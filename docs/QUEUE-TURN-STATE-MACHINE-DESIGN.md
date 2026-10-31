# 消息排队与 Turn Ownership 状态机设计

日期：2026-09-18

状态：**队列与 Turn Ownership 已实现；本文保留 2026-09-18 历史设计，2026-10-09 更新当前合同。**

当前结构以 [Runtime 架构](RUNTIME-ARCHITECTURE.md) 为准。本文后续旧代码示例与原实施顺序用于历史追溯；其中 operation map、supersedes、散装 ID 和布局已被替代。ClearQueue 已实现，不能再将第 13 节的原推迟清单视为当前状态。

本文档记录 `my_agent` 消息排队、turn ownership、异步结果归属和 queued-message UI 的设计依据。现有队列行为沿用本文；类型、执行生命周期与配置合同以当前 Runtime 架构说明为准。

[AGENT-CAPABILITY-ROADMAP.md](AGENT-CAPABILITY-ROADMAP.md) 仍是候选发展方向，不代表其中所有阶段都已承诺实施。本文只冻结已经讨论完成的排队状态机设计；上下文压缩、持久化、checkpoint、slash command 和 RAG 的顺序仍可独立调整。

## 1. 背景与问题

当前 Core 只有一个全局 `Phase` 和一条 `Thread`。`Submit` 无论当前是否正在 streaming，都会直接：

1. 向 `Thread::messages` 追加 user message；
2. 追加空 assistant placeholder；
3. 返回新的 `StartStream`。

与此同时，`StreamTextDelta` 和 `StreamToolCall` 直接修改 `thread.messages.back()`。因此，在旧 stream 尚未结束时再次提交，旧 stream 的后续事件可能写入新创建的 assistant message。

这不是单纯的 UI 串联问题，而是异步事件没有稳定 owner：

- 用户提交、当前 turn 和 provider invocation 没有独立身份；
- queued prompt 与已进入 conversation 的事实没有分离；
- stream/tool 回调只依赖“最后一条 message”或 tool call 字符串 ID；
- `AsyncHost` 的 inbox 是跨线程事件收件箱，不是领域调度队列。

目标是在保持当前 `update(Model, Msg) -> Step` 纯状态机形状的前提下，建立可测试的单 active-turn 排队模型。

## 2. 参考实现：agentty

本设计参考了本地 agentty `89e481d1`（2026-09-11）的实际实现，而不只参考早期设计文档。

agentty 的关键行为是：

- busy 时把 composer payload 放入独立的 `ComposerState::queued`；
- queued item 不进入真实 `Thread::messages`；
- UI 在 conversation 尾部合成 queued preview，并在 composer/status 显示数量；
- 当前完整 turn（包括 tool、permission 和 continuation stream）结束后，才按 FIFO 重新调用统一 submit 路径；
- queue 是进程内的 queued draft，不是持久化任务系统。

本项目采纳：

- 独立 queue；
- 完整 turn 作为调度边界；
- queued preview 只做 UI projection；
- 激活队头时复用统一 submit/activate 路径。

本项目不直接复制：

- queue 不放在纯 UI state 中，而放在 Core `Model`；
- 为 turn、message、stream 和 tool execution 建立明确身份；
- 普通 turn 的取消或局部失败默认继续消费下一条；
- 只有影响整个 session 后续工作的错误才进入阻塞状态；
- 保留单 Msg 输入、单 Cmd 输出，不引入通用 command batch。

## 3. 核心边界

系统中存在两种完全不同的队列：

### 3.1 Runtime inbox

`AsyncHost::InboxState::messages` 是跨线程事件收件箱：

- worker 投递 provider/tool 结果；
- owner thread 按 FIFO drain；
- 消息被 drain 后立即交给 Core；
- 它不表达用户任务的等待状态。

把第二条 `Submit` 留在 inbox 中不能实现排队，因为下一次 drain 仍会立即消费它。

### 3.2 Domain queue

`Model` 需要独立的 queued-message 容器：

```cpp
struct QueuedMessage {
    TurnId turn_id;
    MessageId message_id;
    std::string text;
};

struct Model {
    Phase phase{Idle{}};
    Thread thread{};
    std::deque<QueuedMessage> queued_messages;
    // profile, permission, telemetry, id sequence, ...
};
```

以上代码只规定语义形状，最终命名可以在实现中调整。必须保持的事实是：queued item 有稳定身份和用户 payload，但在激活前不属于 `Thread::messages`。

## 4. 身份与所有权

一次用户提交可能经过多次 provider invocation，因此不能用一个 stream ID 代表整个回合。

### 4.1 身份层级

| 类型 | 语义 |
|---|---|
| `TurnId` | 一次用户提交触发的完整 Agent 回合 |
| `MessageId` | 一条具体 user/assistant message |
| `StreamId` | 一次具体 provider invocation |
| `ToolExecId` | 一次本地工具执行 operation |

典型关系：

```text
QueuedMessage(turn=42, user_message=70)
  → UserMessage(turn=42, message=70)
  → AssistantMessage(turn=42, message=71)
       ← StreamId 100
       → ToolExecId 200
  → AssistantMessage(turn=42, message=72)
       ← StreamId 101
```

同一个 `TurnId` 可包含多个 assistant continuation message 和多个 `StreamId`。

### 4.2 稳定目标

provider delta 不再写 `messages.back()`，而是：

1. 验证事件的 `StreamId` 是当前 active stream；
2. 读取 active stream 保存的 `target_message_id`；
3. 只修改该 assistant message；
4. 非当前 ID 的事件是严格 no-op。

工具结果同样不能只在整个历史中按 provider tool-call 字符串反向搜索。`ToolExecOutput` 应绑定当前 `TurnId` 和本地 `ToolExecId`；provider 的 `ToolCallId` 仍用于协议关联，但不单独承担本地 operation ownership。

### 4.3 确定性 ID

ID 分配必须保持 `update()` 纯和可重复。禁止在 Core 中读取时钟、随机数或进程全局可变计数器。

建议由 `Model` 保存单调序列：

```cpp
struct IdSequence {
    std::uint64_t next_turn{1};
    std::uint64_t next_message{1};
    std::uint64_t next_stream{1};
    std::uint64_t next_tool_exec{1};
};
```

相同的 `Model + Msg` 必须产生相同的 `Step`。

## 5. 状态模型

建议的 phase 语义：

```text
Idle
Streaming
AwaitingPermission
ExecutingTool
Blocked
```

非 Idle 不再等价于 active：`Blocked` 没有正在运行的 turn operation。因此需要区分：

- `has_active_turn()`：是否有正在执行或等待 permission 的 turn；
- `can_start_next()`：是否允许激活队头；
- `is_blocked()`：是否因 session-wide 问题停止调度。

active phase 必须携带足够的 ownership：

```cpp
struct Streaming {
    TurnId turn_id;
    StreamId stream_id;
    MessageId target_message_id;
};

struct AwaitingPermission {
    TurnId turn_id;
    ToolCallId tool_call_id;
};

struct ExecutingTool {
    TurnId turn_id;
    ToolExecId tool_exec_id;
    ToolCallId tool_call_id;
};

struct Blocked {
    BlockReason reason;
};
```

具体代码可以提取共享 active context，但不能重新引入互相可能漂移的平行 owner 字段。

## 6. 单 Msg、单 Cmd 状态机

本设计保留当前核心形状：

```cpp
struct Step {
    Model model;
    Cmd cmd;
};

Step update(Model model, Msg msg);
```

约束是：

> 一次 `update()` 消费一个已经发生的 `Msg`，可以完成零到多次纯内存状态推进，但最多跨越一个外部 effect 边界，因此最多返回一个 `Cmd`。

概念实现：

```cpp
Step update(Model model, Msg msg)
{
    apply_event(model, msg);
    Cmd cmd = drive_until_effect_or_stable(model);
    return {.model = std::move(model), .cmd = std::move(cmd)};
}
```

不为 queue 推进额外制造 `AdvanceQueue` 或 `StartNext` 消息。当前事件导致的确定性后继状态在同一次 `update()` 中完成。

### 6.1 drive 的停止条件

每次内部推进必须至少完成一项：

- 消费一个 queued item；
- 完成一个 active turn；
- 转换到不同的合法 phase；
- 产生唯一外部 `Cmd`；
- 到达稳定等待状态。

不得存在无进展的 `Idle -> Idle` 或循环检查。

### 6.2 Phase 与 Cmd 对应

| Cmd | 返回 Model 必须满足 |
|---|---|
| `StartStream` | `phase` 是相同 `TurnId/StreamId/MessageId` 的 `Streaming` |
| `RunTool` | `phase` 是相同 `TurnId/ToolExecId` 的 `ExecutingTool` |
| `CancelOperation` | 被取消 operation 已在 Model 中逻辑退休 |
| `NoCommand` | Model 已到稳定状态或等待 owner 输入 |

Core 不得返回与 Model 中 active identity 不一致的 command。

### 6.3 取消并继续仍保持单 Cmd

取消当前 operation 且 queue 非空时，`StartStream` 可以显式表达替代关系：

```cpp
struct StartStream {
    TurnId turn_id;
    StreamId stream_id;
    MessageId target_message_id;
    Request request;
    std::optional<OperationId> supersedes;
};
```

- 正常启动：`supersedes = nullopt`；
- 取消当前 turn 并启动下一条：`supersedes = old_operation_id`；
- 取消且 queue 为空：返回单独的 `CancelOperation`。

这是一条固定顺序的语义命令，不是任意 command batch。`AsyncHost` 执行带 `supersedes` 的 `StartStream` 时，先尽力取消/退休旧 operation，再注册并启动新 stream。

是否物理取消成功不影响逻辑正确性：旧 operation 的 ID 已由 Core 退休，其迟到事件全部被忽略。

## 7. 事件与状态推进

### 7.1 Submit

所有提交走同一条路径：

```text
Submit
  → 分配 TurnId + user MessageId
  → 追加到 queued_messages
  → drive
```

若 `can_start_next()`：

1. 原子地从队头移出 item；
2. 保留其 `TurnId/MessageId`，创建正式 user message；
3. 创建新的 assistant target message；
4. 分配 `StreamId`；
5. 进入 `Streaming`；
6. 返回 `StartStream`。

若当前 turn active 或 session blocked，只保留在 queue，返回 `NoCommand`。

### 7.2 Stream delta/tool call

- identity 匹配：更新指定 assistant message；
- identity 不匹配：Model 完全不变，返回 `NoCommand`；
- queued messages 不受影响。

### 7.3 Stream finished

`StreamFinished` 只结束一次 provider invocation，不一定结束整个 turn：

```text
有 pending tool
  → AwaitingPermission 或 ExecutingTool

工具结果尚未送回模型
  → 创建 continuation assistant message
  → 新 StreamId
  → StartStream

没有后续 tool/continuation
  → 完成当前 turn
  → Idle
  → drive 队头
```

queue 的调度边界是完整 user turn，不是一次 provider request。

### 7.4 Tool result

只接受当前 `ToolExecId`：

- 当前 assistant 还有可运行工具：返回下一个 `RunTool`；
- 工具需要 permission：进入 `AwaitingPermission`，返回 `NoCommand`；
- 工具批次完成：创建 continuation stream，返回 `StartStream`；
- 迟到或重复结果：严格 no-op。

### 7.5 Cancel

`CancelActive` 只取消当前 turn，不取消 queue：

1. 立即退休当前 stream/tool operation ID；
2. 保存 partial assistant output；
3. 在对应 assistant message 上记录 `cancelled`；
4. 清理当前 permission 和未终结 tool 状态；
5. 完成当前 turn；
6. queue 非空时以带 `supersedes` 的 `StartStream` 启动下一条；
7. queue 为空时返回 `CancelOperation`。

未来如果需要停止所有工作，应提供独立的 `CancelAll/ClearQueue`，不能让 `CancelActive` 同时承担两种语义。

## 8. 错误与阻塞策略

错误处理策略必须是 typed data，Core 不应临时解析人类可读字符串来决定状态机行为。

```cpp
enum class FailureDisposition {
    RetryCurrent,
    FailAndContinue,
    BlockSession,
};
```

### 8.1 默认策略

| 结果 | 行为 |
|---|---|
| turn 正常完成 | 完成并消费下一条 |
| 用户取消 active turn | 标记取消并消费下一条 |
| 单次 turn 终端失败 | 记录错误并消费下一条 |
| 可重试错误 | 保持同一 `TurnId/MessageId`，分配新 `StreamId` |
| session-wide 阻塞错误 | 进入 `Blocked`，保留 queue |

只有真正使后续模型输出或工作无法继续的错误才阻塞，例如：

- 没有有效模型；
- 认证失效且无法恢复；
- provider 配置无效；
- 整体网络/provider 不可用且重试耗尽；
- 其他已经确认会让后续 queued turn 同样必然失败的问题。

请求特定的失败、内容过滤、单次协议失败或用户取消默认不阻塞后续队列。

普通工具执行失败通常仍属于当前 turn：将失败结果交给模型继续处理。只有工具系统整体不可用且模型无法继续工作时，才考虑 `BlockSession`。

### 8.2 Blocked 解除

外部组件只报告事实，Core 决定是否解除：

| Msg | Core 行为 |
|---|---|
| 有效模型选择完成 | 若阻塞原因是无模型，解除并 drive |
| 认证刷新成功 | 若阻塞原因是认证，解除并 drive |
| provider 配置修复 | 重新验证后解除并 drive |
| 显式 `ResumeQueue` | 重试允许显式恢复的阻塞 |
| 无关 repaint/status 事件 | 不改变 Blocked |

Blocked 期间的新 `Submit` 继续进入 queue，不丢失、不启动。

## 9. AsyncHost 契约

`AsyncHost` 是 effect executor，不是调度器。

它负责：

- 执行唯一 `Cmd`；
- 为 `StartStream` 建立 per-stream cancel handle；
- 捕获 `StreamId/TurnId/target MessageId` 并包装 provider 回调；
- 执行工具并把 `ToolExecId/TurnId` 带回 Core；
- 执行 `supersedes` 的 best-effort 物理取消；
- 把结果投递到 runtime inbox。

它不得：

- 看到 Idle 就自行消费 queue；
- 修改 Core Model；
- 决定错误是重试、继续还是阻塞；
- 把 queued preview 加进 provider request。

### 9.1 Effect 终态

每个未被 supersede 的 operation 必须产生恰好一个终态：

```text
StartStream → StreamFinished | StreamError
RunTool     → ToolExecOutput
```

Host 当前已有“provider 抛异常或静默返回时补 `StreamError`”的边界保证；实现 ID 后必须继续保持。

被 supersede 的 operation 可以继续产生物理回调，但其事件因 ID 过期而对 Core 无效。

## 10. UI projection

queued message 是领域事实，但 queued preview 不是 conversation history。

### 10.1 Dock queue preview

`project_screen()` 不得把 queued item 合成到 transcript turns。它应根据
`model.queued_messages` 在 composer/status 上方的固定 dock 区域生成一个独立的
单行 waiting preview，只展示 FIFO 队头：

```text
queued 1 / 2 · 请继续检查测试失败的原因…
> 输入内容
● Thinking
```

要求：

- preview 不写入 `Thread::messages`；
- preview 不进入 provider request；
- preview 使用 queued item 的稳定 `TurnId/MessageId`；
- preview 不进入 transcript、滚动几何或 scrollback；
- 只显示队头且最多占用一行，文本超出可用宽度时尾部截断；
- 使用明确 `queued 1 / M` 标记和较弱视觉强调，不能伪装成已经发送；
- 激活队头时，dock preview 与正式 user turn 在同一次 Model 更新中切换；
- 队头激活后不能同时保留旧 preview，不能造成重复显示；
- error/cancel 属于旧 assistant message，不能只放在会被下一 turn 覆盖的全局 status 中。

### 10.2 Composer/status

首版至少显示：

- active 时：`type to queue…`；
- queue 非空：`N queued`；
- Blocked：`N queued · waiting` 和阻塞原因；
- 极小终端下可以隐藏 preview 文本或元数据，但不能隐藏“仍有 queued work”的事实。

### 10.3 Scrollback 契约

必须保持已验收的 fullscreen 行为：

- frozen 时新增 queued preview 不强制跳到底部；
- guidance 能区分 new output；
- preview 激活为正式 turn 时不重复显示、不产生跳动；
- 多条 queue 的增加/移除不破坏 viewport offset；
- scroll 状态继续只属于 Painter/viewport，不进入 Model。

permission 首版可以继续保持 modal；Core 仍应正确处理其他入口在 permission 期间提交的消息。是否让 composer 在 permission UI 下继续编辑，留待独立设计。

## 11. 不变量清单

实现和测试必须共同守住：

1. 同一时间最多一个 active turn operation；
2. queued item 在激活前不属于 `Thread::messages`；
3. queued item 不进入 provider request；
4. provider delta 只按 `StreamId + target MessageId` 定位；
5. tool result 只按当前 `ToolExecId` 定位；
6. stale/duplicate event 是 Model 全快照级 no-op；
7. queue 正常按 FIFO 消费；
8. tool/permission/continuation 未结束时不得消费下一条；
9. 正常完成、取消、turn-local 失败默认继续 queue；
10. 只有 `BlockSession` 停止 queue；
11. 一次 `update()` 最多返回一个外部 Cmd；
12. `Cmd` identity 必须与返回 Model 的 phase identity 一致；
13. Core ID 分配确定且可重复；
14. 每个未退休 effect 恰好产生一个终态事件；
15. UI queue projection 不改变 conversation 和 request 的事实内容。

## 12. 首要任务：把 Core 改造成可排队状态机

首要任务不是先做完整 queued-message UI，而是修复 ownership，并用测试钉住单 active-turn 行为。

### Q0：当前缺陷的 characterization tests

- A stream 阻塞期间提交 B、C；
- 断言只启动 A 的 stream；
- B、C 不进入 `Thread::messages`；
- A 的 delta 只能进入 A 的 assistant target；
- provider request 不含 B、C。

这些测试在旧实现上应失败，作为改造的直接证据。

### Q1：ID、domain queue 与纯 Core 推进

- 引入强类型 ID 和 Model-owned deterministic sequence；
- 引入 `queued_messages`；
- Submit 统一先入队，再由 drive 激活；
- 为 phase 增加 ownership；
- 完成无工具文本 turn 的 FIFO 自动推进；
- 保持 `Step` 单 Cmd。

### Q2：AsyncHost stream ownership

- `StartStream` 携带完整 identity；
- Host callback 自动带回 identity；
- stale delta/finished/error 全快照 no-op；
- 重复终态不得结束新 stream；
- 每次 invocation 继续保持恰好一个终态事件。

### Q3：tool、permission、error、cancel 与 Blocked

- `ToolExecId` 和 tool result ownership；
- tool continuation 期间 queue 不前进；
- permission approve/reject 保持原 turn；
- `RetryCurrent/FailAndContinue/BlockSession`；
- cancel 当前 turn 后默认启动下一条；
- supersede 的旧 operation 迟到结果不影响新 turn；
- Blocked 恢复后消费队头。

### Q4：queued-message UI

- transcript synthetic previews；
- composer/status queue count；
- stable preview identity；
- normal/compact/permission/very-small-terminal 投影；
- frozen/follow-tail/new-output guidance 回归。

### Q5：集成验收

- streaming 时连续提交多条；
- tool call 和 continuation 中提交；
- turn-local error 后自动执行下一条；
- cancel 后自动执行下一条；
- session-wide block 保留全部队列；
- 恢复条件满足后从 FIFO 队头继续；
- 真实终端验证 preview、queue count、滚动与 resize。

## 13. 明确推迟

首版不因以下能力扩大范围：

- queue 持久化和进程重启恢复；
- 单项编辑、重排和删除；
- `CancelAll/ClearQueue`；
- queued item 独立重试策略；
- slash command 的排队展开时机；
- attachment/paste 的排队 payload；
- permission 期间并行编辑 composer；
- 可配置 queue 容量。

即使未来增加容量限制，也不得静默丢弃。拒绝入队时必须保留 composer payload 并显示可观察错误。

## 14. 后续讨论方向

以下方向以本状态机为基础，但尚未形成实施承诺：

1. queued item 的编辑、召回、删除和 `CancelAll` UX；
2. queue/history 的持久化边界及 schema version；
3. crash 后 active turn、superseded operation 和 queued item 的恢复语义；
4. context builder、token budget 与 compaction 如何占用或阻塞 scheduler；
5. slash command 是入队前展开还是激活时展开；
6. attachment、paste、RAG context 的 payload snapshot；
7. checkpoint 与 turn/queue identity 的关联；
8. provider retry/backoff 的 typed error taxonomy；
9. permission modal 与可编辑 composer 的焦点设计；
10. 是否需要持久化或跨 session 唯一 ID。

这些讨论不得破坏本文的单 owner、单 Cmd、ID-addressed event 和 queued-not-transcript 基线；如确有必要，应先修改本文。

## 15. 代码索引

### 当前 my_agent 入口（2026-10-09）

- [`include/my_agent/runtime/model.hpp`](../include/my_agent/runtime/model.hpp)：`Model::domain` / `Model::session`、queue、ID 与 stream telemetry；阶段值另见 `runtime/phase.hpp`。
- [`include/my_agent/runtime/msg.hpp`](../include/my_agent/runtime/msg.hpp)：`Submit` 与带明确身份的 stream/tool/permission events；SetRequestPolicy 携带已校验值。
- [`include/my_agent/runtime/agent.hpp`](../include/my_agent/runtime/agent.hpp)：`Cmd`、`Step` 和 `update()` 公共契约；必须保持单 Cmd。
- [`src/runtime/agent.cpp`](../src/runtime/agent.cpp)：当前 submit、delta、tool kick 和 permission 状态推进；核心改造位置。
- [`include/my_agent/runtime/async_host.hpp`](../include/my_agent/runtime/async_host.hpp)：effect executor 公共边界。
- [`src/runtime/async_host.cpp`](../src/runtime/async_host.cpp)：inbox、单取消槽位、执行快照、身份绑定与共用 provider 终态处理；无 supersedes 或退休协议。
- [`src/ui/screen.cpp`](../src/ui/screen.cpp)：`Model + UiState -> ScreenConfig`；queued preview/count 的 projection 入口。
- [`src/ui/ui_loop.cpp`](../src/ui/ui_loop.cpp)：Submit、permission keyboard ownership 和 repaint 调度。
- [`tests/agent_loop_test.cpp`](../tests/agent_loop_test.cpp)：纯 Core 状态机行为测试入口。
- [`tests/async_host_test.cpp`](../tests/async_host_test.cpp)：worker/inbox/终态/stale-event 集成测试入口。
- [`tests/screen_test.cpp`](../tests/screen_test.cpp)：queued preview 与 dock projection 测试入口。
- [`tests/ui_loop_test.cpp`](../tests/ui_loop_test.cpp)：真实事件循环、streaming 输入和终端行为测试入口。

### agentty 参考

- [`agentty/include/agentty/runtime/model.hpp`](../../agentty/include/agentty/runtime/model.hpp)：`ComposerState::QueuedMessage`、queue recall/peek 所需状态。
- [`agentty/src/runtime/app/update/submit.cpp`](../../agentty/src/runtime/app/update/submit.cpp)：busy 入队与统一 submit 路径。
- [`agentty/src/runtime/app/update/stream.cpp`](../../agentty/src/runtime/app/update/stream.cpp)：完整 turn 结束后的 queue drain、error/cancel/compaction 分支。
- [`agentty/src/runtime/view/thread/conversation.cpp`](../../agentty/src/runtime/view/thread/conversation.cpp)：synthetic queued-message preview。
- [`agentty/src/runtime/app/update/composer.cpp`](../../agentty/src/runtime/app/update/composer.cpp)：queue recall、逐项查看和删除 UX 的后续参考。
- [`agentty/src/runtime/app/subscribe.cpp`](../../agentty/src/runtime/app/subscribe.cpp)：composer 与 permission modal 的键盘 ownership。
- [`agentty/src/runtime/app/cmd_factory.cpp`](../../agentty/src/runtime/app/cmd_factory.cpp)：per-stream cancel token 和取消后事件抑制。
- [`agentty/docs/website/keybindings.md`](../../agentty/docs/website/keybindings.md)：用户可见的 queue 编辑行为说明。
- [`agentty/docs/agent_panel/04_architecture.md`](../../agentty/docs/agent_panel/04_architecture.md)：早期 queue/turn 设计背景；应以当前代码行为为准。


## 16. 当前队列与取消合同（2026-10-09）

- Submit 总是进入 session 队列。drive 只在 Idle 取 FIFO 队首，创建 PreparingRequest；当前准备携带 PreparationId 和 QueuedTurnRequest。
- 只有匹配当前代次及队首、且准入通过的结果才提交 user/assistant。工具续接与重试用独立 RequestIntent 分支；工具批次完全结算后才结束完整 turn 或续接 stream。
- StreamRef 同时标识 turn 与 stream，ToolCallRef 保留 assistant 消息锚点；取消/用量统一使用 OperationRef。Core 不接受零 ID 的异步结果，Host 为原始 provider 事件绑定身份。
- 取消先使旧 Phase 身份失效，再用一个 Cmd 推进后续准备或取消匹配句柄。Host 在 PrepareRequest 以及任何新工作开始前作废旧槽位；没有 supersedes、operation map 或终态退休。
- ClearQueue 只丢弃未提交草稿。沿用已有边界：queue 非空时取消 queued/retry preparation 和当前 compaction；不依赖已删除的 trigger 来区分手动压缩。活动 stream/tool 和已提交历史不因清空队列而重写。
- 正在准备时更新有效配置会重发代次，旧成功和失败结果均无效。Streaming / Compacting 的执行输入保持启动快照；后续请求使用新配置。
- 用量的当前 turn 质量从最近普通请求的 StreamRef.turn_id 推导。压缩不计入用户 turn；估算准入不替代真实上下文观测。

详细数据归属、状态流转、已删除接口和验证结果见 [Runtime 架构](RUNTIME-ARCHITECTURE.md) 与 [验证记录](RUNTIME-REFACTOR-VALIDATION.md)。
