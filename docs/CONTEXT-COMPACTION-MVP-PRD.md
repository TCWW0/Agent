# 上下文预算与事务式滚动压缩 MVP

- 日期：2026-09-18
- 状态：**CTX-01–CTX-06 已有本地实现，当前进入验收与问题跟踪**
- 范围：能力路线图 P2；包含 M0「Token Usage Foundation」与 M1「Transactional Rolling Compaction」
- 远端状态：**不创建 GitHub PRD 或 issue**

本文把当前讨论收敛为一份可实施的 PRD，并在同一文件中给出本地 issue 草案。它承接
[AGENT-CAPABILITY-ROADMAP.md](AGENT-CAPABILITY-ROADMAP.md) 的「上下文管理与压缩策略」，并以
[QUEUE-TURN-STATE-MACHINE-DESIGN.md](QUEUE-TURN-STATE-MACHINE-DESIGN.md) 已落地的 queue、turn ownership
和异步 identity 为前置基础。

当前代码已经具备单 active turn、FIFO `queued_messages`、`TurnId` / `MessageId` / `StreamId` /
`ToolExecId` 和由 `AsyncHost` 执行 effect 的纯 Core 状态机。围绕本 PRD 的本地实现目前包括：

- provider usage 已归一化为领域模型，并按 request、turn、session 与 context occupancy 分开记录；
- request preparation、保守的 context projection、fingerprint 和 context admission 已接入；
- CTX-04–CTX-06 的首次压缩、失败/取消回滚、异步 identity 防护和 rolling compaction 已接入；
- bracketed paste 已通过 chip/占位符进入 composer，完整内容只在提交时展开，不再按换行拆成多条消息；
- 超过 hard input limit 的 queue head 会保留在队列中并进入 `Blocked(ContextLimit)`，状态栏会显示 projection、hard limit 及配置提示。

当前仍有待验收的问题记录在“已知限制”和“后续工作”中；本文件不把这些问题视为本轮实现范围内的已完成项。

---

## Problem Statement

随着会话增长，用户目前无法可靠知道模型下一次请求实际会占用多少上下文，也无法区分：

- 最近一次 provider request 的输入/输出 token；
- 一个包含多次工具 continuation 的完整 turn 的累计计费用量；
- 当前会话的累计用量；
- 下一次请求真正会占用的 context window。

这几个数字在工具循环中并不相等。把每次 continuation 重复发送的 input token 简单相加，会把计费用量误当成当前上下文占用；只显示 Ollama 最近一次 `prompt_eval_count`，又无法覆盖 OpenAI、Anthropic、排队中的下一次请求或压缩后的 projection。

当下一次请求接近模型上限时，本地 preparation 会先根据完整 wire projection 执行 admission；超过 hard input limit 的请求不会启动 provider，而是保留 queue 并进入可观察的 `Blocked(ContextLimit)`。压缩只改变 provider wire context，不删除或原地改写用户可见的 `Thread`，因此不会破坏 transcript、tool call/result 关系以及未来 persistence/checkpoint 所需要的事实源。

因此 MVP 需要同时解决两个问题：

1. 建立可信、含义明确且不过度调用额外 API 的 token usage 与 context budget 基础；
2. 在完整 turn 边界执行可回滚压缩，只改变 provider wire context，不改变 transcript 事实。

这里的“保证安全”指：应用在本地准入阶段不会发送一个按已知限制与保守 projection 判断必然超限的请求。它不承诺消除 provider 动态配置变化、服务端 tokenizer 差异或未公开限制导致的所有拒绝；此类拒绝仍必须走现有可观察错误路径。

## Solution

MVP 分两步交付，但共享同一套 context projection 和预算语义。

### M0：Token Usage Foundation

所有 provider 将终态 usage 归一化成带通用 provider-operation identity 和 request fingerprint 的领域事件。普通回答 operation 由 `TurnId + StreamId` 拥有；compaction operation 由 `CompactionId` 拥有，不能伪造用户 turn。Core 按 request、turn 和 session 三个层级记账，并把“下一次请求预计输入量”作为独立的 context budget snapshot。

默认策略不在每个 turn 前调用额外 token-count API：

- 正常路径以 provider 已返回的真实 usage 作为锚点；
- 对下一次完整 wire request 做本地、保守的 projection；
- system prompt、tools、模型或 projection 变化时重新构建 projection，不沿用失效增量；
- provider 没有 usage，或请求被取消/中断时，明确标记为 estimate，不能伪装成 exact；
- provider-specific preflight count 是后续可插拔能力，不是本 MVP 的正确性前提。

预算公式为：

```text
effective_context_limit
  = min(configured_limit, discovered_provider_limit when available)

hard_input_limit
  = effective_context_limit - output_reserve - safety_margin

compact_trigger
  = hard_input_limit - compaction_headroom
```

若 provider/model limit 无法可靠查询，则显式配置是本地事实源；发现到的 provider limit 只能收紧，不能放宽用户配置。所有 reserve、margin 和 headroom 必须在启动时验证，不能形成负预算。

### M1：Transactional Rolling Compaction

Core 保留完整、用户可见、不可因压缩而改写的 transcript；新的 context projection 模块负责把它投影为 provider request：

```text
system prompt + tools
  + accepted continuation summary（若存在）
  + summary 覆盖边界之后的原始消息
```

当 Core 处于 `Idle`、准备激活 queue head，或一个完整 turn 刚结束且 projection 达到软阈值时，compaction planner 在完整 `TurnId` 边界选择旧前缀。它保留最近原始 turn，把“全部待覆盖源内容”交给无 tools 的独立 compaction operation，并只在校验通过后原子替换旧的 accepted compaction record。

本 MVP 的最终产品契约是：

> 基于真实 provider usage 和保守本地 projection，在完整 turn 边界执行可回滚的滚动压缩；以不可变 transcript 为事实源，用已验收摘要替换 provider wire 前缀，并保证 queue、tool 和异步 identity 不被破坏。

