# Specification: Incremental TUI Evolution

## Problem Statement

`my_agent_repl` 已经恢复到可构建、可测试、可运行的状态，但当前对话界面仍保留较早期的视觉表达：用户与助手的身份较弱，正文缺少角色对应的排版语义，连续助手输出与工具活动彼此割裂，权限请求位于固定 dock，流式重绘也尚未具备与长对话相匹配的稳定性和缓存策略。

agentty 已经验证了更成熟的终端对话体验，包括清晰的 speaker identity、Markdown 正文、连续助手 run、结构化工具时间线、内联权限、稳定的流式布局以及层次更明确的 composer 和 status。直接移植其完整 UI 系统并不适合 my_agent：两者拥有不同的全屏布局、滚动、事件循环和渲染所有权，整体替换会破坏已经恢复并验证的 Painter、虚拟终端和 PTY 闭环，也会重新把产品策略耦合进 Maya。

作为终端用户，我需要界面逐步获得更清晰、更紧凑、更接近现代 agent TUI 的阅读体验；作为维护者，我需要每一步都通过 my_agent 已有的语义投影和渲染接缝交付，并以可观察行为驱动下一步，而不是一次性重写整个 UI 或直接修改 Maya 源码。

## Solution

采用分阶段的渐进式演进方案，以 agentty 作为视觉和交互参考，不把它作为必须复制的架构模板。

第一阶段 U1 只建立对话的视觉语言：用户消息显示明确的 `❯ You` 身份，助手消息显示 `✦` 和当前模型名；用户正文保持纯文本语义，助手正文使用 Markdown 语义；角色标题作为身份锚点，正文使用无边框的两列缩进，turn 分割线与该正文区域对齐，既有滚动和 dock 合同保持不变。该阶段先通过语义屏幕投影验证角色和正文类型，再通过 Painter 的虚拟终端输出验证正常及窄视口中的可见结果。

后续阶段在每一阶段重新确认可观察效果后依次推进：U2 合并连续的助手 run；U3 把工具调用升级为通用 AgentTimeline；U4 将待批准权限嵌入相关助手 turn；U5 为已结算与活动中的 turn 建立不同缓存和流式稳定策略；U6 优化 composer 与 status 的信息层级和响应式降级；U7 再评估欢迎页、reasoning、富媒体预览等增强能力。

整个演进继续保留一个深的应用侧投影模块：调用方只需要提供 `Model`、`UiState` 和主题，就能获得完整的语义 `ScreenConfig`。Maya 继续只承担通用 Element、布局和 FrameBuffer 渲染；my_agent 自己持有角色命名、模型展示、消息分组、工具语义、权限位置、布局降级和事件循环策略。

## User Stories

