# TODO

当前 UI 待评估事项：

- 助手正文在现行 `ScreenConfig` 路径中使用 `PlainText`；Markdown 展示需求尚待确定。
- 工具输出在现行路径中完整显示；是否需要显示长度上限尚待确定。
- `Turn::hash_id` 缓存是否值得接入，须先测量长 transcript 的构建与绘制开销。

以下是 2026-09-18 的历史评审记录。旧 `Frame`、`view.cpp` 和
`maya_projection.cpp` 已于后续清理中删除；下文引用它们的路径和行号仅用于追溯，
不代表当前实现。

---

## 1. 空正文的 assistant 消息需要「正在思考」的在飞指示

**登记**：2026-09-18 · 分支 `dev/ui` · 相邻 issue #26（语义屏幕）
**状态**：已实现；现行实现见 `src/ui/screen.cpp` 的 `transcript_config`，
对应行为由 `tests/screen_test.cpp` 覆盖。以下保留当时的设计依据。

### 目标

流式回合在第一个 token 到达之前，transcript 侧没有任何「活着」的信号 ——
用户分不清模型在想还是进程卡死。期望把这个信号在 transcript 的尾巴上表达出来。

### 已定的落点：`Conversation::Config::in_flight`（不要再往下沉到渲染层）

maya 已经提供了正确的槽位，当时本项目尚未接：

```cpp
// maya/include/maya/widget/conversation.hpp:116
std::optional<ActivityIndicator::Config> in_flight;
// :144-145  渲染在所有 turn 之后
if (cfg_.in_flight) rows.push_back(ActivityIndicator{*cfg_.in_flight}.build());
```

投影层的改动就一行，且判断依据（`phase`）正好在它手上：

```cpp
cfg.in_flight = std::holds_alternative<Streaming>(model.phase)
    ? std::optional<ActivityIndicator::Config>{...}
    : std::nullopt;
```

**选它的四个理由**：

1. 它天然带着「活着」的语义 —— 不需要从空字符串去猜；
2. 它是**位置**而不是**内容替换** —— 消息真到了指示器消失、正文出现，
   不会发生「已显示的文本被改写」（正是旧链路 markdown 增量扫描器在防的事）；
3. 它可以动（`ActivityIndicator` 的磁带 / `ThinkingBlock` 的 spinner 都有动画）；
4. 它不污染 transcript 历史 —— 往回翻不会看到一堆 `Thinking…` 化石。

### 通用判据（本次讨论产出，可复用到其他「空值显示什么」的问题）

> 兜底文本的内容是否依赖于「**为什么**这里是空的」？
> **依赖** → 投影层（它拿得到 Model）
> **不依赖** → widget 层（它拥有槽位语义）
> 任何情况下都不该由**通用渲染器**对**任意空文本**兜底。

即：渲染层是信息最少的地方，而这里恰恰是最需要信息的决定。

### 为什么不能由渲染层兜底 —— 「正文为空」有五种成因

区分它们的唯一依据是 `Model::phase` 与消息在列表中的位置，**这两样都到不了渲染层**
（`Turn::Config` 里只有一个哑的 `PlainText{content=""}`）。

| # | 成因 | 期望显示 | 若由渲染层兜底成 `Thinking…` |
|---|---|---|---|
| 1 | 流式已开始、还没吐第一个 token | `Thinking…`（该动） | ✅ 正确 |
| 2 | 流结束后真的没正文（模型空回复） | 空 / 仅 header | ❌ 撒谎，且永久停在那 |
| 3 | 纯工具调用回合（协议里极常见：只有 tool_use block） | 工具卡片，正文空是**正常**的 | ❌ **最高频路径，每个工具回合都挂假「正在思考」** |
| 4 | `StreamError` 把 phase 打回 Idle、error 有值、text 全空 | 错误行 | ❌ 错误被「正在思考」盖住 |
| 5 | `AwaitingPermission` | 权限提示 | ❌ 显示的不是待办的事 |

### 证据链