### 有效压缩的 MVP 定义

一次 compaction 只有同时满足以下条件才可提交：

1. **源输入完整**：规划边界内的每条消息都进入 compaction input；不允许先静默裁剪再声称覆盖整个边界；
2. **边界有效**：只覆盖已经完成的完整 turn，不拆开 tool call/result，不包含 active turn 或 queued prompt；
3. **身份一致**：返回事件属于当前 `CompactionId`，source digest 与计划时一致；
4. **结构有效**：输出可以解析为固定版本的 `ContinuationSummary`，必填字段存在且不为空；
5. **确有净节省**：summary 加 raw tail 的预计 token 比压缩前少，并达到配置的 minimum savings；
6. **后续可发送**：提交后的下一次 projection 不超过 `hard_input_limit`；
7. **原子可回滚**：校验前只写 pending buffer；失败、取消或迟到事件不能修改当前 accepted record。

这些门槛可以验证结构、安全性和输入覆盖，但不能证明模型在语义上没有遗漏。语义差分评测、事实 oracle 和 critic model 属于后续质量迭代，不伪装成 MVP 已解决的问题。

## User Stories

1. **US-01** — 作为长会话用户，我希望看到当前 context occupancy，而不是累计计费用量，以便判断会话还能继续多久。
2. **US-02** — 作为使用工具循环的用户，我希望多次 continuation 的重复 input 被正确计入 turn cost，但不会被错误叠加为 context occupancy。
3. **US-03** — 作为用户，我希望看到当前 turn 的 input/output 用量，以便理解一次任务的实际模型调用成本。
4. **US-04** — 作为用户，我希望 exact 与 estimated token 数有可见差异，以便不会把估算值误认为 provider 权威结果。
5. **US-05** — 作为用户，我希望 OpenAI、Anthropic 和 Ollama 的 usage 在产品中具有一致含义，以便切换 provider 后仍能理解状态栏。
6. **US-06** — 作为用户，我希望 cached input 不被重复加到总 input 上，以便统计结果不会虚高。
7. **US-07** — 作为用户，我希望应用为模型输出预留空间，以便输入没有占满整个 context window 而导致模型无法回答。
8. **US-08** — 作为本地模型用户，我希望显式 context 配置同时参与 Ollama 请求和本地预算，以便状态栏与实际运行窗口一致。
9. **US-09** — 作为用户，我希望接近软阈值时自动整理上下文，以便不需要手工监控每一个 turn。
10. **US-10** — 作为用户，我希望压缩期间继续提交 prompt，并让它们按 FIFO 留在 queue，以便输入不会丢失或污染压缩源。
11. **US-11** — 作为用户，我希望 compaction 不出现在 transcript 中，以便看到的仍是我与 Agent 实际发生的完整对话。
12. **US-12** — 作为用户，我希望最近若干完整 turn 保留为原文，以便当前任务的局部细节不完全依赖摘要。
13. **US-13** — 作为用户，我希望 tool call、permission 结果和 tool output 不会被从所属 turn 中拆开，以便后续模型仍能理解工具链路。
14. **US-14** — 作为用户，我希望 active turn 永远不被压缩，以便 streaming、permission 和 tool continuation 不被中途改写上下文。
15. **US-15** — 作为用户，我希望压缩成功后 queued prompt 自动继续执行，以便 compaction 只是一次短暂的内部准备阶段。
16. **US-16** — 作为用户，我希望压缩失败时仍保留原始 transcript 和上一份可用摘要，以便失败不会破坏会话。
17. **US-17** — 作为用户，我希望无净节省或压缩后仍超限的摘要被拒绝，以便“已压缩”一定代表上下文确实变小且可继续。
18. **US-18** — 作为用户，我希望在没有安全压缩方案时看到明确的 context-limit blocked 状态，以便系统不会冒险发送必然失败的请求。
19. **US-19** — 作为用户，我希望 cancel compaction 后系统能安全继续或明确阻塞，以便不会留下半份摘要。
20. **US-20** — 作为用户，我希望长会话可以多次滚动压缩，以便一次摘要不会成为会话寿命的终点。
21. **US-21** — 作为用户，我希望后续压缩基于上一份已验收摘要和新增旧 turn，而不是不断把已覆盖原文重复塞回请求，以便容量持续受控。
22. **US-22** — 作为用户，我希望 compaction 只使用当前 provider/model 且禁止调用 tools，以便整理上下文不会产生新的外部副作用。
23. **US-23** — 作为用户，我希望状态栏在压缩时显示简洁的 `compacting context` 和 queued 数量，以便知道系统没有卡死。
24. **US-24** — 作为用户，我希望 compaction 的正文和中间 delta 不出现在 UI 中，以便内部维护流量不会冒充 Agent 回复。
25. **US-25** — 作为用户，我希望旧的、重复的或乱序的 usage/compaction 事件被忽略，以便异步竞态不会污染统计或上下文。
26. **US-26** — 作为维护者，我希望相同 `Model + Msg` 产生确定的后继状态，以便预算和 compaction 可以用纯状态机测试覆盖。
27. **US-27** — 作为维护者，我希望 projection、planner 和 validator 是可独立测试的深模块，以便 provider、Core 和 UI 不共享易漂移的隐式规则。
28. **US-28** — 作为维护者，我希望普通测试不依赖真实网络或模型，以便 CI 稳定且失败可复现。
29. **US-29** — 作为维护者，我希望同一 source boundary 的失败不会触发无限自动重试，以便 queue 不会陷入 compaction loop。
30. **US-30** — 作为维护者，我希望所有预算参数由显式配置和验证后的默认值控制，以便未来调优不需要改变状态机语义。

## Implementation Decisions

### 1. 术语与计量边界

MVP 固定四种不同概念，禁止复用同一个 `used_tokens` 字段表达多个含义：

