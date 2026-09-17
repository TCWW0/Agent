<h1 align="center">my_agent</h1>

<p align="center">
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

<p align="center">
  <b>C++26 的终端编码智能体 —— 从零重建，每个切片走一遍红绿循环。</b><br>
  对 <a href="https://github.com/1ay1/agentty">agentty</a> 的从零复刻：事件循环、传输层、
  工具与检索全部自建 —— 唯一外借的是渲染层。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-26-blue?style=flat-square" alt="C++26" />
  <img src="https://img.shields.io/badge/CMake-%E2%89%A54.4-064F8C?style=flat-square" alt="CMake 4.4" />
  <img src="https://img.shields.io/badge/renders-via%20maya-8A2BE2?style=flat-square" alt="maya" />
  <img src="https://img.shields.io/badge/provider-localhost%20Ollama-green?style=flat-square" alt="Ollama" />
</p>

## 为什么是 my_agent？

这个项目存在的理由，是搞懂一个 TUI Agent**到底怎么工作** —— 靠亲手重建一个，而不是读一个。每个子系统以独立切片落地、各走一遍红→绿循环，提交记录就是课程表。

- **事件循环才是主题。** 手工搭的 Elm 风格核心 —— `AsyncHost` 之上的 `(Model, Msg) → Model`，外加工作线程池与唤醒信号。[maya](https://github.com/1ay1/maya) 刻意**只用于渲染**；它的 `run<Program>` 循环在本仓库被明令禁用 —— 因为「拥有这个循环」正是项目的意义所在。
- **把终端当作对手。** raw mode 由 RAII 还原、**也**由信号处理器还原（SIGSEGV 下析构函数不会跑）；驱动用 `Synced`/`Divergent` 一致性状态机追踪屏幕，resize 或半途写失败都不可能在屏上留下幽灵单元格。每个论断都在真 pty + 虚拟终端模型上验证，宽度数据用官方 Unicode 16。
- **本地优先。** 模型是 localhost 的 Ollama；检索完全离线（没有嵌入服务时退化为关键词路）；memory/skills 是 `.my_agent/` 下的普通文件 —— Claude Code 的 `.claude/` 目录直接读，不用复制。
- **单二进制、零运行时依赖。** SSE 解析、BM25、RRF 融合、TUI —— 全部是自写的 C++/STL，只站在三个小型的固定版本库之上。

<a id="getting-started"></a>

## 快速开始

要求：C++26 工具链、CMake ≥ 4.4、OpenSSL，以及本地跑着的 [Ollama](https://ollama.com)。

> **maya 检出不在本仓库里。** 当前分支需要一份本地
> [maya](https://github.com/1ay1/maya) 工作副本，且包含尚未进入上游的
> `Composer::CaretMode::SolidCell` 扩展 —— 放在 `./maya`，或用
> `FETCHCONTENT_SOURCE_DIR_MAYA` 指向它。缺了它 CMake 会按设计直接报错，
> 而不是悄悄退回某个旧的上游 pin。

```bash
git clone git@github.com:TCWW0/Agent.git my_agent && cd my_agent
# 把你的 maya 检出放到 ./maya（见上面的说明）
cmake -B build && cmake --build build -j
ollama pull qwen3.5:latest        # 或者任何你已有的模型
./build/my_agent_repl
```

全部配置走环境变量：

| 变量 | 默认值 | 含义 |
|------|--------|------|
| `MY_AGENT_OLLAMA_HOST` | `localhost` | Ollama 服务主机 |
| `MY_AGENT_OLLAMA_PORT` | `11434` | Ollama 服务端口 |
| `MY_AGENT_MODEL` | `qwen3.5:latest` | 使用的模型 |
| `MY_AGENT_PROFILE` | `write` | 权限 profile：`minimal` · `ask` · `write` |
| `MY_AGENT_CONTEXT_LIMIT` | `8192` | 状态栏显示的上下文预算 |
| `MY_AGENT_DOCS_DIR` | `./docs` | `search_docs` 的语料根目录 |

不在 tty 上（管道、CI、`| tee`）？TUI 拒绝启动，二进制回落到带 `[y/N]` 权限确认的行式 REPL —— 一条不依赖终端的端到端路径，在测试里保持全绿。

## 功能特性

<table>
<tr>
<td width="50%">

### 🧠 记住你的事实
`remember` / `forget` 工具把纯 JSONL 写进 `~/.my_agent/memory.jsonl` 与 `<项目>/.my_agent/memory.jsonl`。两个 scope 的内容**每一轮**都重新注入系统提示 —— 会话中途记住的事实，下一次请求就在场。

### 📖 Agent Skills
把 `SKILL.md` 放进 `.my_agent/skills/` 或 `.claude/skills/` 即生效 —— 项目根覆盖 `~/`，已为 Claude Code 装好的 skill 无需复制一份。只有「名字 + 描述」这一层进提示；正文由 `skill` 工具按需加载，资源文件只列不读。

### 📚 本地检索
对你的文档目录跑 `search_docs`：BM25 + 稠密嵌入的混合检索、RRF 融合、增量语料缓存、无嵌入服务时优雅退化为纯 BM25。完全离线。[运作原理 ↓](#retrieval-rag)

</td>
<td width="50%">

### ⚡ 自有事件循环，无框架
`AsyncHost` —— `dispatch` / `run_until_quiescent` / `model()` —— 驱动 `(Model, Msg) → Model` 的 reducer。工具跑在工作者线程池上、通过可 poll 的唤醒信号叫醒循环；重绘按帧窗口合并。渲染接缝只有两个调用：`FrameBuffer::render()` + `commit()`。

### 🛡️ 权限 profile
每个工具声明效果集（`ReadFs` · `WriteFs` · `Net` · `Exec`）。`minimal` 对读也要确认、`ask` 对写确认、`write` 放开工作区；`read` 工具对工作区之外的路径一律拒绝，没有例外。

### 🖥️ 扛得住现实的 TUI
全屏备用屏；raw mode 由 RAII **和**崩溃处理器双路还原（SIGSEGV 之后你的 shell 依然可用）；resize 整屏重同步而不是留幽灵单元格；composer 按 UTF-8 字符（而非字节）编辑。

</td>
</tr>
</table>

## 模型提供方

出厂入口只连**本地 Ollama** 服务 —— 无需密钥、不出云。**Anthropic** 与 **OpenAI** 的线上格式（请求展开、SSE 流解码、交错工具调用累积）已实现并有传输层测试覆盖，但尚未接进 `main` —— 目前离入口只差一个接缝。

```bash
MY_AGENT_MODEL=qwen3.5:latest ./build/my_agent_repl    # 默认
MY_AGENT_OLLAMA_HOST=192.168.1.10 MY_AGENT_OLLAMA_PORT=11434 ./build/my_agent_repl
```

<a id="retrieval-rag"></a>

## 检索（RAG）

与其把整份文档塞进每一条提示，`search_docs` 只取回一小批**带来源标注**的段落。
引擎完全本地；唯一可选的网络跳是 localhost 的 Ollama 嵌入服务 —— 没有它就
退化为纯 BM25，照常工作。

```bash
export MY_AGENT_DOCS_DIR=~/my-project/docs   # 或者直接建一个 ./docs
ollama pull nomic-embed-text && ollama serve # 可选：解锁稠密那半边
```

<details>
<summary><b>管线</b></summary>

1. **语料遍历** —— 对文档根下的每个文件取指纹（大小 + mtime），逐文档分块。
   首次调用建索引；后续调用只重嵌入指纹漂移的文件，所以第二次建库是秒级，
   不是分钟级。
2. **BM25** —— 分词块上的关键词排序（常开路径；零配置、零网络）。
3. **稠密嵌入** —— 块与查询经 localhost Ollama 嵌入，带指令调优角色
   （文档侧与查询侧加不同的前缀，遵循 nomic/e5 惯例）。
4. **RRF 融合** —— 两份排名表经 Reciprocal Rank Fusion 合成一份混合排名；
   嵌入服务不可达时，结果如实标注为纯 BM25，而不是悄悄缺掉一半。

索引在首次 `search_docs` 调用时才**懒建** —— 注册工具本身零 I/O、零网络。
检索质量由 `rag_bench` 跟踪 —— 一个三级阶梯基准（BM25 主场的合成查询、
检验 dense 兜底能力的 LLM 改写探针、按 gold 块身份缓存改写）：

```bash
./build/rag_bench ~/my-project/docs 5 100 nomic-embed-text:latest
```

</details>

## 按键

| 按键 | 动作 | 按键 | 动作 |
|------|------|------|------|
| `Enter` | 发送（空行不发） | `y` / `n` | 批准 / 拒绝待确认的工具 |
| `Backspace` | 删除一整个 UTF-8 字符 | `←` `→` `Home` `End` | 在输入行内移动 |
| `↑` / `↓` | 在折行后的输入行间移动 | `^U` / `^C` | 清空草稿（绝不退出） |
| `Ctrl-D` | 退出 —— 唯一的退出路径 | | |

## 更多

<details>
<summary><b>架构</b></summary>

- **更新循环**：`AsyncHost`（dispatch / `run_until_quiescent` / `model`）跑在
  `runtime/agent.cpp` 的 `(Model, Msg) → Model` reducer 上。流式增量、工具结果、
  权限应答全部作为 `Msg` 走同一条收件箱；后台工作落在工作者线程池上，
  经可 poll 的唤醒信号叫醒循环。
- **视图**：`Model → Element`（`ui/view.cpp`、`ui/screen.cpp`），由 maya 渲染。
  项目只用一个 maya 接缝 —— `FrameBuffer::render()` + `commit()` —— 刻意不用
  `maya::run<App>`：循环是交付物，不是可以外包的细节。
- **终端驱动**：全仓库唯一动 `termios`、唯一写终端字节的地方。备用屏 + raw
  mode 按逆序还原；崩溃处理器（async-signal-safe 的 `write` + `tcsetattr`）
  覆盖析构不跑的那些路径。屏幕一致性是一个 `std::variant`：`Synced`
  （front 缓冲 == 终端像素，可差分）或 `Divergent`（尺寸变化或写失败 ——
  全量重序列化，逐行带行尾擦除，单次写出）。
- **转录**：markdown 按行扫描、带围栏（fence）意识，流到一半的代码块渲染成
  实时文本而不是闪烁。
- **无头模式**：`headless_runner` 不开终端驱动同一套循环，在权限闸门处停下，
  等外部应答。

</details>

<details>
<summary><b>测试</b></summary>

36 个测试二进制、343 个用例，全绿。这套测试不止单元测试：

- **真 pty** —— raw mode、resize、字节级输出都在真伪终端上验证，不用 mock。
- **虚拟终端** —— 自写的 ECMA-48/DEC 模型，把应用*实际吐出*的字节解码成
  网格；断言读作「屏幕上是 X」，而不是「输出包含字节序列 Y」。它在 resize
  时保留旧内容 —— 幽灵单元格就是这样被抓到的。
- **官方宽度判据** —— Unicode 16.0.0 的 East-Asian-Width 数据驱动与渲染器
  同一张宽度表，emoji 不可能在断言两侧被同样地错算。

开发本身就是测试先行：每个切片先落一条红测试提交（`red N`），再落实现
（`green N`），各自指向对应的 GitHub issue。

</details>

<details>
<summary><b>仓库布局</b></summary>

```
src/
  domain/     会话模型与 profile
  provider/   anthropic · openai · ollama 传输层 + SSE 组帧
  http/       HTTPS 客户端（cpp-httplib，强制 OpenSSL）
  prompt/     系统提示组装（环境、记忆、技能目录）
  runtime/    agent 循环 · 异步宿主 · 工作者线程池 · 唤醒信号 · 无头运行器
  tool/       注册表、效果与权限策略、read、memory、skills、search_docs
  rag/        语料 · 分块 · BM25 · 嵌入 · RRF 混合 · 基准
  ui/         事件循环、终端驱动、视图/屏幕、markdown 扫描、
              输入编辑与布局、状态栏、重绘时钟、resize 监视
tests/        36 个 gtest 二进制 + 虚拟终端 + 宽度判据
```

依赖经 FetchContent 固定版本：googletest、nlohmann_json、cpp-httplib ——
以及[快速开始](#getting-started)里描述的本地 maya 检出。

</details>

<details>
<summary><b>现状</b></summary>

已完成：带流式与工具调用的完整 agent 循环、权限闸门、记忆、技能、带基准的
本地检索，以及 TUI（composer、状态栏、resize 安全渲染、崩溃还原）。

进行中（以切片 issue 跟踪）：把 Anthropic/OpenAI 传输层接进入口、resize
滞回、以及小尺寸终端的布局高度预算契约。

</details>

## 关于

个人学习项目 —— [agentty](https://github.com/1ay1/agentty) 的复刻，一片一片
建起来，从字节层面理解终端编码智能体的每一层。暂无许可证；`docs/` 下的
工程笔记保存在本地，不属于仓库。
