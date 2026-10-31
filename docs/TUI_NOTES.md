# TUI 设计决策与实测记录

`accelerate/m5-m7` 分支上的 TUI 迁移已经完成到 #23。本文保留早期自研实现的实测依据，
并在末尾记录 Maya 接入后的职责、数据与端到端复验结果。

这份文档与 [ACCELERATION_NOTES.md](ACCELERATION_NOTES.md) 同一个体例：不讲代码怎么写
（看 commit 和源码注释更准），只记录**当时面对什么问题、有哪些选项、选了哪个、
为什么、代价是什么**，以及**实测数字**。每条末尾附一句可迁移的一般原则。

TUI 这一轮的特点是**大量判断只能靠量测决定**。异步 + 终端 + 信号三者叠加，
「读代码觉得对」的失效率高到不能接受 —— 下面每个带数字的地方，数字都是真跑出来的。

---

## 目录

- [零、这个 issue 存在的理由：一个实测缺陷](#零这个-issue-存在的理由一个实测缺陷)
- [一、分层与 seam](#一分层与-seam)
- [二、终端控制](#二终端控制)
- [三、唤醒与事件循环](#三唤醒与事件循环)
- [四、帧节奏](#四帧节奏)
- [五、信号](#五信号)
- [六、文本宽度](#六文本宽度)
- [七、破坏验证：三次都改写了我的结论](#七破坏验证三次都改写了我的结论)
- [八、端到端实测](#八端到端实测)
- [九、被拒绝的方案](#九被拒绝的方案)
- [十、Maya 迁移后的审计](#十maya-迁移后的审计)
- [十一、ScreenConfig 之后的渲染消费路径](#十一screenconfig-之后的渲染消费路径)
- [附录：可迁移原则速查](#附录可迁移原则速查)

---

## 零、这个 issue 存在的理由：一个实测缺陷

不是「想做个好看的界面」。行式 REPL 有一个**量测过的**缺陷：

```
chunks=2
  t=+0.01s  banner
  t=+19.14s 整个回答
```

19 秒里屏幕一片死寂，然后所有文本一次性砸出来。原因不在打印逻辑，而在
`run_until_quiescent()` 的语义 —— 它在 `phase == Streaming` 时**根本不返回**，
所以前端在整个流式期间拿不到控制权。`Printer` 每次 drain 后追加增量的设计没有错，
错在它一个回合只被调用一次。

这决定了 TUI 不是渲染问题而是**并发结构问题**：必须让前端在 provider 还在飞的时候
就能跑。修完之后同一条路径：

```
chunks=58   t=+13.9s ~ +16.8s 连续到达
```

**可迁移的一般原则**：把 UI 卡顿归因给「渲染慢」之前，先量一次事件到达的时间分布。
2 chunks 和 58 chunks 是完全不同的病，前者根本不在渲染层。

---

## 一、分层与 seam

八个切片对应八个 seam，每个都可以独立测：

| 层 | 职责 | 纯? | 测试数 | 对应 agentty/Maya |
|---|---|---|---|---|
| `view` | `Model` → `Frame` 投影 | 纯 | 11+ | agentty `render()` 的上游投影 |
| `maya_projection` | `Frame` → `maya::Element` | 纯 | focused | Maya element builder |
| `terminal` | 备用屏 / raw mode / 光标 | IO | 9 | Maya `PosixTerminal` 的边界 |
| `wake_signal` | eventfd 唤醒 | IO | 4 | Maya `PosixWakeFd` |
| `repaint_clock` | 帧预算 | 纯 | 5 | agentty 帧节流 |
| `input` | 字节 → `Key` | 纯 | 5 | Maya `InputDecoder` |
| `resize_watch` | SIGWINCH → fd | IO | 4 | Maya `SignalWatch` |
| `ui_loop` | poll 驱动 + `apply_key` | 混合 | 10+ | 不使用 `maya::run` |

投影、布局输入和时钟这些纯 seam 都不需要 pty、不需要线程、不需要真实终端。
这不是巧合而是分层的目的：把「难测的东西」压到尽可能少的层里。

### 决策 1：`view` 是纯投影，输入行不进 `Model`

**问题**：用户正在打字的那半行字放哪。

**选项**

- A 放 `Model`，跟对话历史一起。状态集中。
- B 放 `UiState`（前端私有），`view(model, ui, size)` 三参数投影。

**选择 B。理由**：`update()` 不关心用户打了一半的东西 —— 它只在提交时看到一条完整的
`Submit`。把编辑缓冲放进 `Model` 意味着每敲一个键都要走一次 `update()`，
而 `headless_runner_test` 的快照比较会因为「打字中间态」而变得毫无意义。

**代价**：`view` 多一个参数，前端要自己维护 `UiState`。换来的是**光标位置、
滚动偏移、输入缓冲这类纯显示状态永远不会污染领域模型**。

`apply_key` 的签名把这个边界钉死了：

```cpp
struct KeyOutcome {
    std::optional<Msg> msg;   // 有值 = 这次按键跨越了前端与领域的边界
    bool quit{false};
};
KeyOutcome apply_key(const Key& key, UiState& ui, const Model& model);
```

绝大多数按键 `msg` 为空（只改 `UiState`）。需要 `model` 是因为同一个键在不同 `phase`
下含义不同 —— `AwaitingPermission` 时 `y` 是批准而不是文本。

**可迁移的一般原则**：判断一个状态该不该进领域模型，问「纯核心需要它来做决定吗」。
编辑中间态的答案永远是否 —— 领域只关心**提交**，不关心**输入过程**。

### 决策 2：`view` 承担折行与裁剪，而不是交给终端（实现结论已失效）

**历史选择**：折行与裁剪曾由 `view` 执行，`Frame` 保证行数和列数契约；输入行则
横向保留尾部。这个判断当时是在没有布局库的前提下保护光标定位的正确性。

**迁移后的结论**：#17 将执行者移交给 Maya/Yoga。`view` 现在保留完整语义文本，
`maya_projection` 把它转换成非收缩的文本节点，Maya 负责显示列宽、折行、隐藏溢出以及
尾部可见性。行数契约没有被取消，只是从本项目的 `view` 实现移到了已测试的布局层；
输入行也由同一层处理，因此旧的「输入行必须恰好一行」实现细节不再适用。

**保留下来的原则**：下游若按行数定位，必须有一个组件持有行数契约。迁移改变了执行者，
没有改变这个正确性要求。

### 决策 3：状态行不依赖已到达的文本

`Streaming` 时显示 `... thinking`，`ExecutingTool` 显示 `... running <name>`，
`Idle` 返回**空串**（空闲时占一行反而是噪声）。

关键约束：**第一个 token 到达之前它就得在屏幕上**。这正是原缺陷的用户体验层面 ——
用户分不清模型在想还是进程卡死了。状态行只读 `phase`，不读 `messages`，所以
`Submit` 之后立刻可见。

`AwaitingPermission` 时显示 `allow <tool>? [y/n]`，工具名由 `view` 从历史消息的
`tool_calls` 里**反查** id 得到（`Model` 不为渲染冗余存一份）。从后往前找，
因为待审批的调用总是最近那条 assistant 消息发起的；找不到时退回 id 本身 ——
宁可显示哈希串也不能显示空白。

**可迁移的一般原则**：进度指示的数据来源必须是**状态**而不是**产出**。
依赖产出的指示器在「还没有产出」这个最需要它的时刻恰好是空的。

---

## 二、终端控制

### 决策 4：备用屏（alt screen）而不是 Inline 模式

**选项**

- A Inline：在当前屏幕上就地重绘，退出后内容留在 scrollback 里。
- B 备用屏 `\x1b[?1049h`：进入独立缓冲区，退出后原样还原用户的 shell。

**选择 B。理由**：Inline 模式需要精确记账「我占了几行、光标现在在第几行」，
而这个记账会被任何一次尺寸变化、任何一次意外输出（provider 的 stderr）打乱。
备用屏把这个问题整个消掉 —— 屏幕是我的，坐标从 (1,1) 开始。

**代价**：退出后看不到对话历史。这是真实的损失，但 Inline 的正确性成本更高，
且行式回退路径仍然保留着「历史留在终端里」这条通道。

**可迁移的一般原则**：需要绝对坐标的渲染，先争取一块**独占的坐标系**。
在共享坐标系里做行记账，是把别人的行为变成自己的正确性前提。

### 决策 5：全帧重绘，不做行级差分（自研结论已失效）

**历史选择**：自己写每帧全量输出，理由仍然成立——稳态窗口只有 157 字节，自己维护
行级差分则要承担行数变化和残留旧字符这类最难查的显示 bug。

**迁移后的结论**：不是「差分不值得」，而是「**自己写**差分不值得，**使用已经写好
并测过的**差分值得」。Maya 的 `FrameBuffer` 已经维护 cell 级前后缓冲，并且测试了
成功写入后 `commit`、写失败跳过 `commit` 和相同帧的空 diff。因此现在使用 Maya 的
差分；项目仍然保留 20fps 帧预算，避免在没有事件时重绘。

**可迁移的一般原则**：优化判断依赖实现成本。一个已有测试覆盖的专用组件，会改变
「不值得自研」的工程权衡，但不会推翻原始带宽测量。

### 决策 6：崩溃还原不能用 RAII

**问题**：raw mode 没还原意味着用户的 shell 从此不回显，只能敲 `reset` 才救得回来。

RAII 处理正常退出，但**SIGSEGV 时析构不会执行**。所以额外挂 `install_crash_handler()`。

两个位置选择是刻意的：

1. 放在**真实入口**（`main.cpp`）而不是驱动构造函数里 —— 挂信号是**进程级副作用**，
   测试要能构造驱动而不动它。
2. `TerminalDriver` 的作用域必须**比 `run_ui` 大一点**：析构负责退出备用屏并还原
   termios，而给行式回退路径打的那句提示得等还原之后才打，否则它会连同备用屏一起被
   丢掉，用户看到的是一个没有任何说明的空白屏。

```cpp
{
    my_agent::ui::TerminalDriver terminal{STDIN_FILENO, STDOUT_FILENO};
    terminal.install_crash_handler();
    if (my_agent::ui::run_ui(host_runtime, terminal)) {
        host_runtime.shutdown();
        return 0;
    }
}   // 析构在这里：备用屏退出、termios 还原
std::cout << "\nNot a terminal, falling back to line mode." << ...;
run_line_repl(host_runtime);
```

**可迁移的一般原则**：RAII 覆盖不到崩溃路径。任何「不还原就会让用户环境坏掉」的
副作用都需要一条不依赖析构的还原路径 —— 并且把它挂在真实入口，不要挂在可测的类里。

### 决策 7：非 tty 判断放在 `run_ui` 里，不放在调用方

`run_ui` 在非 tty 时**立即返回 false**，调用方回退到行式 REPL。

**理由**：「能不能跑」是那个循环自己的前置条件，调用方只需要知道它没跑。
判断放调用方意味着两处都要知道 tty 检测规则。

行式回退路径不是遗留代码，而是**有意保留的通道**：管道、CI、重定向、`| tee`
场景下往文件里吐转义序列毫无意义；而它本身还是阶段一的验收出口，
保留着它就保留了一个不依赖终端的端到端通道。

**可迁移的一般原则**：前置条件属于**有那个条件的组件**，不属于调用方。
返回值告诉调用方结果，不要求它复述判断逻辑。

---

## 三、唤醒与事件循环

### 决策 8：唤醒必须是可 poll 的 fd —— mutex + condvar 做不到

**这是整个 TUI 的技术核心。**

原来的 `WakeSignal` 是 mutex + condvar + `bool signaled_`（见
ACCELERATION_NOTES 决策 5）。语义完全正确，但**condvar 无法被 poll**。
而 TUI 需要同时等两件事：键盘可读、后台有消息。

`poll` 只认 fd，所以 `WakeSignal` 换成 **eventfd**：

```cpp
int AsyncHost::wake_fd() const noexcept;   // 放进 poll 集合
```

这不是优化而是**能力问题** —— 没有可 poll 的唤醒，就不可能在流式期间读键盘，
而那正是这个 issue 要解决的事。

**代价**：eventfd 是 Linux 专有，而且**在容器里可能被 seccomp 过滤掉** ——
所以类内已经带了自管道回退（语义一致，多一个 fd），两条路都失败时留在 -1
成为惰性哨兵：`signal`/`wait` 变成空操作，调用方靠超时轮询继续跑。
降级运行胜过启动失败，而三条路径的差异被完整关在这一个类里。

**可迁移的一般原则**：并发原语的选择要看**它能不能被别的等待机制观察**。
「语义正确」的原语如果无法参与统一的事件等待，就会强迫上层开线程去桥接它 ——
那是更糟的复杂度。

### 决策 9：电平唤醒必须 drain —— 一个量测出来的真 bug

`wake_fd` 的 POLLIN 是**电平**的：不取走那个计数，它就一直亮着。后果有两层，
第二层是我一开始没想到的：

1. `poll` 每次立刻返回 → 循环退化成忙转。
2. **`poll` 的超时被彻底短路** → 帧预算的 `pending_` 机制（决策 11）失效。

实测同一个 30 秒窗口：

| | 输出字节 | CPU 时间 |
|---|---|---|
| 未 drain | 4110 | 2.91s |
| drain 后 | 157 | 0.00s |

修法是循环里一句：

```cpp
if (wake_slot >= 0 && (fds[wake_slot].revents & POLLIN) != 0) {
    static_cast<void>(host.wait_wake(std::chrono::milliseconds{0}));
}
```

**这个 bug 是破坏验证暴露的，不是读代码发现的** —— 详见第七节。

**可迁移的一般原则**：把 fd 放进 `poll` 之前，先确认它是电平还是边沿触发。
电平 fd 不 drain 不只是忙转，它还会让**所有依赖 `poll` 超时的机制静默失效**。

### 决策 10：drain → paint → poll 的顺序是延迟属性，不是正确性要求

**这是一条我写错又改回来的结论。**

我最初在注释里写「drain 必须在 poll 之前，否则会丢事件」。破坏验证时把两者换序，
**流式测试依然通过** —— 因为 `RepaintClock` 的 `pending_` 会把被跳过的重绘变成一次
有限的 `poll` 超时，事件最迟在下一轮上屏。

所以真实的性质是：drain 在前，事件在**本轮**上屏；drain 在后，最迟**下一轮**
（≤50ms）。这是一帧的延迟差异，帧时钟是兜底。注释按实测改写了。

**可迁移的一般原则**：注释里写「必须」之前，试着违反它。违反了还能过的「必须」
是**延迟属性**或**性能属性**，把它写成正确性要求会让后来的人不敢碰真正安全的重排。

---

## 四、帧节奏

### 决策 11：跳过的重绘必须转成有限超时

20fps 的帧预算。`RepaintClock::should_paint()` 返回 false 时，
**必须**记下 `pending_ = true`，并让 `time_until_next_paint()` 返回剩余时间给 `poll`。

**为什么是正确性而不是优化**：`StreamFinished` 之后**不再有任何唤醒**。
如果最后一个 delta 恰好落在帧预算里被跳过，而 `poll` 又用了无限超时，
那最后一段文本**永远不会上屏** —— 用户看到一个缺了尾巴的回答。

这条不变量有专门的测试：`PaintsTheFinalChunkEvenThoughNoWakeFollowsTheEndOfTheStream`
（delta + `StreamFinished`，之后没有任何事件）。

**可迁移的一般原则**：任何「稍后再做」的节流都必须自带**触发稍后**的机制。
依赖「反正下次还会有事件」是错的 —— 最后一次事件之后就没有下次了，
而那恰好是用户最在意的一帧。

### 决策 12：没有 wake fd 时用哨兵超时

`wake_fd()` 返回 -1（eventfd 创建失败）时，`poll` 用固定的哨兵超时而不是
`pending` 时间。理由：此时后台消息不再有唤醒通知，只能靠轮询兜住 ——
屏幕会有延迟，但循环照常跑，不该因此拒绝启动。

**可迁移的一般原则**：降级路径要**明确降级到什么**，而不是让主路径的参数在退化
输入下碰巧还能用。这里 `pending` 在无 wake fd 时语义已经不对了，需要另一个值。

---

## 五、信号

### 决策 13：SIGWINCH 走自管道，不靠 EINTR

**问题**：窗口尺寸变化要重排。信号处理器里几乎什么都不能做 —— 不能加锁、
不能分配、**不能调 `ioctl` 查尺寸**（非 async-signal-safe）。而事件循环此刻正阻塞在
`poll` 上。

**选项**

- A 只靠 `poll` 被 EINTR 打断，循环转一圈自然重绘。
- B 自管道：处理器只做一件 async-signal-safe 的事 —— 往管道写一个字节；
  fd 放进 `poll` 集合。

**选择 B。三个理由**，第一个是决定性的：

1. **A 依赖处理器已安装**。`SIGWINCH` 的默认动作是**忽略**，而被忽略的信号
   根本不打断 `poll`。所以 A 并不是「不装处理器的简单方案」，它同样要装 ——
   只是装一个空函数，还必须记得不带 `SA_RESTART`。
2. EINTR 只能让循环转一圈，重绘本身还得靠那次重绘 —— 而它可能正好被帧预算跳过。
3. 走 fd 之后，重排和别的事件走**同一条路径**，没有第二套逻辑。

处理器的全部内容：

```cpp
extern "C" void resize_signal_handler(int)
{
    const int fd = g_resize_write_fd.load(std::memory_order_relaxed);
    if (fd < 0) { return; }
    const int saved_errno = errno;          // 处理器必须存还 errno
    const char token = 'w';
    static_cast<void>(::write(fd, &token, 1));
    errno = saved_errno;
}
```

**`errno` 存还是必须的**：处理器可能在主流程两条语句之间插入，
`write` 失败会覆盖主流程正要检查的 `errno`。

**构造顺序**：`pipe()` → 两端设 nonblock + cloexec → **先发布全局写端** →
再 `sigaction`。反过来会有一个窗口：处理器已生效但写端还是 -1，信号被丢掉。

**析构顺序**：还原处理器 → 清全局 → 关 fd。这个顺序保证**正在运行的处理器
永远不会往已被复用的 fd 号上写** —— 那是静默的数据损坏。

**可迁移的一般原则**：把信号接进事件循环，用自管道而不是 EINTR。
「默认被忽略的信号不打断阻塞调用」这一点让 EINTR 方案并不更简单，
而 fd 方案顺带让信号和普通事件共用一条路径。

### 决策 14：`ResizeWatch` 的嵌套行为 characterize 而不是修

处理器只认得一个全局写端，所以**后构造的胜出**，而且内层析构会把全局状态一并清掉
—— 外层从此也收不到信号。

**选择**：写测试**固定这个行为**，并把「同时只该存在一个实例」写进头注释，
而不是加引用计数栈。

**理由**：`run_ui` 里那一个实例的作用域覆盖整个循环，实际路径上不会嵌套。
为不存在的场景写代码是在增加不会被执行的分支。但**约束必须有据可查** ——
靠读实现才发现的约束等于没有约束。

```cpp
// 内层析构之后外层**也**收不到了 [...] 这不是想要的行为，但也不是崩溃 ——
// 记下来是为了让「同时只该存在一个」这条约束有据可查。
ASSERT_EQ(0, ::raise(SIGWINCH));
EXPECT_FALSE(readable(first.fd()))
    << "characterization：嵌套后外层不再工作，约束是同时只存在一个";
```

**可迁移的一般原则**：单例式的进程级资源，与其支持嵌套，不如**测试并文档化
不支持嵌套**。characterization 测试把「不支持」从口头约定变成会失败的断言。

### 决策 15：合并信号突发是正确性，不是优化

拖动窗口边框会连发几十个 SIGWINCH。`drain()` 一次读空管道并返回「是否发生过变化」，
50 次信号换来一次重排。

中间尺寸画出来的帧全是浪费，而且**按最终尺寸重排一次**才是用户要的结果。

**可迁移的一般原则**：事件合并在「只关心最终状态」的场景下是正确性的一部分 ——
逐个处理不只是慢，它还会让中间态短暂可见。

---

## 六、文本宽度

### 决策 16：字节 ≠ 码点 ≠ 显示列（实现已由 Maya 替换）

三个量仍然不同：CJK 字符可以是 3 字节、1 码点、2 显示列。这个正确性结论没有失效，
失效的是本项目自己维护 `display_width` 的实现。#17 删除自有表后，Maya 的 Unicode
数据生成结果承担显示宽度、emoji presentation 的现代/旧终端模式与运行时降级。

迁移前的具体对照见 #17：my_agent 的 W/F 表与 Maya 都是 122 段、182,719 个码点；
Maya 另有 80 段、1,212 个 emoji-presentation 码点，其中 26 个不在 W/F 表内。

### 决策 17：极窄终端的失败方式由 Maya 固定

「窄终端也不能崩溃、不能切出非法 UTF-8、必须保留最新内容」仍然是验收原则，
但执行者已经从自有 `wrap`/`tail_within` 换成 Maya 的布局与 overflow 处理。#17 的
1–2 列测试继续作为投影 seam 的回归，不再把旧的具体裁剪结果当作实现契约。

**可迁移的一般原则**：退化输入没有正确答案时，明确选一种**失败方式**并测试它。
不选的结果是每次重构都换一种失败方式，而其中某些会破坏上层契约。

---

## 七、破坏验证：三次都改写了我的结论

第七节单独立出来，因为 TUI 这一轮的破坏验证**没有一次是「确认我写对了」**——
三次都推翻或修正了我原本的判断。这比全部通过有价值得多。

| 破坏 | 我的预期 | 实际结果 | 我改了什么 |
|---|---|---|---|
| paint 移到 drain 之前 | 流式测试失败 | **通过** | 改注释：延迟属性，不是正确性（决策 10） |
| 去掉 `pending_ = true` | 末帧测试失败 | **通过** | 发现真 bug：`wake_fd` 从未 drain（决策 9） |
| 去掉 resize 的强制重绘 | 重排测试失败 | **通过** | 删掉那段代码 —— 它是多余的 |

第二行是最值得记的：一次**没有咬到的破坏**才是那轮最有价值的信息。
`pending_` 破坏掉还能过，说明 `poll` 的超时路径根本没被走到 —— 顺着这条线才找到
电平 fd 未 drain。修完之后**重跑同一个破坏，它正确地红了**。

第三行是我自己写的一段投机代码：resize 时额外强制一次 `render` 绕过帧预算。
破坏掉还能过，说明帧时钟已经覆盖了这个场景 —— 删掉，并把这次量测写进注释，
免得日后有人再加一遍。

**可迁移的一般原则**：破坏验证的价值主要在**没咬到的那次**。
「破坏了还能过」有两种解释 —— 测试没有鉴别力，或者**另有一条你不知道的路径**。
第二种是找 bug 的线索，不要急着改测试。

---

## 八、端到端实测

单元测试全绿不等于功能可用（ACCELERATION_NOTES 决策 22 已有三次证据）。因此 #23
重新在真 pty 上覆盖循环，而不是只复用 `view` 快照。

### 当前自动化复验

| 路径 | seam 与断言 |
|---|---|
| 流式 | `PaintsStreamingTextBeforeTheTurnEnds`：回合不结束时先看到独有流式文本 |
| 最后一段 | `PaintsTheFinalChunkEvenThoughNoWakeFollowsTheEndOfTheStream`：终态之后无 wake 仍补画 |
| 批准 | `ApprovesARealToolCallThroughThePtyAndShowsItsDetails`：基线后发送 `y`，看到 `allow read`、`effect=read_fs`、实际参数、`[done]` 与后续哨兵；fake effect 确认参数确实传入执行边界 |
| 拒绝 | `RejectsARealToolCallThroughThePtyWithoutExecutingIt`：基线后发送 `n`，看到 `[rejected]` 与后续哨兵，fake effect 保持未调用 |
| 还原 | `terminal_test` 在真实 pty 比较 raw mode 进入前后属性；进入/退出字节覆盖备用屏与光标显隐，崩溃子进程覆盖不依赖析构的还原 |
| 真实 SIGWINCH | `ReflowsOnTerminalResizeWithoutWaitingForAKeypress`：`TIOCSWINSZ` 后只 `raise(SIGWINCH)`，不发送键盘字节仍按新尺寸重排 |
| 行式回退 | `/dev/null` seam 确认 `run_ui` 返回 false；终端驱动不向非 tty 输出 ANSI |

批准和拒绝测试在副作用前先记录 pty 输出基线，并断言参数/后续回复使用不可能预先
存在的哨兵串。这样 `[done]` 或 `[rejected]` 的出现不能被测试启动时已有的帧伪造。

### 历史缺陷与历史 HITL 证据

早期 pty 验证抓到两个单元测试永远发现不了的缺陷：`run_ui` 忽略了驱动持有的输入 fd，
以及 `view` 漏渲染 `message.error`。两者已经修复，原始诊断仍保留在历史记录中。

此前用真实 Ollama 做过一次 HITL 复验：流式路径从 2 个末尾 chunk 变为 58 个连续 chunk；
批准、拒绝、还原和真实窗口变化均通过。#23 的自动化测试把其中依赖外部 provider 和用户
文件系统的部分换成 deterministic fake stream/effect，但仍穿过真实 `poll`、raw mode、
备用屏、Maya 写出和 `run_ui`，因此专门锁定事件循环与显示边界。

**可迁移的一般原则**：自己写的验证脚本也要验一遍。「断言通过」和「断言有鉴别力」
是两件事；断言要用**不可能预先存在的哨兵**，基线要在**副作用发生之前**抓。

### 一个诊断方法

同一个测试**失败两次**之后，我停止改代码，改为量测：写两个 pty 探针，
一个带 200ms 延迟（通过）、一个不带（失败，`bytes=46 phase_streaming=0`）。
差异隔离出根因：**`tcsetattr(TCSAFLUSH)` 丢弃 raw mode 之前排队的输入**。

反复读代码不会发现这个 —— 它在 libc 的行为里，不在我的代码里。
修法是让测试同步在备用屏字节 `\x1b[?1049h` 上（它写在 flush **之后**，
所以是一个有效的同步点）。

**可迁移的一般原则**：同一个测试失败两次就停止推测，改为构造**两个只差一个变量**
的探针。第二次失败是「我的心智模型错了」的信号，而模型错了的时候，
再读一遍代码只会得到同样的错误结论。

---

## 九、被拒绝的方案

### 实现层面

| 被拒方案 | 看起来为什么合理 | 真正的问题 |
|---|---|---|
| 输入缓冲放进 `Model` | 状态集中，只有一份真相 | 每敲一键走一次 `update()`，快照比较失去意义 |
| 超宽文本交给终端回卷 | 终端本来就会折，少写一层 | 实际行数超出 `Frame` 行数，光标定位失准 |
| Inline 模式 | 退出后历史留在 scrollback | 行记账会被尺寸变化和意外输出打乱 |
| 自己实现行级差分重绘 | 少发字节 | 稳态才 157 字节；自研差分 bug 表现为残留旧字符，最难查 |
| 只靠 RAII 还原 termios | 教科书答案，作用域清晰 | SIGSEGV 时析构不执行，用户 shell 从此不回显 |
| 崩溃处理器挂在驱动构造函数里 | 和还原逻辑放一起，内聚 | 挂信号是进程级副作用，测试无法构造驱动 |
| tty 判断放调用方 | 调用方决定要不要进 TUI | 两处都要知道 tty 规则；前置条件属于有条件的组件 |
| 继续用 condvar 做唤醒 | 已经在用，语义正确 | **无法被 poll**，流式期间读不了键盘 —— 能力问题 |
| 只靠 EINTR 处理 SIGWINCH | 不用建管道，代码更少 | 被忽略的信号不打断 `poll`，仍要装处理器；且重绘可能被帧预算跳过 |
| `ResizeWatch` 加引用计数栈 | 支持嵌套，更通用 | 实际路径不会嵌套，是为不存在的场景写分支 |
| 按码点折行 | 比按字节对，且简单 | CJK 是 2 列，实际宽度会是预期的两倍 |
| resize 时强制重绘绕过帧预算 | 重排要立刻可见 | 破坏验证证明帧时钟已覆盖 —— 多余代码 |

### 范围层面（本轮明确不做）

不是没时间，是**判断当前不做更好**：

- **自己实现行级差分**：稳态带宽已量过（157 字节 / 30 秒），收益小于一类难查的显示 bug；
  当前使用 Maya 已测试的 cell diff。
- **scrollback 视口**：需要一套独立的滚动状态与按键映射，与当前「尾部优先」的裁剪
  策略正交，值得单独一个 issue。
- **Inline 模式 + 行记账**：见决策 4。
- **Esc / Ctrl-C 中断流式**：需要新的 `Msg` 变体 + provider 侧协作停止，
  是领域层改动而不是 UI 改动，不该混在这轮里。
- **更精细的 UI polish**：#21 已提供基础 markdown，#22 已提供基础 turn rail 和语义
  颜色；它们目前刻意保持简单，后续可以继续优化视觉层，不改变事件循环或领域边界。

---

## 十、Maya 迁移后的审计

### 当前职责边界

Maya 现在拥有：

- `Element` 树到 cell canvas、Unicode 显示宽度、Yoga 布局、折行、overflow clipping；
- cell 级 diff、ANSI 序列、DECAWM/EL 的边界处理与色深降级；
- Maya 自带的 markdown/widget 能力，作为后续 UI polish 的可选参考。

my_agent 仍然拥有：

- `Model` 到 `Frame`/`StyledLine` 的投影，以及 `Frame` 到 `maya::Element` 的薄投影层；
- 自有事件循环、`poll`、wake fd drain、帧预算、SIGWINCH 自管道和尺寸合并；
- raw mode、备用屏生命周期、崩溃还原、光标生命周期与非 tty 行式回退；
- `AsyncHost`、`WorkPool`、`WakeSignal`、`HeadlessRunner` 和领域 `update()`。

明确不使用 Maya `run<Program>()`：它会要求 Maya 接管 `update(Model, Msg)` 与 `Cmd`，
这会越过本项目的核心事件循环边界。

### 事件循环的六条原则仍然不变

Maya 只接在 `Frame` 之后，所以以下六条原则没有因依赖迁移而改变：

1. 唤醒原语必须能被统一等待机制观察，才能与键盘和 resize 一起进入 `poll`。
2. 电平触发的 wake fd 必须 drain，否则会忙转并短路帧预算的超时路径。
3. 被帧率跳过的重绘必须留下一个有限的后续触发，尤其是最后一个 stream chunk。
4. wake fd 不可用时必须使用明确的哨兵超时，不能永久阻塞或忙轮询。
5. `SIGWINCH` 通过自管道进入同一个事件循环，不在 signal handler 里执行布局或 IO。
6. resize 突发只按最终尺寸合并处理，避免中间尺寸短暂覆盖最终状态。

### Maya 前后数据对照

数字使用同一份已记录的测量口径；构建时间是 #15 的 clean rebuild，当前产物和自有 LOC
是本地 #23 复验前的工作树测量。LOC 只统计 `include/`、`src/`、`tests/`，不把 Maya
源码算进 my_agent。

| 指标 | Maya 前 | 引入/迁移后 | 说明 |
|---|---:|---:|---|
| W/F 宽度表 | 122 段 / 182,719 码点 | Maya 122 段 / 182,719 码点 + 80 段 / 1,212 emoji-presentation 码点 | #17 对照；额外表中 26 个码点不在 W/F 内 |
| clean rebuild | 58.11 s | 89.28 s（#15）；109.91 s（#23 当前） | #15 与 #23 均为 `--parallel 2`；依赖源码已缓存 |
| `my_agent_repl` 产物 | 20,001,080 B | 25,065,216 B | 前者是 #15 引入前基线，后者为当前构建产物 |
| my_agent 自有 LOC | 12,162 | 15,793（#22）/ 16,000（#23 未提交测试） | 同口径 `include/` + `src/` + `tests/`；增量包含后续 TUI slices，不归因于 Maya 单项 |

这组数据说明迁移的工程取舍：正确的 Unicode/layout/diff 组件由 Maya 承担，my_agent
保留自己的运行时和投影边界；二进制体积与构建时间的变化可以单独量测，不把它们误写成
事件循环被框架接管的代价。

### #23 复验结论

流式、批准、拒绝、还原、真实 `SIGWINCH` 和非 tty 回退均有对应测试；raw mode 相关标志
在 pty 上前后比较，备用屏退出和光标显示由进入/退出序列测试覆盖。当前 rails、状态和
语义颜色实现是基础版本，视觉精修留作后续工作，不影响本次功能闭环。

---

## 十一、ScreenConfig 之后的渲染消费路径

前十节记的是「面对什么选择、选了什么」。这一节体例不同：`ScreenConfig → Element`
之后的三段全是 Maya 既定机制，没有本项目的取舍可记，所以这里记的是**落点表** ——
目的是省掉后来人重新追一遍源码的成本，以及在动这棵树之前先知道它会咬哪里。
下面的行号取自本仓 `maya/` 工作树当前状态，每条写到纸上之前都就地核过。

### 四段管道

```
ScreenConfig
  │ ① 翻译   widget 的 build()                   本项目 + Maya widget 共同写
  ▼
Element 树（变体数据树，不含任何像素）
  │ ② 展平   render_detail::build_layout_tree    renderer.cpp:425
  ▼
vector<LayoutNode>（下标即身份，没有指针）
  │ ③ 求解   layout::compute                     yoga.hpp:263 / yoga.cpp:834
  ▼
每节点一个父相对 Rect（LayoutNode::computed）
  │ ④ 上色   render_detail::paint_element        renderer.cpp:857
  ▼
Canvas（cell + StylePool）→ diff / 序列化 → 终端字节
```

除这四段，还有一层 `thread_local` 的**跨帧组件缓存**，它在 ② 的 measure
（`renderer.cpp:627-691`）和 ④ 的 paint（`:1536-1661`）两处都会短路。缓存的完整
机制记在 `TODO.md` 附录第 5 条。

### ① 翻译：三档物化时机

同一个 `.build()`，成本发生在什么时刻有三档：

| 档 | 例子 | 机制 |
|---|---|---|
| 急切 | `Turn::build()`（`hash_id` 空时，**本项目就是这条**） | 直接 `build_inner`，每帧真建整棵子树（`turn.hpp:160-163`、`:183`） |
| 半惰性 | `Composer::build()`（仅 streaming/executing 态） | 把 `build_impl` 塞进 `ComponentElement.render` lambda（`composer.hpp:236-249`） |
| 惰性 | composer 的 hint 行 | `component(lambda)`（`composer.hpp:618`） |

三档的区别不是「能不能渲染」，而是**谁在什么时候付构造成本**。`Conversation` 走
`cfg.turns` 这条路时是急切档，源码注释写得很直白：*every `Turn::Config` in this
vector gets `Turn{cfg}.build()` called on it once per frame*（`conversation.hpp:56`）。
判据「值不值得改」见 `TODO.md` 第 5 条末尾。

翻译层的产物只有**结构 + 属性**，没有任何画好的东西 —— 连文字都只是
`TextElement{content, style}`。本项目真实的树（一条用户消息 + 流式回复 + 半截输入）：

```
vstack(width=fixed(cols), height=fixed(rows), overflow=Hidden)    terminal.cpp:89-100
├── vstack(grow=1, shrink=1, min_height=fixed(0), overflow=Hidden)   transcript pane
│   └── [scrolly 包装]                                              terminal.cpp:86
│       └── v(conversation)                   有意加的一层，见下    terminal.cpp:79
│           └── Conversation::build()                              conversation.hpp:130
│               └── v(rows) | padding(0,1)                         conversation.hpp:179
│                   ├── Turn element #0 "you"                      turn.hpp:158
│                   │   └── box(Row, border=left Bold, pad left 2)  ← "rail"
│                   │       └── v(header, blank, body_rows, error_block) | grow(1)
│                   └── Turn element #1 "agent"
└── dock | shrink(0.0f)                                            terminal.cpp:99
    └── v(rows) | padding(0,1)                                     screen.cpp:35
        ├── blank()                                                ← 呼吸行
        ├── Composer::build()            → v(inner, rule, hints) | border(Round) | bcolor   composer.hpp:760-762
        └── StatusBar::build()           → v(3 rows)               status_bar.hpp:99,131-133
```

`terminal.cpp:79` 那句 `v(std::move(conversation))` 是**未经核实**的一处。它看着多余：
`Conversation::build()` 返回的已经是 `BoxElement`（`v(rows) | padding(0,1)`），而
`scrolly` 即使拿到非 Box 的 inner 也会自己包一层（`dsl.hpp:974-1016`），所以它不构成
「scrolly 需要它」的理由。`dsl.hpp:925-929` 的注释倒是给了一个方向：多一层包装会改变
`align_items` 的传递（Stretch 只把包装层的那一个孩子拉到包装宽度，不再拉孙子）——
`scrolly` 自己会设 `align_self = Start`（`dsl.hpp:966`），这一层大概是在隔离这个影响。
但**注释和 commit 里都没有写它的理由**，这是读代码的推断。动这一行之前先构造探针，
别按这条推断改。

`fill_available_height = false`（`screen.cpp:203`）会改变整棵 transcript 的形状 ——
不追加尾部 `spacer()`、也不套 `grow(1.0f)`（`conversation.hpp:177-182`）。理由写在字段
自己的注释里（`conversation.hpp:116-120`）：这个 fill 是给 inline 的 AppLayout 用的，
Conversation 在那里要吸收 composer 上方的富余高度；**而全屏宿主把它放进滚动视口时
必须关掉，否则那段合成的空白会变成可滚动内容，跟随尾部时会把上面的真实 turn 藏到
视口外**。注意这跟「composer 不要 `grow`」是两个不同的理由（后者在
`composer.hpp:769-779`，措辞沿用的是 agentty 的 AppLayout，本项目现在的关系是
「transcript pane 吸收富余高度、dock 禁缩」，结论一样但别把两条理由混起来）。

### ② 展平：树 → 平坦向量 + 下标

`build_layout_tree` 深度优先，每个 `Element` `emplace_back` 一个 `LayoutNode` 并
返回其在 `nodes` 里的下标，父节点把子下标存进 `children`（`renderer.cpp:425-737`）。

不用树本身是为了 ③ 的三趟算法需要反复随机访问节点，而**下标是稳定身份** ——
缓存条目、ledger 回写、hit 区域都靠它；同时没有指针就没有生命周期问题，
`nodes.clear()` 就能复用整块内存。

| Element 变体 | 映射到 LayoutNode | **没进 LayoutNode 的** |
|---|---|---|
| `BoxElement` `:431-485` | style 全字段搬运（direction/wrap/align/justify/grow/shrink/basis/width/height/min/max/gap/padding/margin）+ border 折算成每边 1 格 | **border 的字形与颜色**、背景色；`is_stack` 的盒子只把第 0 个孩子纳入布局（`:474-476`） |
| `TextElement` `:487-501` | **只有 measure**：`TextElement::measure(max_width)` | style 完全不进布局 —— 字色粗细不影响尺寸 |
| `ElementList` / `ElementListRef` `:503-531` | 一个默认构造的匿名节点 + 全部孩子 | 它自己没样式，所以叫「透明」 |
| `ComponentElement` `:533-735` | FlexStyle 的**子集**（无 direction/wrap/justify/gap/border）+ measure | 组件内部结构要等 `render` 被调用才出现 |

`MeasureFn` 是**函数指针 + `const void* ctx`**（`yoga.hpp:118-125`），不是
`std::function` —— measure 路径上没有堆分配。所以「每帧全建」的代价主要在建 Element
和 layout，不在 measure 的调用开销上。

`ComponentElement` 的 measure 有三支（`:554-565` / `:566-723` / `:724-733`）：有
`node.measure` 就用它（老路，契约靠人守，注释原话是 *Drift between the two silently
clips rows*）；否则若有 `render` 就**跑一遍 render 然后数行**（自动 measure，注释说
这样 *makes measure/render disagreement structurally impossible*，代价是每帧 render
两次）；都没有则退化成 `{max_width, 1}` 的占位。

**两个陷阱：**

`overflow` 在 Box 分支里被赋值了两次（`:457-459` 显式映射，紧接着 `:471-472` 用
`static_cast` 覆盖）。两者结果相同 —— element 侧枚举（`box.hpp:62-66`）与 layout 侧
（`yoga.hpp:66-70`）顺序都是 Visible/Hidden/Scroll —— 所以第一处是死代码。读代码时
容易以为「这里有一层显式翻译」，其实没有。

`element.hpp:57-59` 的注释说 `ElementList` 会被「spliced directly into the parent's
child list」，实现却是包一个匿名 Column 容器（`renderer.cpp:506-512`）。注释与实现
不符，而多一层容器会改变百分比解析基准与 min/max 链。**本项目不受影响**：grep 零命中
`ElementList`，它唯一的产生者是 `dsl::map()`（`dsl.hpp:385`），本项目没用；
`v(vector)` 走 BoxBuilder 的 range 重载（`builder.hpp:177-181`），出来仍是
`BoxElement`。所以上面那张树图的节点数是真的，没有被藏掉一层。

### ③ 求解：LayoutNode → Rect

`LayoutNode` 只有四个字段（`yoga.hpp:127-132`）：`style` / `children` / `measure` /
`computed`。比 `Element` 少得多 —— **它只回答「多大 / 在哪」，不回答「长什么样」**。

根节点的约束不由调用方的那两行 `width/height(fixed)` 决定，而是 `render_tree` 强制
覆盖的（`renderer.cpp:2047-2053`）：

```cpp
layout_nodes[root_idx].style.width  = Dimension::fixed(canvas.width());
if (!auto_height)
    layout_nodes[root_idx].style.height = Dimension::fixed(canvas.height());
layout::compute(layout_nodes, root_idx, canvas.width(), canvas.height());
```

「整屏」的物理来源是这一句，`terminal.cpp:90-91` 写的只是把话说在前面。

`computed.pos` 是**父相对**坐标。`yoga.hpp:272-276` 有个 `resolve_absolute` 可以累加
成绝对坐标，但 ④ 不调它 —— 因为 `Scroll` 需要在递归途中改写 offset，一次性算绝对
坐标反而做不了平移。

### ④ 上色：两棵树同时走

```cpp
void paint_element(const Element& elem, Canvas& canvas, StylePool& pool,
                   const std::vector<layout::LayoutNode>& layout_nodes,
                   std::size_t layout_idx, int offset_x, int offset_y);
```

签名同时收 `Element` 和 `LayoutNode` 两条线，靠下标对齐 —— 位置来自后者，内容来自
前者。这是 ② 里「LayoutNode 丢掉视觉信息」的**代价**：不可能只走一棵树。

`ax = offset_x + computed.pos.x`（`renderer.cpp:870-871`）逐层累加成绝对坐标。
`Overflow::Scroll` 的视口在递归子项时改写 offset（`:1102-1103`：
`child_ox = ax - live_scroll_x`），并把内容高度**回写**进 `ScrollState` 的
`max_x`/`max_y`（`renderer.cpp:996-1074` 这个块，赋值在 `:1027-1033`）。回写发生在
paint 而不是 layout，这正是 `terminal.cpp:262-266`
「首帧 paint 后检查 `at_bottom()`、必要时重建一次树」存在的原因。

### 裁剪的四类触发（这一节最会咬人的地方）

裁剪**不只由 `overflow(Hidden)` 决定**。`renderer.cpp:965-968`：

```cpp
const bool clip_width  = explicit_clipping ||
    (!is_auto_height() && (bounded_width  || has_b));
const bool clip_height = explicit_clipping ||
    (!is_auto_height() && (bounded_height || has_b));
```

| # | 触发条件 | 裁哪轴 | 受 auto_height 影响 |
|---|---|---|---|
| 1 | `overflow` ∈ {Hidden, Scroll} | **两轴** | 否 |
| 2 | 有 border（`has_b`） | **两轴** | 是 |
| 3 | `width` 是 fixed/percent | **只裁宽** | 是 |
| 4 | `height` 是 fixed/percent | **只裁高** | 是 |

生产路径上 `auto_height = false`：`pipeline.hpp:127` 调的是 5 参重载
`render_tree(root, back_, pool_, theme_)`，默认值在 `renderer.hpp:103`。**所以裁剪在
全屏路径上一直是开着的**，不是只有写了 `overflow(Hidden)` 的地方才裁。

auto-height 之所以要抑制裁剪（`renderer.cpp:944-951` 注释）：inline/measure 路径上
canvas 起始比内容小，裁了内容就长不大、高度永远量不出来 → 空白屏。本项目走全屏
`FrameBuffer`，不碰这条。

把上表套到本项目真实的树上：

- 根 vstack（fixed w/h + Hidden）→ 两轴裁；
- transcript pane（Hidden）→ 两轴裁，且不依赖 auto_height；
- scrolly viewport（Scroll）→ 两轴裁 + 平移；
- **composer 的圆角盒 → 两轴都裁**。它自己没写任何 overflow，裁它的是「它有边框」
  这件事（`composer.hpp:762` 的 `| border(Round)`），裁在内容矩形
  `content_w = aw - inner_horizontal()`（`:969-972`）；
- `Conversation | width(size.columns)`（`terminal.cpp:77`）→ **只裁宽，不裁高**。

3/4 只裁一轴的原因在同一段注释里：*Treating a fixed width as a two-dimensional
boundary truncates naturally-growing text after its first wrapped row.*

### 落点表：`ScreenConfig` 的每个决定落在哪

| `ScreenConfig` 里的决定 | 落点 | 位置 |
|---|---|---|
| `transcript.turns` 的条数与内容 | `Conversation::build` 里 rows 的元素个数 | `conversation.hpp:130` |
| `fill_available_height = false` | 不追加 `spacer()`、不 `grow(1)` | `screen.cpp:203` → `conversation.hpp:177-182` |
| 每条 message → 一个 `PlainText` | `turn.body.emplace_back(...)` | `screen.cpp:215` |
| Turn 的 `label` / `rail_color` | `box().border(Bold, rail_color).border_sides(left)` | `turn.hpp:314-321` |
| dock 的 `padding(0,1)` | `BoxElement.layout.padding` → `LayoutNode.style.padding` | `screen.cpp:35` |
| pane `grow(1)/shrink(1)/min_height(0)` | FlexStyle → yoga 伸缩求解 | `terminal.cpp:80-87` |
| dock `shrink(0.0f)` | `flex_shrink = 0` → yoga 不给它压 | `terminal.cpp:99` |
| `overflow(Hidden)` | `layout::Overflow::Hidden`（裁剪 + shrink 循环 opt-out） | `terminal.cpp:84` |
| `scrolly(scroll, 0)` | Scrollable 组件 + `ScrollState` 回写 | `terminal.cpp:86` |
| `width(size.columns)` | `FlexStyle.width = fixed` | `terminal.cpp:77` |
| **`theme`** | **只变成 Color 值进 `Style` —— `LayoutNode` 完全看不到** | 全链路 |

最后一行值得单独记一句：**`theme` 在这条链上从不参与布局**。它只参与 ① 的颜色计算
和 ④ 的上色。这是「`ScreenConfig` 不是自足结构」这个结论的机制层证据 —— 主题被注入
了两次（投影一次、渲染一次），而不是 `ScreenConfig` 带着它就够了。

### 视觉链与布局链是两条

拿同一个 `Composer::Config` 的两个字段各走一遍：

```
# 视觉链：phase → 边框颜色
model.phase
  → composer_state()        screen.cpp:130    → Composer::State
  → phase_color()           screen.cpp:152    → cfg.active_color
  → box_color               composer.hpp:280-285（loop / awaiting 的优先级也在这里）
  → | bcolor(box_color)     composer.hpp:762  → BoxElement.border.color
  → 【不进 LayoutNode】
  → paint_border 时进 StylePool → 序列化时变成 SGR

# 布局链：一个行数下限
cfg.min_body_rows = 1       screen.cpp:195
  → while (body_rows.size() < min_body_rows) push blank   composer.hpp:542
  → inner 多一行 → Element 树多一个节点
  → LayoutNode 的 measure 结果 +1 → Rect 高 +1
  → dock 高 +1 → transcript pane 可用高度 -1
```

**同样出自 `ScreenConfig` 的两个字段，一个只走视觉链、一个只走布局链。** 分不清这两条
时，改一个颜色会以为要动布局，改一个尺寸会以为会被样式覆盖。这是读这套渲染器最省事的
一把钥匙。

**可迁移的一般原则**：位置与内容拆到两棵结构里维护，代价是消费方必须同时走两条线并
靠位置对齐；换来的是一套纯几何的求解算法不必知道任何视觉概念。选哪边要看求解算法的
复用价值 —— 这里值。

---

## 附录：可迁移原则速查

脱离本项目上下文的版本。接着 ACCELERATION_NOTES 的 25 条往下编号。

**分层**

26. 判断状态该不该进领域模型，问「纯核心需要它来做决定吗」。编辑中间态的答案永远
    是否 —— 领域只关心**提交**，不关心**输入过程**。
27. 当下游要按「行数」定位时，产出方必须保证行数契约。把折行推给终端等于放弃契约，
    而它会在光标定位这种间接的地方失效。
28. 进度指示的数据来源必须是**状态**而不是**产出**。依赖产出的指示器在
    「还没有产出」这个最需要它的时刻恰好是空的。

**终端 / 独占资源**

29. 需要绝对坐标的渲染，先争取一块**独占的坐标系**。在共享坐标系里做行记账，
    是把别人的行为变成自己的正确性前提。
30. RAII 覆盖不到崩溃路径。会让用户环境坏掉的副作用需要一条不依赖析构的还原路径，
    且挂在真实入口而不是可测的类里。
31. 前置条件属于**有那个条件的组件**，不属于调用方。返回值告诉调用方结果，
    不要求它复述判断逻辑。

**事件循环**

32. 并发原语的选择要看**它能不能被别的等待机制观察**。语义正确但无法参与统一事件
    等待的原语，会强迫上层开线程去桥接 —— 那是更糟的复杂度。
33. 把 fd 放进 `poll` 前先确认电平还是边沿。电平 fd 不 drain 不只是忙转，
    它会让**所有依赖 `poll` 超时的机制静默失效**。
34. 任何「稍后再做」的节流都必须自带**触发稍后**的机制。「反正下次还会有事件」是错的
    —— 最后一次事件之后就没有下次，而那恰好是用户最在意的一帧。
35. 降级路径要明确降级到**什么**，而不是让主路径的参数在退化输入下碰巧还能用。
36. 把信号接进事件循环用自管道而不是 EINTR。默认被忽略的信号不打断阻塞调用，
    所以 EINTR 方案并不更简单，而 fd 方案让信号和普通事件共用一条路径。
37. 事件合并在「只关心最终状态」的场景下是正确性的一部分 —— 逐个处理不只是慢，
    它还会让中间态短暂可见。

**退化输入**

38. 单例式的进程级资源，与其支持嵌套，不如**测试并文档化不支持嵌套**。
    characterization 测试把「不支持」从口头约定变成会失败的断言。
39. 退化输入没有正确答案时，明确选一种**失败方式**并测试它。不选的结果是每次重构
    都换一种失败方式，而其中某些会破坏上层契约。

**注释与结论**

40. 注释里写「必须」之前试着违反它。违反了还能过的「必须」是延迟属性或性能属性，
    写成正确性要求会让后来的人不敢碰真正安全的重排。

**验证**

41. 破坏验证的价值主要在**没咬到的那次**。「破坏了还能过」意味着测试没有鉴别力，
    **或者另有一条你不知道的路径** —— 第二种是找 bug 的线索，不要急着改测试。
42. 自己写的验证脚本也要验一遍。断言要用**不可能预先存在的哨兵**，
    基线要在**副作用发生之前**抓 —— 一次性脚本没有人会 review。
43. 同一个测试失败两次就停止推测，构造**两个只差一个变量**的探针。第二次失败是
    「我的心智模型错了」的信号，而模型错了的时候，再读一遍代码只会得到同样的错误结论。
44. 把 UI 卡顿归因给「渲染慢」之前，先量一次事件到达的时间分布。

**结构与契约**（第十一节）

45. 「透明」是**声明**，不是结构。文档说某个节点会被拼进父节点的 child list，实现可能
    给它包一层容器 —— 而多一层容器会改变百分比与 min/max 的解析基准。验证透明要看
    布局树，不要看注释。
46. 一个行为的触发条件如果既有显式开关又有若干隐式来源，就把隐式的那几处也写出来。
    只在显式开关处写注释，后来的人会把「没写开关」读成「不会发生」。
47. 位置与内容拆到两棵结构里维护，代价是消费方必须同时走两条线、靠位置对齐；
    换来的是一套纯几何的求解算法不必知道任何视觉概念。值不值取决于求解算法的复用价值。
48. 紧挨着的两次赋值要问哪一次是真的。被下一行覆盖的映射读起来像一层翻译，实际是
    死代码 —— 而它会让后来的人以为改那里有效。
    2 chunks 和 58 chunks 是完全不同的病，前者根本不在渲染层。