| 概念 | 含义 | 聚合规则 |
|---|---|---|
| Request usage | 一次具体 provider invocation 的实际 input/output | 由通用 operation identity 归属，只接收一次终态值 |
| Turn usage | 同一 `TurnId` 内普通回答 invocation 的计费用量 | input/output 分别求和；另保留 latest request；不混入 compaction cost |
| Session usage | 当前进程会话内已完成 request 的累计计费用量 | 可按 normal/compaction 分类；不用于 context admission |
| Context occupancy | 下一次完整 provider request 的预计 input | 由 projection 计算，不能由 turn/session usage 求和得到 |

`RequestUsage` 领域事件至少携带 typed operation identity、可选 turn owner、总 input、output、cache read、cache creation、exact/estimated 标记和 request fingerprint。不同 provider 的归一化规则为：

- OpenAI：Chat Completions 的 `prompt_tokens`（未来 Responses 对应 `input_tokens`）已是总 input；cached tokens 是其子集，不能再次相加；
- Anthropic：总 input 为普通 input、cache read input 与 cache creation input 之和；output 使用终态累计值；
- Ollama：`prompt_eval_count` 已是总 input；cached prompt count 若存在也是子集；`eval_count` 是 output。

usage 必须在对应 operation 的 finished event 之前进入 inbox。usage 只更新计量状态，不推进 turn；只有终态事件可以触发 tool、continuation、compaction commit 或下一个 queued turn。

### 2. Provider usage 适配

- Ollama 终态 frame 不再只写 UI side channel；它产生统一 usage 事件，再产生 terminal event。现有吞吐率统计可以继续保留，但 token 事实进入 Core。
- OpenAI Chat Completions 请求启用 streaming usage。decoder 必须允许 `finish_reason` 先结束内容、usage-only chunk 后到达，最终仍保证 usage-before-terminal 和恰好一个 terminal event。
- Anthropic 在 `message_start` 收集 input/cache usage，在 `message_delta` 收集累计 output usage，在 `message_stop` 前发出统一 usage。
- usage 缺失不是协议死锁：正常 terminal 仍结束 stream，Core 以 estimate 标记该 request。
- provider decoder 不自行发明 turn ownership；`AsyncHost` 像现有 stream delta 一样为事件补齐 typed operation identity 与 prepared-request fingerprint。

### 3. Request preparation 与 context budget

当前 `AsyncHost` 会在 Core 已经产出 `StartStream` 后才调用动态 system-prompt provider。预算若只检查 Core 当时的 `Request`，会漏掉 memory、skills 和环境信息；而这些内容可能在一次 tool call 后发生变化。因此 MVP 必须建立“先准备完整请求、再准入、最后联网”的 seam，不能用固定常量假装 system prompt 不变。

请求路径调整为：

1. Core 为 queued head 或 tool continuation 生成 request draft，进入带稳定 `PreparationId` 的 `PreparingRequest`，但尚不把 queued head 移入 transcript，也不创建 assistant target；
2. `AsyncHost` 快照当前 system prompt 与 provider/model 配置，由 provider adapter 规范化完整 wire 输入、fingerprint 和预算测量，再投递 `RequestPrepared`；这个步骤不发起模型网络请求；
3. Core 只接受当前 preparation identity，随后决定直接 `StartStream`、先 `StartCompaction` 或安全阻塞；
4. 只有 admission 通过后才提交 queued user message、创建 assistant target 并启动 provider；tool continuation 也必须走相同 preparation seam。

`RequestPrepared` 把所有外部读取结果作为值交回 Core，因此 `update(Model, Msg) -> Step` 仍然是确定性的。stale preparation 与其他 stale async event 一样是严格 no-op。

`ContextBudget` 是独立深模块。它的输入是 prepared full-wire projection、有效 context limit、output reserve、安全余量、最近 exact anchor 与 estimator；输出是不可变 snapshot：

- observed request input/output；
- projected next input；
- exact 或 estimated；
- soft trigger、hard input limit 和剩余 headroom；
- request fingerprint。

本地 admission 使用保守值，而不是为了显示更平滑而低估。request fingerprint 覆盖模型、system prompt、tools 和 messages 的规范化 wire 输入；模型、system prompt、tools、accepted compaction 或 provider 编码方式变化后，旧 anchor 不可直接复用。

MVP estimator 使用两条路径并取更保守值：一条从最近 fingerprint-compatible 的 exact usage 锚点计算变化，另一条从当前 canonical wire 重新估算。hard admission 的 full-wire 估算不得小于“受支持文本/JSON 的 UTF-8 byte count 按一 byte 一 token 计入，再加 provider/message/tool framing reserve”的保守 ceiling。这个 ceiling 可能提前触发压缩，但不能为了提高利用率而在 hard gate 中采用更乐观的平均 bytes-per-token。

provider-specific exact preflight count 可以在未来作为 `ContextBudget` 的能力输入加入，但本 MVP 不要求每 turn 调用，也不让 Core 依赖 HTTP。

### 4. Context limit 与配置

- 显式配置是始终可用的上限；provider/model metadata 可用时只取更小值。
- Ollama runnable path 应显式设置与本地预算一致的 `num_ctx`，避免只改 UI 数字而实际分配窗口仍由服务端默认值决定。
- `output_reserve` 必须与 provider 的最大输出配置一致或更保守；Anthropic 当前必填的 output limit 不能与预算各写一套。
- `safety_margin` 吸收 tokenizer、消息 framing 和服务端实现差异。
- `compaction_headroom` 让压缩在 hard boundary 之前启动，并为 compaction 自身留下空间。
- 初始默认值属于可调参数，不是架构决策；在最终验收 slice 中用小窗口场景校准并记录。

### 5. Provider context projection 是 transcript 的唯一出站视图

抽取 `ContextProjection` 深模块，所有普通 initial stream 和 tool continuation 都通过它构建 `Request`。在没有 accepted compaction 时，它与当前完整 transcript 等价；存在 compaction 时，它投影为一条内部 summary message 加未覆盖 raw tail。