1. As a terminal user, I want user and assistant turns to have visibly distinct identities, so that I can scan a long conversation quickly.
2. As a terminal user, I want user turns labelled `You`, so that the transcript reads like a conversation rather than a debug log.
3. As a terminal user, I want user turns to use a prompt-shaped glyph, so that prompts are recognizable without relying only on color.
4. As a terminal user, I want assistant turns to use a distinct assistant glyph, so that generated answers are visually anchored.
5. As a terminal user, I want assistant turns labelled with the active model name, so that I know which model produced the visible answer.
6. As a terminal user, I want a sensible assistant fallback label when no model name is available, so that the header is never blank.
7. As a terminal user, I want user input rendered as literal plain text, so that characters such as Markdown markers are not reinterpreted in my prompt.
8. As a terminal user, I want assistant answers rendered as Markdown, so that headings, lists, emphasis and code are readable.
9. As a terminal user, I want tool-call text to remain visible while assistant Markdown is introduced, so that U1 does not lose existing execution evidence.
10. As a terminal user, I want inline errors to remain attached to their assistant turn, so that failures keep their conversational context.
11. As a terminal user, I want each turn anchored by a clear role header and its body consistently indented without a hanging rail, so that multiline content remains calm and complete.
12. As a terminal user, I want message bodies to remain visible in a normal 80×24 terminal, so that richer headers do not consume the content.
13. As a terminal user, I want message bodies to remain usable in narrower terminals, so that the identity treatment degrades without hiding the answer.
14. As a keyboard user, I want existing transcript scrolling and latest-output guidance to keep working, so that visual improvements do not regress navigation.
15. As a terminal user, I want the composer, permission prompt and status bar to retain their current behavior during U1, so that one slice changes one observable concern.
16. As a maintainer, I want role presentation derived in the semantic screen projection, so that application policy remains outside Maya.
17. As a maintainer, I want role presentation tested through the existing public projection interface, so that tests survive internal helper refactors.
18. As a maintainer, I want final visible cells tested through the existing Painter interface, so that semantic correctness and real layout are both covered.
19. As a maintainer, I want no Maya source patch for role identity or Markdown selection, so that the pinned dependency remains independently upgradeable.
20. As a maintainer, I want agentty treated as reference material rather than a source transplant, so that my_agent keeps its own runtime and viewport architecture.
21. As a terminal user, I want consecutive assistant messages to read as one logical run in a later slice, so that multi-step agent work is not visually fragmented.
22. As a terminal user, I want tool activity presented as typed, compact timeline entries in a later slice, so that commands and results are easier to scan than ASCII dumps.
23. As a terminal user, I want tool details to remain available without dominating the transcript, so that routine actions stay compact while diagnostics remain reachable.
24. As a terminal user, I want permission requests displayed next to the tool action that needs approval in a later slice, so that approval context is local.
25. As a terminal user, I want settled turns to render cheaply in long conversations, so that streaming remains responsive as history grows.
26. As a terminal user, I want an actively streaming turn to update without stale cached cells, so that new tokens appear reliably.
27. As a terminal user, I want streamed Markdown to avoid unnecessary layout jumping, so that reading remains comfortable while output arrives.
28. As a terminal user, I want composer chrome to become quieter and more focused in a later slice, so that the conversation remains the visual center.
29. As a terminal user, I want status information to degrade by importance at narrow widths, so that essential model and phase state remain readable.
30. As a new user, I want a concise welcome state only after the core conversation view is stable, so that decorative work does not precede daily usability.
31. As a maintainer, I want every UI phase to start from one failing behavioral test, so that each change demonstrates new coverage before implementation.
32. As a maintainer, I want each future phase evaluated against real rendered output before the next phase is fixed, so that the roadmap can respond to evidence.
33. As a maintainer, I want existing full-screen ownership, virtual-terminal verification and PTY lifecycle checks preserved, so that visual evolution does not weaken recovery guarantees.
34. As a maintainer, I want any new UI abstraction to pass the deletion test, so that thin forwarding modules do not add indirection without hiding complexity.
35. As a project owner, I want each phase to be independently reviewable and reversible, so that I can stop at a coherent UI baseline if later ideas do not justify their cost.

## Implementation Decisions

- Delivery is incremental. U1 is the only implementation authorized by this first cycle; U2 through U7 remain ordered follow-up stages and begin only after the preceding rendered result is reviewed.
- The existing semantic screen projection remains the primary application interface. Role identity, assistant label selection and body-slot semantics are hidden behind that interface rather than distributed among callers.
- The assistant header uses the current UI model display name when available, then the current request policy model, then a stable `Assistant` fallback. The transcript and status bar therefore agree without requiring every non-REPL caller to populate UI telemetry.
- User turns use a prompt glyph and `You` label. Assistant turns use an assistant glyph and the resolved model label. Existing semantic theme colors continue to distinguish the header glyph and label; U1 intentionally drops the incomplete per-turn rail.
- User message text remains a plain-text body slot. Assistant message text becomes a Markdown body slot. Existing tool-call bodies remain plain text during U1 and retain their current order after assistant text.
- Empty streaming assistant messages continue to use the existing thinking indicator instead of producing an empty turn.
- U1 does not introduce speaker-run grouping, structured tool cards, inline permission, turn caching or a new streaming Markdown state machine. Those concerns remain separate vertical slices.
- The application-side turn composition adapter remains responsible for adapting Maya primitives to my_agent's independently scrolled full-screen transcript. It renders a colored role header followed by a borderless two-column body indent. No product-specific Maya source changes are required.
- The application-side inter-turn divider uses the same two-column indent as the body container and consumes the remaining available width. Alignment is the contract; no fixed divider character count is exposed.
- Agentty's continuous turn rail is a useful visual reference, but it is not copied into this host: Maya's bordered turn allocation clips multiline bodies inside my_agent's independently scrolled full-screen viewport. A header-only anchor preserves the information hierarchy without importing that architectural mismatch.
- The current alternate-screen Painter, viewport model, dock layout, repaint ownership and terminal lifecycle remain authoritative. Agentty's inline layout and native scrollback strategy are not transplanted.
- Future U2 may use Maya's continuation semantics, but message grouping policy must remain in my_agent and preserve the underlying conversation messages.
- Future U3 may use Maya's AgentTimeline rendering primitive, but conversion from tool state to typed timeline entries must remain an application projection concern.
- Future U4 may use Maya's generic Permission primitive, but approval identity and routing remain bound to my_agent's current tool-call reference.
- Future U5 separates immutable settled content from the live edge. Cache identity must change whenever visible content changes, and the active turn must never reuse stale settled output.
- Future U6 keeps responsive priority policy in my_agent. Generic layout primitives may be reused, but Maya does not decide which product fields are essential.
- New modules are added only when they hide meaningful policy or enable two real adapters. Thin pass-through wrappers are rejected by the deletion test.

