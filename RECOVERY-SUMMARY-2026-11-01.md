# my_agent 项目恢复总结

日期：2026-11-01

## 1. 恢复目标与边界

本轮恢复以「可构建、可测试、可继续演进」为验收标准，核对了恢复
记录、旧交接文档和当前工作树。恢复过程没有追求逐字复制旧文件或
复现旧提交历史，而是依据已验证行为和职责边界重建可运行闭环。

恢复内容按以下三类处理：

- **已验证能力**：直接保留并建立回归基线，特别是完整 RAG 管线。
- **仅有记录回放的能力**：通过公共边界测试重建，不假设旧实现细节仍是最佳
  方案。
- **明确缺失或构建阻塞**：先以 Red 固定可观察效果，再完成最小 Green，
  最后才进行结构收敛。

## 2. 完成的恢复

### 2.1 构建与运行时

- 增加 `MY_AGENT_BUILD_TUI` 选项，核心运行时可以脱离 Maya/TUI 单独构建。
- 恢复 Agent reducer、`AsyncHost`、请求准备、压缩、工具调度、权限、取消、
  回放和系统提示词之间的完整闭环。
- 新增工具副作测试，并将旧测试迁移到当前的强类型 ID 与状态机边界。
- 移除 `agent.cpp` 中六个只做一层转发的 `allocate_*` 函数，ID 分配现在直接
  位于对应状态变化旁，保留了强类型与后增语义。

### 2.2 TUI 与 Maya 边界

- 不再通过修改 Maya 源码获得应用特有行为。
- 引入项目自有的 Conversation、Turn、Composer、StatusBar 配置/组合层。
- 新增 Maya 动画宿主适配器，将渲染端动画请求转换到 `my_agent` 自有的
  `RepaintClock` 和事件循环。
- 禁止在业务代码中使用 Maya umbrella include 和 `maya::run()`；唯一的 app 层
  include 限制在动画宿主适配器。
- 修复正常终端高度下对话正文丢失、无效工具状态仍允许粘贴等问题，
  并补充画布与 UI loop 回归测试。
- Maya 现以 Git submodule 形式位于 `my_agent/maya`，固定到
  `d3d53262ce6888ff782e130fe174a7bd5da9fc58`。CMake 优先使用本地子模块，未初始化
  时仍可通过 FetchContent 获取同一提交。

### 2.3 工具边界与路径策略

- `ReadRoots` 将 `/tmp` 读取权限变为显式会话策略；生产默认行为保持不变，
  边界测试不再偶然依赖构建目录是否位于 `/tmp`。
- 相关读取、目录列举、skill 和会话服务测试已覆盖该策略。

### 2.4 clangd 与开发环境

- CMake 始终导出 `build/compile_commands.json`，仓库 `.clangd` 指向该编译数据库。
- 已在仓库内重建标准 `build/` 配置，代码提示不再依赖失效的旧路径。
- VS Code 工作区显式使用 `/usr/bin/clangd-22`；系统默认 clangd 14 无法正确解析
  GCC 15 的 C++26 标准库。
- clangd 22 已从编译数据库读取真实 include/标准参数，对生产翻译单元的
  严格检查返回 0。

## 3. RAG 保留与验证

本轮没有重写 RAG 主干。以下能力均已保留并通过测试：

- BM25 词法检索。
- dense embedding 与不可用时的 BM25-only 降级。
- RRF 融合。
- 独立 local rerank 与 TEI rerank。
- rerank/嵌入失败回退。
- `search_docs` 工具与逐题 judgments 评测报告。
- 增量语料缓存与嵌入身份隔离。

当前评测数据：

- 单题 judgments：retrieval 和 local rerank 均为 `hit@1=1.000`。
- 三题 fixture：retrieval 和 local rerank 均为 `hit@1=0.333`，rerank 未产生改善。
- 项目文档合成评测：2075 个查询，BM25 `recall@5=0.991`，`MRR=0.955`。
- 评测时 dense 服务不可用，因此不能据此评价 dense、RRF 或 TEI 的实际收益。
- dense 请求失败时不覆盖磁盘上一份有效向量缓存，连续 cache miss 是已有
  保护契约，不是恢复缺陷。

## 4. 最终验证基线

- 仓库内标准 `build/` 从头配置成功。
- 完整构建成功，包含核心库、RAG、provider、TUI、REPL、benchmark 与全部测试
  目标。
- Agent/compaction/tool-loop 聚焦测试：95/95 通过。
- CTest 目录共 543 项；542 项非 socket 基线返回 0，本地 HTTP socket 超时测试
  单独运行通过。
- 外部 Ollama、TEI、TLS 等服务型用例在服务不可用时按既有规则跳过。
- RAG 测试基线：86/86。
- `git diff --check` 通过；工作树中没有 bench 缓存或构建产物进入版本控制。

编译仅保留固定上游版本在 GCC 15 下的已知警告：`nlohmann_json` 弃用提示与
Maya 内部格式警告，没有项目编译错误。

## 5. 架构结论

- `my_agent` 拥有自己的 reducer、异步宿主和终端事件循环；Maya 只提供渲染原语。
- 应用专属行为留在 `include/my_agent/ui` 和 `src/ui` 适配层，不再维护 Maya 源码
  patch。
- 运行时内部转发层仅在它真正承担边界、不变式或政策时保留；简单 ID
  包装不再抽象为独立函数。
- RAG 下一步不应在评测数据不足时盲目调权或替换算法。

## 6. 建议的下一切片

下一个 RAG TDD 切片应先提高评测可信度：

1. 建立代表实际使用的中英文 judgments，覆盖精确查找、过程型问题、歧义问题与
   不可回答问题。
2. 在可用的 dense/TEI 环境中分别记录 retrieval、local rerank 和 TEI rerank 的
   逐题排名。
3. 依据 Red 数据决定优先改进 tokenizer、候选集、RRF 权重还是 reranker，不预设
   算法结论。