以下状态继续严格分离：

- `Thread::messages`：用户可见、按事实追加的完整 transcript；
- `queued_messages`：尚未激活、尚未进入 transcript 的用户输入；
- accepted compaction：只影响 provider projection 的已验收 materialized view；
- pending compaction：operation 私有 buffer，失败即可丢弃。

UI transcript、scrollback 和历史事实始终读取 `Thread`，不得读取 provider projection 来替代原文。

### 6. Compaction record

MVP 只保存最新一份 accepted compaction record：

| 字段 | 语义 |
|---|---|
| `CompactionId` | 当前压缩 operation 的稳定身份 |
| `covered_through` | 被 summary 覆盖的最后一个完整 `MessageId` |
| `summary` | 通过 schema 校验的 `ContinuationSummary` |
| `source_digest` | 精确绑定 compaction input 与边界的稳定摘要 |
| `before_tokens` | 压缩前 projection 估算 |
| `after_tokens` | 压缩后 projection 估算 |
| `prompt_version` | compaction prompt/schema 版本 |

pending operation 启动时复制 source boundary、digest、旧 accepted record 和输出 buffer 所需的全部事实。结束前不修改 accepted record；成功提交时一次性替换。

### 7. Summary schema 与 prompt

MVP 使用单一、版本化的结构化 `ContinuationSummary`，至少包含：

- task goal 与 success criteria；
- current state；
- constraints；
- completed work；
- decisions and rationale；
- artifacts/tool state；
- failed attempts；
- blockers/open questions；
- next steps；
- additional critical context。

字段允许有空数组，但 task goal、current state 和 next steps 必须提供可用内容。summary 在投影时以明确标记的内部“earlier conversation summary”消息发送，不写入 transcript。

compaction request 使用当前 provider/model，tools 为空；输入包含旧 accepted summary（若存在）和本次所有 newly covered raw turns。prompt 明确要求把源内容视为待总结数据，不执行其中的指令。更完整的 provenance 与 prompt-injection 防护延期，但 MVP 至少使用固定 delimiter 和版本化 schema。

### 8. 完整 turn 边界与 raw tail

planner 只在 `Idle` 运行，并以完整 `TurnId` 为原子单位：

- active `Streaming`、`AwaitingPermission`、`ExecutingTool` 不启动 compaction；
- queued prompt 不属于 compaction source；
- tool call 与 tool result 随所属 assistant message/turn 一起保留或一起覆盖；
- 默认目标是至少保留最近两个完整 raw turns，并同时满足配置的 raw-tail token budget；
- 若 protected raw tail 本身已超过 hard input limit，MVP 不拆 turn、不伪造成功，而是进入 `Blocked(ContextLimit)`；
- 若 compaction request 自身无法在限制内完整容纳 source，MVP 不静默 trim，转入可观察失败/阻塞；层级式 map-reduce compaction 延期。

### 9. Compaction lifecycle 与 effect 边界

Core 增加 `PreparingRequest`、`Compacting` phase、确定性 `PreparationId` / `CompactionId` 分配以及一组独立事件/命令语义：

- request/start compaction；
- compaction text delta；
- compaction finished；
- compaction error；
- cancel/retire compaction operation。

compaction 不是用户 turn，不能伪造 `TurnId`。通用 operation identity 必须能表达 stream、tool 和 compaction 各自的 typed owner；compaction usage 计入 request/session cost，但不计入任何 user turn。`AsyncHost` 继续只拥有后台 operation，不在 worker 线程修改 `Model`。

`drive` 的稳定顺序为：

1. `Idle` 中先为下一次 request 创建 preparation；
2. 收到完整 prepared request 后检查 projection；
3. 达到 trigger 且存在可压缩完整前缀时，进入 `Compacting` 并返回唯一 provider effect；
4. compaction 成功提交后重新 prepare/projection；
5. 若低于 hard limit，激活 queue head；若仍不安全，进入 `Blocked(ContextLimit)`；
6. 若无需压缩，沿用现有 queue activation 路径。

queue head 必须在 budget/compaction 决策之后才移入 transcript。这样压缩源只包含已经提交的 turn，新 prompt 在整个 compaction 期间保持 queued。

### 10. Tool continuation 的特殊规则

tool continuation 属于尚未完成的 active turn，MVP 不在其间压缩。每次 continuation 仍必须通过 projection 和 hard admission：

- projection 安全时继续现有 tool loop；
- projection 不安全时，不发送 provider request，把当前 turn 以可观察错误结束并进入 `Blocked(ContextLimit)`；
- 后续 queue 保留，不得绕过 blocked 状态自动启动；
- compaction 不得改变已经发生的 tool side effect，也不声称能回滚外部世界。

软阈值、output reserve 和 headroom 的意义之一，就是降低 active turn 在 continuation 中才首次撞上 hard boundary 的概率。

### 11. 失败、取消与重试语义

- invalid schema、digest mismatch、boundary drift、empty output、超 summary cap、无 minimum savings 或 after projection 超限都视为未提交；
- provider error 与用户 cancel 丢弃 pending buffer，保留旧 accepted record 与完整 transcript；
- 失败后若待发送 request 仍低于 hard limit，可以继续 queue；否则进入 `Blocked(ContextLimit)`；
- 同一 source digest 在没有新增已完成 turn、配置变化或显式用户动作时，不自动重复 compaction；
- stale/duplicate compaction event 与现有 stale stream/tool event 一样是严格 no-op；
- compaction cancel 可以通过现有单 `Cmd` 替代语义退休旧 operation，但不得引入任意 command batch。

### 12. UI 契约

状态栏把 occupancy 与 billing 分开，目标信息形态为：

```text
ctx ~42k/128k · turn in 61k · out 3k
```