| 事实 | 位置 |
|---|---|
| `transcript_config` 只填 `turns`，**没有设 `in_flight`** | `src/ui/screen.cpp:200-227` |
| 每条 message 无条件变成 `PlainText{msg.text}`（空文本也建 turn） | `src/ui/screen.cpp:215` |
| 空 `PlainText` → `render_slot` 返回 `blank()` → `is_blank` 跳过 | `maya/include/maya/widget/turn.hpp:358`（定义）、`:197`、`:269`（跳过） |
| 非 continuation turn 的 body 结构固定含 header + 空行 | `maya/include/maya/widget/turn.hpp:308-311` |
| ⇒ **现状**：空正文的 assistant 消息 = 光秃秃的 `agent` 头 + 一行空，无任何活动信号 | 由上两条合成 |
| 全项目唯一的 "Thinking" 在 **dock 的 status bar** 里，不在 transcript | `src/ui/screen.cpp:245`（`phase_display`，`◆ Thinking` + breathing） |
| 全项目 grep `in_flight` / `ActivityIndicator` 零命中（本项目侧） | `grep -rn "in_flight\|ActivityIndicator" src include tests` |
| 旧链路早有「空回合必须看得见」的教训（错误场景同理） | `src/ui/view.cpp:586-593` 注释 |
| 用户消息不可能为空（空行不发 `Submit`），五种成因里不含用户侧 | `src/ui/ui_loop.cpp:73-77` |

### 验收方式

- **Config 层**（便宜、无需 tty）：断言 `screen.transcript.in_flight.has_value()`
  随 `phase` 变化 —— 参照 `tests/screen_test.cpp` 的 `turn_text()` 断言风格。
- **网格层**（贵但钉得住"正确效果"）：用 `tests/virtual_terminal.hpp` 解码 ANSI
  回格子，断言指示器出现在最后一个 turn 之下、且 `Idle` 时不存在。
- ⚠️ 哨兵约束（`docs/DEBUG_POSTMORTEM_TRANSCRIPT_BODY.md` 教训 6）：
  哨兵串必须满足被测对象的固有领域约束（如 maya StatusBar 的 `verb_width=10`），
  否则断言会假挂。

### 未决问题

1. **用哪个 widget**：`ActivityIndicator` 是 hex dump 磁带风格（host 给词池，词在
   噪声里滚过去，偏"黑客"）；`ThinkingBlock` 是可折叠推理块带 spinner
   （`Thinking... ⠋ ▼` + 内容）。`in_flight` 槽位收的是 `ActivityIndicator::Config`，
   要另一种风格得换槽位或改 widget 选型。
2. **是否只做 `Streaming`**：`ExecutingTool` / `AwaitingPermission` 要不要也走同一个
   槽位，还是继续只由 dock 的 status bar 承担？（重复表达会不会太吵）
3. **和 markdown 回流的关系**：如果将来新链路接回 markdown 渲染（见附录 2），
   `in_flight` 与增量提交前缀的边界要一起定。

---

## 附：同一轮 review 中发现的相邻缺口（历史记录）

这些是本文件第 1 条之外的发现。除第 5 条已展开外，其余**未做进一步调查**，仅登记线索：

1. **两条链路的语义重复且正在漂移** —— `effect_label`、tool_call 四态文案、
   role→rail 色在 `src/ui/view.cpp:60-82,356-374` 与 `src/ui/screen.cpp:54-94,208-213`
   各有一份相同实现，无机制阻止只改一边。
2. **新链路丢了两个旧能力（迁移完成前是视觉退化）**：
   markdown 不解析（`screen.cpp:215` 走 `PlainText`；旧链路见 `view.cpp:215-247`）；
   tool output 的 240 字符截断 + `[... N characters elided]` 消失
   （`view.cpp:19,97-123`）。
3. **状态栏从「空闲不画」变成「恒 3 行」** —— 旧逻辑 `view.cpp:640`
   （`phase != Idle || has_status_metadata` 才画）；新 `status_config` 无条件构建，
   而 `maya::StatusBar::build()` 固定输出 3 行（`status_bar.hpp:113-133`）。
   是有意取舍还是遗漏，未确认。
4. **`Frame::markdown_layers` 是死结构** —— 只有 `view.cpp:527` 写、
   `view_test` 读，运行时无消费者（`view.hpp:65`）。