## Testing Decisions

- Good tests assert behavior users and callers can observe through existing public interfaces. They do not call private projection helpers, inspect renderer internals, assert helper invocation order or mock my_agent-owned collaborators.
- The primary seam is semantic projection from `Model + UiState + Theme` to `ScreenConfig`. It catches role labels, glyphs, body-slot semantics, model-name fallback, errors and tool-slot preservation. It misses actual grid allocation, wrapping, clipping and terminal lifecycle behavior.
- The secondary seam is Painter output observed through a virtual terminal grid. It catches visible headers, complete multiline Markdown output, body/divider alignment, the absence of a hanging rail, viewport allocation and narrow-terminal degradation. It misses real PTY timing, operating-system resize delivery and partial writes.
- The PTY seam is reserved for changes to streaming, resize, scroll and lifecycle behavior. U1 must not add a PTY test unless the semantic or virtual-terminal seams reveal a behavior that cannot be verified without it.
- Existing screen projection, painter, viewport and UI-loop tests are prior art. New tests extend those behavioral suites instead of creating a parallel UI test harness.
- U1 proceeds in vertical slices: first role identity, then body rendering semantics, then visible Painter behavior. Each slice demonstrates Red before the smallest Green and runs focused regression checks before the next test is written.
- The role-identity test uses literal expected labels and glyphs from this specification, not values recomputed from the implementation.
- The body-semantics test distinguishes user plain text from assistant Markdown through the public `ScreenConfig` variant. It also confirms that existing tool activity remains represented.
- Painter tests use fixed viewport dimensions and assert meaningful visible text or cells rather than entire ANSI byte snapshots.
- Narrow-width tests verify that message bodies remain observable and that headers degrade safely; they avoid freezing incidental whitespace unless it is part of the visual contract.
- Mocks are unnecessary for U1. Future external timing or terminal I/O behavior may use existing deterministic fakes at those system seams, but internal UI modules remain real.
- Focused screen and painter tests run after each slice. The complete deterministic CTest baseline runs before U1 is declared complete.
- Manual execution of `my_agent_repl` supplements automated verification for visual feel, but it is not a substitute for repeatable semantic and cell-level tests.

## U2 Design: Logical Assistant Runs

### Expected Observable Effect

一次用户提交可能经历多次模型请求和工具调用，但它在 transcript 中应读作一个连续的 assistant run。属于同一逻辑回合的相邻 assistant 消息只显示一次 `✦ <model>` 标题，消息之间不出现 inter-turn divider；各段 Markdown、工具调用文本和错误仍按领域消息原有顺序完整显示。新的用户消息或新的逻辑回合继续开始新的视觉 turn，并保留对齐后的 divider。

U2 不把领域消息物理合并，也不改变 provider history。它只改变 `Model -> ScreenConfig` 的语义投影，因此工具调用仍可通过原始 `MessageId` 和 `ToolCallRef` 精确定位，流式目标消息也不会失去身份。