- `~` 表示 estimate；exact 不带该标记；
- `ctx` 表示 projected next input，不是 session 累加值；
- turn input/output 是当前完整 turn 内多次 request 的累计值；
- `Compacting` 显示 `● compacting context`；
- queue preview 与 queued count 延续现有 UI 契约；
- compaction delta、summary 正文和 validation failure detail 不进入 transcript；
- 成功可以显示短暂非 transcript 状态，如 `context compacted`，但 MVP 不新增复杂 history panel。

### 13. 深模块边界

本 MVP 优先形成四个可孤立测试、对外接口窄的深模块：

1. **Usage normalizer**：吸收三家 provider 的字段与 cache 语义差异，只向 Core 输出统一 request usage；
2. **Request preparation/context budget**：快照动态 system prompt，统一 limit、reserve、margin、exact anchor、estimate 和 admission decision；
3. **Context projection**：从 transcript、accepted compaction、system prompt 和 tools 构建唯一出站 request 与 fingerprint；
4. **Compaction planner/validator**：选择完整 turn boundary，生成 source digest，并决定 pending output 是否可以原子提交。

Core 负责 lifecycle、identity、queue 和 effect 调度；`AsyncHost` 负责实际并发 operation；UI 只消费投影后的可见状态。任何层都不应重新实现 token 口径或 coverage 规则。

## Testing Decisions

### 好测试的定义

- 测试公开行为、事件顺序和持久状态，不断言私有辅助函数或容器布局；
- 给定相同 `Model + Msg`，断言相同 `Step`、phase、identity 和 command；
- provider transport 测试喂任意切片的 SSE/NDJSON fixture，不访问网络；
- compaction 测试使用可控 fake effect 和预制 summary，不断言真实模型一定生成某段措辞；
- UI 测试断言用户看到的 status/queue/transcript，不把内部 summary 当成 transcript fixture；
- 每个失败测试都同时验证“没有发生什么”：旧 record 未改、queue 未丢、transcript 未删、迟到事件未推进状态。

### 必测模块与行为

1. **Provider adapters**
   - OpenAI usage-only chunk、`finish_reason`、`[DONE]` 的排序与恰好一次 terminal；
   - Anthropic input/cache/output 累积与 `message_stop`；
   - Ollama total/cached/output 归一化；
   - usage 缺失、malformed frame、split frame 和 terminal error。

2. **Usage/accounting**
   - request identity 匹配与 stale usage no-op；
   - 同一 turn 多次 tool continuation 的 turn sum；
   - context occupancy 等于 latest/projected request，而非 repeated input sum；
   - cached tokens 不重复计数；
   - cancel/error 无 final usage 时保持 estimate。

3. **Context budget/projection**
   - dynamic system prompt 在 admission 前进入 prepared request，且 preparation 本身不访问模型网络；
   - stale/duplicate `RequestPrepared` 不启动 stream；
   - configured/provider limit 取较小值；
   - reserve/margin/headroom 配置验证；
   - system prompt、tools、model 和 compaction 变化使 fingerprint/anchor 失效；
   - soft trigger、hard admission 与 exact/estimate 标记；
   - no-compaction projection 与现有 request 在语义上等价。

4. **Compaction planner/validator**
   - 只选择完整 turn，tool call/result 不被拆开；
   - protected raw tail；
   - compaction input 覆盖计划内全部消息；
   - schema、digest、boundary、summary cap、minimum savings 和 after-budget gate；
   - no eligible prefix 和 source-too-large 的失败结果。

5. **Core/AsyncHost lifecycle**
   - `Idle -> Compacting -> Idle/Streaming` 的唯一 effect 顺序；
   - compaction 期间 submit 只进入 queue；
   - buffer 隔离与 atomic commit；
   - stale/duplicate finish、error 和 delta no-op；
   - cancel、provider error、invalid/no-gain summary 的 rollback；
   - hard limit block 保留 queue；
   - 同一 source digest 不无限重试；
   - compaction operation 不伪造 `TurnId`。

6. **重复滚动压缩**
   - `summary_n + newly covered raw turns -> summary_n+1`；
   - 新 record 原子替换旧 record；
   - projection 只保留最新 accepted summary 与 raw tail；
   - 完整 transcript 在两次以上压缩后仍逐条存在。

7. **UI 与回归**
   - exact/estimated context、turn input/output 的状态栏内容与降级布局；
   - `Compacting` activity、queued count 和 queue preview；
   - compaction summary 不进入 transcript/scrollback；
   - 既有 streaming、permission、tool、cancel、blocked 和 queued-turn 测试继续通过。

### 测试先例

- provider framing 与协议映射沿用现有 OpenAI、Anthropic、Ollama transport fixture 风格；
- reducer 与 identity 行为沿用现有 agent-loop、tool-loop 测试；
- 跨线程 ownership 与迟到事件沿用现有 AsyncHost 测试；
- 用户可见状态沿用 status-bar、screen、view 和 UI-loop 测试；
- 普通 CTest 不调用真实 provider。真实模型只作为显式 smoke/calibration 步骤。

## Out of Scope

以下能力明确不进入本 MVP：

- transcript 删除、原地改写或只保留摘要；
- session persistence、历史恢复时持久化 compaction record、checkpoint/restore；
- `/compact` 等 slash command 和手工选择压缩范围；
- queue item 的编辑、删除、重排和持久化；
- 每个 turn 都调用 provider token-count API；
- OpenAI Responses token-count 迁移、Anthropic count-tokens 接口和 Ollama metadata/runtime context 自动发现；
- provider/model context limit 的完整在线 catalog 与自动更新；
- 层级式/map-reduce compaction 和超大 source 的分块总结；
- exact fact ledger、tool-specific reducer、artifact snapshot 和 provenance graph；
- 完整的 prompt-injection/provenance 防御；
- 语义差分 corpus、must-preserve fact oracle、critic model 和 continuation benchmark；
- salience optimizer、动态摘要风格、compaction model routing；
- RAG/context retrieval 代替旧上下文；
- compaction summary 的用户编辑器、history panel 或多版本回滚 UI；
- 将 OpenAI/Anthropic provider selection 接入当前只运行 Ollama 的主入口。