5. **新链路未用 `Turn::hash_id` —— 每帧重建整棵 transcript 子树**（已展开）

   **事实**：`Conversation::Config::turns` 这条路径，maya 自己的注释写明
   「every `Turn::Config` in this vector gets `Turn{cfg}.build()` called on it
   **once per frame**」（`maya/include/maya/widget/conversation.hpp:56`）。
   而 `Turn::build()` 在 `hash_id` 为空时直接返回 `build_inner(*cfg)` ——
   没有 `component()` 包装，**没有资格进缓存**（`turn.hpp:160`、`:140`）。
   本项目 `grep -rn "hash_id" src tests` 零命中。

   **缓存机制**（逐条读 `maya/src/render/renderer.cpp` 核实，非转述）：

   | 项 | 事实 | 位置 |
   |---|---|---|
   | 生命周期 | `thread_local`，**跨帧**存活，不是每帧新建 | `renderer.cpp:204-207` |
   | 两张表 | 指针键 `entries` + 哈希键 `entries_by_hash` + `hash_lru` | `:190`、`:194` |
   | 宽度 | **是条目的字段，不是键的一部分**；命中要求 `entry.width == width` | `:249-250` |
   | 防盗用 | 指针键额外校验 `generation`；**哈希键忽略 generation** | `:259`、`:242` |
   | 淘汰 | 指针键条目 `last_frame < prev` 即逐出；哈希键走有界 LRU（4096） | `:1994`、`:2006-2013` |
   | 宽度变化 | `clear_component_cache()` 全清 | `:2030` |
   | 命中 | measure 直接返回缓存高度；paint 走 cell blit（memcpy packed 行） | `:627-691`、`:1536-1661` |
   | 未命中 | 调 `render` lambda：measure `(max_width, 1<<20)`、paint `(content_w, content_h)` | `:585,702`、`:1705` |
   | `CacheId{0}` | = 空 = 回退到指针键；`build()` 保证不产出 0 | `cache_id.hpp:104-107`、`:212` |

   **两条实现路径**（都有现成落点，不必自研）：

   - **(a) 显式 `Turn::Config::hash_id`** —— 内容哈希，委托给 maya 的
     `component().hash_id()`（`turn.hpp:177`）。硬要求：id 必须随渲染输出变化，
     **live turn 必须留空**（`turn.hpp:41`）。
   - **(b) `Conversation::Config::PreBuilt` / `frozen`** —— 宿主持有跨帧稳定的
     Element。走 `Element(std::shared_ptr<const Element>)` 构造时**自动生成**
     hash_id，键是 control block 的稳定 id 而非 `sp.get()`
     （`element.hpp:359-381` + `id_for_shared` `element.hpp:156-175`）。
     `PreBuilt` 的注释就是为这条路径写的（`conversation.hpp:60-70`）。

   ⚠️ **缓存用错 id 的症状是幽灵格子，而 maya 自己踩过**：`element.hpp:359-381`
   的注释记着早期版本从 `sp.get()` 派生 id，导致**不相关但落在同一堆地址**的
   shared_ptr 互相碰撞 —— 症状是 *streaming composer / footer ghost*：
   为早已析构的 shared_ptr 捕获的格子带，被 blit 到新内容底下。
   与本项目 #13/#14 的幽灵行是**同一病根**（身份认错 → 格子串位）。
   动这条时要连带想起那条线。

   **未决问题**：

   - 走 (a) 还是 (b)？(a) 改动小但只省下 build，省不掉 `turns` 向量本身的
     逐帧构造；(b) 更彻底但要宿主自己持有 Element 前缀。
   - settled / live 的边界怎么定 —— Turn 文档的硬要求是 live turn 留空，
     但这个项目里「live」除了最后一条流式消息，是否还包括正在执行工具的回合？
   - **值不值得做需要实测，不是估算**。测量手段现成：renderer 自带
     build/layout/paint 三段纳秒计时累加器（`renderer.cpp:2043-2073`），
     也可以比同一 model 连续两帧的字节数。先量，再决定。
6. **状态栏「占位行 + 真身替换」的隐含假设** —— `view.cpp:642-646` 塞一行
   `plain_status_text` 占位，`maya_projection.cpp:143-147` 渲染时按
   `status_bar_line` 换成真 Element。当前成立只因旧链路用 `fit_row` 恰好 1 行，
   无测试守着；换多行 widget 会静默错位光标行号。