### Grouping Rules

- 第一条可见 assistant 消息总是 run head，显示完整标题。
- 仅当当前消息和紧邻的上一条可见消息都是 assistant，且二者拥有相同、非零的 `TurnId` 时，当前消息投影为 continuation。
- 不把缺失身份的 `TurnId{0}` 当作可合并身份；恢复或测试产生的未知消息默认保持独立，避免偶然吞掉标题和 divider。
- 用户消息始终开启新的视觉 turn。不同 `TurnId` 的相邻 assistant 消息也保持独立，以便异常或导入历史仍可辨认。
- continuation 只抑制重复标题和 inter-turn divider，不重排、不拼接也不删除 Markdown、工具调用或错误 body slot。
- 空的流式 assistant 尾部继续使用 thinking indicator；当该尾部开始产生文本时，它加入同一 run，而不是突然出现第二个 assistant 标题。

### Module and Seam Decisions

- 现有 screen projection module 继续提供唯一的 `project_screen(Model, UiState, Theme) -> ScreenConfig` interface；调用方无需了解 run 分组。
- 分组实现留在 `transcript_config` 的实现内部，并复用 Maya `Turn::Config::continuation` 语义。不会增加只转发配置的浅 module，也不会修改 Maya。
- `TurnId` 是分组真相，role adjacency 只是必要条件。渲染层不反向推断领域身份。

### TDD Slices

1. Primary Red at the semantic projection seam: two adjacent assistant messages with the same non-zero `TurnId` project as one run head plus one continuation, while their body slots remain separate and ordered.
2. Boundary Red at the same seam: different `TurnId` values and zero identities do not merge.
3. Painter Red at the virtual-terminal seam: a tool-assisted run displays one assistant header, no divider inside the run, all content in order, and a divider before the next user turn.
4. PTY coverage is added only if streaming the continuation exposes timing, resize or viewport behavior that semantic and Painter tests cannot observe.

### U2 Non-goals

- Replacing plain tool-call text with AgentTimeline; that remains U3.
- Moving permission UI into the run; that remains U4.
- Caching settled runs or changing streaming Markdown behavior; those remain U5.
- Altering message persistence, request encoding, tool execution or `TurnId` allocation.

## Out of Scope

- Replacing the complete TUI in one change.
- Copying agentty's application layout, event loop, scrollback ownership or private state model.
- Modifying Maya source to encode my_agent role, model, tool, permission or responsive-layout policy.
- Implementing U2 speaker-run merging, U3 structured tool timelines, U4 inline permissions, U5 settled/live caching, U6 composer/status redesign or U7 welcome and rich-content features in the U1 cycle.
- Changing conversation storage, provider protocols, tool execution, permission decisions, RAG behavior or runtime phase semantics.
- Reworking the existing Painter viewport, alternate-screen lifecycle, scroll controls or repaint scheduler without a separately observed defect.
- Adding timestamps, token metadata, reasoning panels, checkpoint controls or rich media before the basic turn language is validated.
- Achieving pixel-for-pixel or behavior-for-behavior parity with agentty.

## Further Notes

- my_agent and the reference project already pin the same formal Maya revision, so the visible gap is primarily application projection and composition rather than a missing Maya upgrade.
- The current application-owned turn composition is intentional: stock turn composition previously lost body rows inside my_agent's independently scrolled full-screen layout. The adapter preserves the dependency while keeping that host-specific correction local.
- The agreed testing seams are: semantic projection for meaning, Maya element composition as an adapter detail, Painter plus virtual terminal for visible layout, and PTY only for time- and terminal-dependent behavior.
- U1's target visual contract is deliberately small: in an 80×24 terminal, a user turn reads `❯ You`, an assistant turn reads `✦ <model>`, user text remains literal, assistant content renders as Markdown, both bodies remain visible, multiline content has a consistent indent without a partial left rail, and inter-turn dividers align with the body region.
- After U1, the next phase is chosen from rendered evidence rather than roadmap momentum. If identity and Markdown expose spacing, wrapping or streaming problems, those observations take priority over U2.