## Further Notes

### 质量结论

MVP 可以对“输入是否完整、结果是否结构有效、是否真正省 token、是否仍能安全发送”给出确定性答案；不能仅靠 runtime 规则证明摘要语义完备。后续最有价值的质量迭代是固定 fixture 上的 differential continuation eval：同一 probe 分别读取 full context 和 compacted context，以 full-context 可答为条件计算 compacted-context recall，并单独统计 critical fact loss 与 contradiction。

### 已知限制

- provider 通常返回“刚完成 request 的 usage”，而不是一个实时“剩余窗口”字段；remaining context 必须由有效上限和下一次 projection 计算。
- OpenAI 的模型查询接口不应被假设一定返回 context window；显式配置仍是必要 fallback。
- Ollama 的模型最大能力、当前 runner 分配的 `context_length` 和请求 `num_ctx` 是三个不同概念。MVP 优先显式控制请求窗口，而不是频繁轮询服务。
- provider usage 是 observation/anchor，不是 pre-send safety gate 的唯一依据。

### 已确认：长粘贴可能触发 context limit

- composer 中的 paste chip 只是视觉占位符；按 Enter 时仍会展开完整粘贴内容参与 request projection，不会因为 chip 而截断或降低真实输入长度。
- 当前默认配置为 `configured_limit = 8192`、`output_reserve = 1024`、`safety_margin = 128`，因此默认 `hard_input_limit = 7040`。system prompt、tools、消息 framing 以及较长的中文/Markdown 粘贴内容都会计入 projection。
- 在启动后的 `Idle` 场景中，长粘贴可能经历 `PreparingRequest -> Blocked(ContextLimit)`。此时 queue 会被有意保留，provider 不会启动；没有可压缩的 completed turn 时也不能凭空执行 compaction。
- 当前状态栏已在明确进入 `Blocked(ContextLimit)` 时显示 projected input、hard input limit，并提示调整 `MY_AGENT_CONTEXT_LIMIT`。但仍观察到部分超限路径继续显示 `Thinking` 的静默问题；这不是本轮修复范围，暂不在此处改变状态机或默认策略。

### 后续工作

1. 追踪所有 `PreparingRequest`、`RequestPrepared`、`Blocked(ContextLimit)`、`Streaming` 的事件到 phase/view/repaint 路径，定位为何部分超限场景仍停留在 `Thinking`。
2. 增加启动后长粘贴的 PTY 回归测试，断言 queue 保留、provider 不启动、状态最终可见为 `Blocked(ContextLimit)`，而不是只检查内部 projection 结果。
3. 明确 blocked queue 的恢复 UX：调整预算或模型配置后的重新准入，以及 queue item 的 recall/edit/drop；当前 queue 编辑、删除和重排仍不属于 MVP。
4. 使用真实 Ollama 校准保守 estimator、`num_ctx`、output reserve、safety margin 与 compaction headroom，评估默认值是否造成过早阻塞。
5. 完成 CTX-07 的手工验收：普通输入、长粘贴、tool continuation、压缩取消/失败和至少两轮 rolling compaction，并记录结果。

### 尚待校准、但不阻塞架构的参数

- soft trigger 的比例或 headroom；
- output reserve 与 safety margin；
- protected raw turns 的默认数和 token budget；
- summary token cap 与 minimum savings；
- compaction prompt 的具体措辞。

这些参数必须可注入，使自动化测试使用小整数边界；生产默认值在最终 HITL 验收中记录，不应散落为测试魔数。

---

# Local Issue Breakdown

以下 issue 是 `$to-issues` 产出的本地 tracer-bullet 草案。编号只在本文件内稳定，不对应 GitHub issue。除最后的真实环境校准外，架构决策已经闭环，因此实现 slice 默认标为 AFK。

## 拆分总览（实现进度）

| # | Title | Type | Current status | Blocked by | User stories covered |
|---|---|---|---|---|---|
| CTX-01 | 将 Ollama request usage 接入 Core 与状态栏 | AFK | 已实现，待最终验收 | None | US-01–US-04, US-06, US-08, US-25–US-28 |
| CTX-02 | 补齐 OpenAI 与 Anthropic usage parity | AFK | 已实现，待最终验收 | CTX-01 | US-05–US-06, US-22, US-25, US-28 |
| CTX-03 | 准备并投影下一次请求，执行 context admission | AFK | 已实现，存在静默超限待排查 | CTX-01 | US-01–US-08, US-18, US-26–US-30 |
| CTX-04 | 在完整 turn 边界完成首次事务式压缩 | AFK | 已实现，待真实 provider 验收 | CTX-03 | US-09–US-18, US-22–US-24, US-26–US-30 |
| CTX-05 | 封闭压缩失败、取消与异步竞态 | AFK | 已实现，待真实 provider 验收 | CTX-04 | US-10, US-15–US-19, US-23–US-29 |
| CTX-06 | 支持重复滚动压缩 | AFK | 已实现，待真实 provider 验收 | CTX-04 | US-11–US-14, US-17–US-22, US-25–US-30 |
| CTX-07 | 校准默认预算并完成 MVP 验收 | HITL | 未完成 | CTX-02, CTX-05, CTX-06 | US-01–US-30 |

建议依赖图：

```text
CTX-01 ──┬──> CTX-02 ───────────────┐
         └──> CTX-03 ──> CTX-04 ──┬──> CTX-05 ──┐
                                  └──> CTX-06 ──┴──> CTX-07
```

原始拆分中的 `Type` 仍保留用于表示执行方式；`Current status` 反映当前本地实现进度。CTX-07 仍是需要真实 provider 和人工操作确认的收尾切片。

## CTX-01 — 将 Ollama request usage 接入 Core 与状态栏

- **Type:** AFK
- **Implementation status:** 已在本地实现，自动化测试已覆盖，待 CTX-07 的真实 provider 验收。
- **User stories covered:** US-01–US-04, US-06, US-08, US-25–US-28

### What to build

打通当前实际运行的 Ollama 路径：终态 frame 先产生带 stream identity 的统一 request usage，再产生 `StreamFinished`。Core 保存 request/turn 可见的 usage，状态栏从领域状态读取 token 事实并区分 exact/estimated；现有实时吞吐率仍可使用 telemetry，但不再独占 token count 的事实来源。

### Acceptance criteria

- [ ] Ollama `done=true` 中的 input、cached input（若有）和 output 按统一语义解析，并在 terminal 前投递。
- [ ] `AsyncHost` 为 usage 补齐当前 typed operation identity 与 request fingerprint，Core 只接受 active operation 的第一次终态 usage。
- [ ] stale、duplicate 或错误 identity 的 usage 是严格 no-op，不能改变 turn/session 统计。
- [ ] 无 usage、取消或错误终结仍能结束 stream，并把相关数字标记为 estimated/unavailable。
- [ ] 状态栏展示最近 request/当前 turn 的 input/output，且普通 transcript 不发生变化。
- [ ] `MY_AGENT_CONTEXT_LIMIT` 对应的值被传入 Ollama `num_ctx` 或有等价的单一配置路径，状态栏与请求不再各用一套窗口。
- [ ] transport、Core、AsyncHost 和 status/view 测试覆盖完整路径，默认测试不访问网络。

### Blocked by

None — can start immediately.

## CTX-02 — 补齐 OpenAI 与 Anthropic usage parity

- **Type:** AFK
- **Implementation status:** 已在本地实现，待 CTX-07 的最终验收。
- **User stories covered:** US-05–US-06, US-22, US-25, US-28

### What to build

让已经存在的 OpenAI 与 Anthropic transport adapter 遵循 CTX-01 的统一 usage/terminal contract。目标是协议 parity 和可测试的 library seam，不在本 slice 中增加主程序 provider selector。

### Acceptance criteria

- [ ] OpenAI streaming request 请求 usage，decoder 支持 `finish_reason`、usage-only chunk 与 `[DONE]` 的真实顺序。
- [ ] OpenAI 在 usage 到达后才投递唯一 terminal；兼容缺失 usage 或缺失 sentinel 的成功结束路径，不造成 owner 永久等待。
- [ ] OpenAI `prompt_tokens` 被归一化为 total input，cached input 被视为其子集而不重复相加。
- [ ] Anthropic 从 `message_start` 与 `message_delta` 合成终态 total input/output，并在 `message_stop` 前投递 usage。
- [ ] Anthropic 普通 input、cache read 与 cache creation 的求和规则有独立 fixture 钉住。
- [ ] 任意 SSE 切片、malformed frame、error event、重复终态都维持恰好一个 terminal 的现有契约。
- [ ] 两个 adapter 输出与 Ollama 相同的 Core event，不向 Core 暴露 provider-specific JSON。

### Blocked by

- Blocked by CTX-01.

## CTX-03 — 准备并投影下一次请求，执行 context admission

- **Type:** AFK
- **Implementation status:** 已在本地实现；长粘贴超限的静默 `Thinking` 路径仍待排查。
- **User stories covered:** US-01–US-08, US-18, US-26–US-30

### What to build

引入尚不压缩历史的第一版 request-preparation seam、`ContextProjection` 与 `ContextBudget`：所有 initial/continuation request 先快照 dynamic system prompt，生成规范化 full-wire projection、fingerprint 和 projected input，再根据 effective limit、output reserve 和 safety margin 决定是否允许联网。这个 slice 先交付准确语义与安全阻塞，为后续 compaction 提供唯一入口。

### Acceptance criteria

- [ ] 无 accepted compaction 时，projection 与现有完整 `Thread` 请求在 provider 语义上等价。
- [ ] Core 在 `PreparingRequest` 中持有稳定 identity；dynamic system prompt 和 provider/model 配置通过 `RequestPrepared` 作为值返回，保持 reducer 纯度。
- [ ] provider 网络请求只会在 preparation 通过 admission 后启动；stale/duplicate preparation 是严格 no-op。
- [ ] request、turn、session 与 context occupancy 四种数字分开存储和展示。
- [ ] 同一 turn 的多次 tool continuation 会累计 turn billing，但 context occupancy 只反映下一次完整 request。
- [ ] configured/provider limit 取较小值；reserve、margin 和 headroom 的非法组合在启动时失败。
- [ ] fingerprint 覆盖 model、system prompt、tools 与 messages；任一变化都会使不匹配的 exact anchor 失效。
- [ ] soft trigger 与 hard input limit 使用可注入小预算完成确定性测试。
- [ ] 下一次 queued request 超过 hard limit 时不启动 provider、queue head 不移入 transcript，并进入 `Blocked(ContextLimit)`。
- [ ] active turn 的 tool continuation 超过 hard limit 时不发送请求，当前 turn 得到可观察错误，后续 queue 保留。
- [ ] UI 用 `~` 区分 estimate，并清楚区分 `ctx` 与 `turn in/out`。

### Blocked by

- Blocked by CTX-01.

## CTX-04 — 在完整 turn 边界完成首次事务式压缩

- **Type:** AFK
- **Implementation status:** 已在本地实现，待真实 provider 验收。
- **User stories covered:** US-09–US-18, US-22–US-24, US-26–US-30

### What to build

交付一次从 trigger 到继续 queue 的完整 happy path：planner 选择一个完整旧前缀，Core 启动无 tools、无 fake turn 的 compaction operation，buffer 接收结构化摘要，validator 验证后原子提交 accepted record，projection 随即用 summary + raw tail 启动原 queued prompt。

### Acceptance criteria

- [ ] Core 有独立 `Compacting` phase、`CompactionId`、command/event 和 typed operation ownership，compaction 不占用 `TurnId`。
- [ ] planner 只覆盖 completed whole turns，至少目标保留最近两个 raw turns，tool call/result 不被拆开。
- [ ] compaction input 包含边界内全部源消息；任何容量裁剪都会缩小 declared boundary，而不是静默漏输。
- [ ] compaction 使用当前 provider/model、空 tools、固定 prompt version 和结构化 summary schema。
- [ ] compaction request usage 归入 request/session 统计的 compaction 类别，不污染当前或上一 user turn 的 input/output。
- [ ] pending delta 只进入隔离 buffer，不进入 transcript、assistant message 或 UI scrollback。
- [ ] validator 至少检查 identity、source digest、boundary、schema、required fields、summary cap、minimum savings 和 after hard budget。
- [ ] 只有全部校验通过才原子替换 accepted record；完整 `Thread` 逐条不变。
- [ ] compaction 期间的新 submit 保持 queued；成功后重新 projection 并按 FIFO 激活原 queue head。
- [ ] `Compacting` 状态和 queued count 在状态栏可见，summary 内容不可见。
- [ ] 无可压缩完整前缀、protected tail 超限或 source 本身无法完整送入时，不伪造成功并进入明确安全路径。

### Blocked by

- Blocked by CTX-03.

## CTX-05 — 封闭压缩失败、取消与异步竞态

- **Type:** AFK
- **Implementation status:** 已在本地实现，待真实 provider 验收。
- **User stories covered:** US-10, US-15–US-19, US-23–US-29

### What to build

把首次压缩从 happy path 补齐为可恢复事务：覆盖 provider error、invalid/no-gain output、用户 cancel、迟到/重复事件以及 hard-limit fallback，确保旧 projection、transcript 和 queue 在所有异常下保持一致。

### Acceptance criteria

- [ ] provider error、empty/invalid summary、digest/boundary mismatch、no-gain 和 after-limit failure 均丢弃 pending buffer。
- [ ] 失败或取消不会修改 accepted record、完整 transcript 或 queued payload。
- [ ] 迟到/重复 compaction delta、finish、error 在 identity 退休后是严格 no-op。
- [ ] cancel 能通过单 command 语义退休后台 operation；后续安全 request 可以继续，不安全 request 进入 `Blocked(ContextLimit)`。
- [ ] 同一 source digest 不会在自动 `drive` 中无限重试；新增 completed turn、配置变化或显式动作才允许新 attempt。
- [ ] blocked 状态保留 queue，现有 resume/cancel 语义没有被 compaction 绕过。
- [ ] AsyncHost shutdown 与 worker completion 不会让 pending buffer 在 Core 外提交。
- [ ] 失败状态只作为 status/error 呈现，不向 transcript 注入伪 assistant summary。

### Blocked by

- Blocked by CTX-04.

## CTX-06 — 支持重复滚动压缩

- **Type:** AFK
- **Implementation status:** 已在本地实现，待真实 provider 验收。
- **User stories covered:** US-11–US-14, US-17–US-22, US-25–US-30

### What to build

在已有 accepted record 上继续增长会话：下一次 compaction 输入为旧 summary 加其边界之后、即将被覆盖的全部 completed raw turns；新 record 成功后原子替换旧 record，provider projection 始终只有最新 summary 与最新 raw tail。

### Acceptance criteria

- [ ] 第二次及后续 compaction 不把已覆盖的原始 transcript 重复加入普通 provider projection。
- [ ] rolling input 精确包含 previous accepted summary 与所有 newly covered raw turns，并生成新的 source digest。
- [ ] 新 `covered_through` 单调前进且落在完整 completed turn 末尾。
- [ ] 新 summary 校验失败时继续使用旧 accepted record；成功时一次性替换。
- [ ] 两次以上 compaction 后，用户可见 `Thread`、turn/message identity 和 tool history 仍完整不变。
- [ ] projection 中不存在 summary 套 summary 的多条堆叠，只暴露最新 canonical summary。
- [ ] rolling source 太大时不静默 trim；MVP 走明确失败/阻塞，并把层级式压缩留给后续版本。
- [ ] 自动测试覆盖至少两轮成功、第二轮失败回滚、stale first-round event 和 no-gain second round。

### Blocked by

- Blocked by CTX-04.

## CTX-07 — 校准默认预算并完成 MVP 验收

- **Type:** HITL
- **User stories covered:** US-01–US-30

### What to build

在全部 AFK slice 完成后，以小 context window 和真实 Ollama 进行端到端验收，校准 production defaults，并审阅一次用户可见状态。若本地具备凭据，可额外执行 OpenAI/Anthropic 显式 smoke，但其缺失不应让普通测试或 Ollama MVP 无法验收。

### Acceptance criteria

- [ ] 全量普通测试通过，且不依赖网络或 provider 凭据。
- [ ] 小窗口真实 Ollama 会话展示：usage 到达、soft trigger、compacting 状态、queued prompt、原子提交和继续回答。
- [ ] 手测至少包含一次 tool continuation，并确认 turn billing 与 context occupancy 不混淆。
- [ ] 手测一次 compaction cancel/failure，确认 transcript 与 queue 无损且没有无限重试。
- [ ] 手测至少两轮 rolling compaction，确认最近 raw turns 与完整 scrollback 均保留。
- [ ] 记录 soft trigger、output reserve、safety margin、raw tail、summary cap 与 minimum savings 的最终默认值及理由。
- [ ] 审阅状态栏在窄终端下的降级优先级，确认新字段不会破坏已有 queue/permission UI。
- [ ] 更新本文状态与验收证据；不自动创建或修改任何远端 GitHub issue。

### Blocked by

- Blocked by CTX-02.
- Blocked by CTX-05.
- Blocked by CTX-06.
