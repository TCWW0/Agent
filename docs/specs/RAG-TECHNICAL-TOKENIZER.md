# RAG Technical Tokenizer

状态：M0 证据已冻结；M1 legacy module 提取与等价验收完成；M2-A identifier-parts Green、
资源审计、identifier 专项集及 live Nomic Dense/Hybrid 控制均已完成并通过。下一切片为
M2-B resource guard，尚未建立 Red。

## 1. 为什么先建立 module

Tokenizer 同时决定 BM25 建库、BM25 查询、质量 bench 的合成查询，以及 local rerank
的 lexical coverage。它应当是一个 in-process、纯计算、无 I/O 的深 module：调用方只学习
一个稳定 interface，规则、UTF-8 扫描和后续技术标识符解析保持在 implementation 内。

当前只有一个生产策略，因此不增加抽象基类、运行时 port、配置对象或 adapter。出现第二个
真实生产策略之前，这些 seam 都是假设性的。

### 1.1 领域适配的层级

Lexical tokenizer 是检索模型的一部分，不是与语料无关的通用预处理。自然语言文档通常把
标点视为边界；技术文档中的 `_`、`::`、`-`、版本点号和 flag 前缀却可能承载 identity。
因此技术 RAG 可以按领域语法定制 tokenizer，但应分层控制：

1. 通用正确性：确定性、大小写、UTF-8 容错、顺序与重复语义；
2. 领域语法：identifier、namespace、version、flag、path；
3. 语料特例：stopwords、synonyms 或专有词表。

当前只推进前两层。第三层必须有真实 corpus evidence 后作为独立实验，不能混入技术语法。
BM25 的索引与查询必须共享同一 analyzer；Dense model 自带的 subword tokenizer 是另一条
表示链，不应与这里的 lexical tokenizer 混为一谈。

## 2. Interface 与版本身份

权威 interface 位于 `include/my_agent/rag/tokenizer.hpp`：

```cpp
[[nodiscard]] std::vector<std::string> tokenize(std::string_view text);
[[nodiscard]] std::string_view tokenizer_identity() noexcept;
```

所有调用方必须通过这个 interface；BM25、rerank 或 bench 不得拥有私有 token 规则。
`tokenizer_identity()` 标识可观察行为。任何可能改变 token 的规则都必须更换 identity，
并写入质量报告。M1 identity 为 `tokenizer-legacy-v1`。

## 3. Legacy V1 contract（M1）

M1 只移动 seam，不改变行为：

- token 按源文本顺序返回，重复 token 保留；
- ASCII alphanumeric run 转为小写，长度小于 2 的 run 丢弃；
- ASCII 标点和空白是分隔符；
- 连续非 ASCII 字节序列按推断的 UTF-8 codepoint 切片；单个切片原样保留，多个切片产生
  重叠 bigram；当前并不识别具体 Unicode script；
- 非法或截断 UTF-8 不抛异常，并保证向前推进；截断尾部按一个原始字节片段保留。输出未
  承诺为合法 UTF-8；
- 不做 stemming、stopword removal、Unicode normalization 或完整 case folding；
- 当前没有最大 token 长度。为了 exact equivalence，M1 不提前加入 guard；V2 引入 guard
  时必须更换 identity；
- 相同字节输入在同一实现上得到确定性相同的 token 序列。

Legacy characterization examples：

| 输入 | Legacy V1 输出 | 已知限制 |
|---|---|---|
| `The Quick-Brown FOX` | `the, quick, brown, fox` | — |
| `a I am` | `am` | ASCII 单字符丢弃 |
| `检索引擎` | `检索, 索引, 引擎` | 非 ASCII 一律视为同类 run |
| `配置BM25参数` | `配置, bm25, 参数` | 只因 ASCII/non-ASCII 切换而分开 |
| `C++` | 空 | 技术语言名丢失 |
| `v1.2` | `v1` | 版本 identity 丢失 |
| `MY_AGENT_DOCS_DIR` | `my, agent, docs, dir` | 没有 whole identity |
| `HybridRrfWeights` | `hybridrrfweights` | 没有 camel/acronym parts |
| `std::string_view` | `std, string, view` | 没有 whole identity |
| `café` | `caf, é` | Latin 被 ASCII 边界拆开 |

## 4. M0 冻结证据

正式 baseline `results/rag_quality/before-tokenizer-optimization.json` 保持不覆盖：

- SHA-256：`d94096fa0a228b97d5acdd49aac8e728ff9f07ae9e298fee91ffb026b839748f`；
- 数据：30 cases，10,905 chunks，candidate K = 16；
- BM25 Hit@1 `13/30`，Hit@4 `17/30`，Recall@10 `21/30`，Recall@16 `23/30`；
- BM25 MRR@10 `0.5075396825`，nDCG@10 `0.5527443260`；
- 30 条逐查询 rank 保存在 baseline 的 `cases` 数组；
- legacy token examples 由 `tests/rag_tokenizer_test.cpp` 固定。

提取前的等权本地复现报告为
`/tmp/my-agent-tokenizer-m0-pre-module-2026-11-08.json`。它只用于 M1 exact-equivalence，
不替代正式 baseline。

词表大小、平均 tokens/chunk、postings 数和查询延迟尚未形成稳定测量口径；M2 改变行为前
补入同一量具，避免把不同采样方法产生的数字当成 tokenizer 差异。

## 5. M1 验收

- implementation 从 `bm25.cpp` 移到独立 `tokenizer.cpp`；
- `rag.hpp` 不再暴露 tokenizer；调用方显式依赖专用 header；
- BM25 build/query、bench 和 rerank 使用同一个 interface；
- tokenizer contract tests 位于独立测试文件，不在 `rag_test.cpp` 重复；
- 质量报告记录 `dataset.tokenizer_identity`；
- characterization tests 全部通过；
- 提取前后 30 条 BM25、retrieval、local-rerank rank 逐条相同，汇总质量指标相同；
- chunk count 不变。Dense-only 不受 tokenizer module 提取影响；M1 的无模型复现不据此声称
  已验证 live Dense。

2026-11-08 验收结果：

- `Tokenizer.*:Bm25.*:Rerank.*`：18/18 通过；
- 完整 `rag_test`：82/85 通过，仅保留 tokenizer 工作前已存在的 3 个 Weighted RRF
  默认权重断言失败；
- M1 报告：`/tmp/my-agent-tokenizer-m1-direct-2026-11-08.json`；
- 30 条查询在 BM25-only、retrieval、local-rerank 三阶段均为 30/30 unchanged；
- Hit@1/4、Recall@10/16、MRR@10、nDCG@10 的版本间 delta 全部为 `0`；
- chunk count 保持 `10,905`，报告记录 `tokenizer-legacy-v1`；
- 本轮 `dense_queries == 0` 且 `hybrid_queries == 0`，因此验收结论只覆盖 legacy tokenizer
  提取的词法与 BM25-fallback 路径，不扩张为 live Dense/Hybrid 结论。

## 6. Technical Tokenizer V2 范围

M2 以失败测试驱动，并将下列行为作为单一 tokenizer 变量：

- whole + parts：snake_case、kebab-case、camelCase、PascalCase、acronym；
- namespace、路径、flags、版本、protocol、error code；
- `C++`、`C#`、`-j2` 等有意义的单字符技术 token；
- 最大 token 长度与 token explosion guard。

M3 再单独处理 UTF-8 decode、script classification 和 CJK run。V2 第一轮明确不包含 Porter
stemming、完整 stopword list、NFKC、完整 Unicode case folding、ICU/utf8proc 依赖，也不
改变 chunking、embedding、candidate K、fusion 或 rerank 权重。

### Agentty 参考取舍

参考实现：

- `agentty/rag-cpp/include/rag/text/tokenizer.hpp`
- `agentty/rag-cpp/src/text/tokenizer.cpp`
- `agentty/rag-cpp/include/rag/lexical/bm25.hpp`
- `agentty/rag-cpp/src/lexical/bm25.cpp`

M2 借鉴其 implementation 形状：单次扫描只收集 candidate，大小写、最小/最大长度、过滤与
派生 token 都集中在一个 flush policy；返回 token vector 的纯、确定性 interface；索引与
查询共用同一分析链。M2-B 的 `max_len` 也应进入这一 flush policy，而不是散落到调用方。

当前不复制 Agentty 的 `Tokenizer` class/options、English stopwords、Porter stemmer、stemming
cache 与按字节 `std::isalnum` 规则：`my_agent` 只有一个真实生产策略，提前暴露配置会扩大
interface；其默认 English 语义和非 ASCII 行为也不适合当前中英文技术语料。

Agentty 还通过 BM25 postings 计算 rerank term coverage，避免重新 tokenization passage。这是
后续可以单独评估的 locality/性能改进，不与 M2-A 标识符语义同时修改。

### 6.1 M2-A：复合标识符

本切片同时覆盖 snake_case、kebab-case、camel/Pascal、acronym 与 namespace，避免为每种
拼写建立零碎测试。最终输出契约为：保留 Legacy V1 已经提供的 identity token，再按源顺序
增加 semantic parts；ordinary prose 不跨空格组合，不为 `_`、`-`、`::` 额外制造 candidate
whole。

独立 camel/Pascal candidate 的规范化 whole 是 Legacy V1 已有 identity，因此继续保留；
snake、kebab、namespace 的 Legacy identity 是结构分隔符两侧的 segment。Title-Case prose
（例如既有 contract 中的 `Quick-Brown`）继续把 `-` 当普通分隔符。

代表性输出：

| 输入 | M2-A 输出 |
|---|---|
| `MY_AGENT_DOCS_DIR` | `my, agent, docs, dir` |
| `HybridRrfWeights` | `hybridrrfweights, hybrid, rrf, weights` |
| `getHTTPResponseCode` | `gethttpresponsecode, get, http, response, code` |
| `HTTPServerError` | `httpservererror, http, server, error` |
| `std::string_view` | `std, string, view` |
| `IconName::AlertCircle` | `iconname, icon, name, alertcircle, alert, circle` |
| `retrieval-cache` | `retrieval, cache` |

结构分隔符内部的 camel/Pascal segment 还要保留 Legacy V1 已经产生的 segment whole。
例如 `IconName::AlertCircle` 在新增 semantic parts 的同时，不能丢掉旧检索词 `iconname`、
`alertcircle`。segment whole 只在该 segment 确实发生大小写拆分时显式补回；普通
`std::string_view` 不因此重复产生 `std`、`string`、`view`。

本切片最终 identity 为 `technical-tokenizer-v2-identifier-parts`。`C++`、`C#`、flags、versions、
protocols、paths、error codes 与 max-length guard 留给 M2-B，不在 M2-A Green 中顺手实现。

M2-A implementation 不变量：

- 单次线性扫描定位 ASCII candidate span，不复制 candidate；
- structural/camel 分析只产生指向输入的 `string_view` parts，最终输出时才分配字符串；
- 独立 camel/Pascal candidate 保留一个 Legacy whole；结构化 candidate 不新增 full whole；
- 结构分隔符内部发生 camel 拆分时，按 `segment whole → semantic parts` 保留层次；
- 不排序、不去重，继续保留重复 token 契约；
- scanner 不决定 whole/parts policy，所有 ASCII 输出集中在一个 candidate emission 函数；
- 非 ASCII run 与 legacy bigram 在本切片逐字节保持不变；
- public interface 不增加 helper、options 或中间分析类型。

M2-A 验收顺序：先让 tokenizer Red 变绿；再增加一个 camel identifier → BM25 partial-query
tracer；随后记录 vocabulary、平均 doc length、postings 和 BM25-only rank 变化。在本切片
完成前不横向增加 flags/path/max-length 测试。

whole+parts Green、兼容性修正与收缩 ablation 的决策证据：

- tokenizer/BM25/rerank 定向回归 `19/19` 通过，camel identifier tracer 可由 `rrf weights`
  命中只含 `HybridRrfWeights` 的目标文档；完整词表审计为 `legacy-only=0`；
- 完整 candidate whole 方案在 10,905 chunks 上把 vocabulary 从 `11,185` 增至 `17,274`
  （`+54.44%`），平均 doc length 从 `85.7617` 增至 `92.3259`（`+7.65%`），postings
  从 `407,520` 增至 `443,438`（`+8.81%`）；
- 9-case identifier 专项集位于
  `eval/agentty_docs_4205c612/judgments/identifiers.passages.json`。完整方案相对 Legacy 为
  6 improved / 0 degraded / 3 unchanged，Hit@1 `0.3333 → 0.7778`，Recall@16
  `0.6667 → 1.0000`；
- 但去掉 `_`、`-`、`::` structured whole 的 ablation 得到完全相同的专项 ranks，同时
  vocabulary 仅为 `11,300`（相对 Legacy `+1.03%`）、平均 doc length `87.2189`
  （`+1.70%`）、postings `413,660`（`+1.51%`）；
- 30-query BM25 主集在收缩方案下 Hit@1/4、Recall@10/16 均与 Legacy 相同，MRR@10 与
  nDCG@10 各提高约 `0.0006`；完整方案则有 1 improved / 2 degraded，并使 Hit@4 下降；
- 使用 `nomic-embed-text:latest`、K=16、等权 RRF 的 live 控制中，Dense-only 30/30 ranks
  不变。收缩方案的 Hybrid 30/30 ranks 不变；完整方案为 2 improved / 2 degraded，并让
  一个相关 passage 从第 16 名掉出候选集；
- 因此拒绝 structured candidate whole，保留收益来源——standalone camel parts 与 nested
  segment 中间层。最终生产 Green 已复现 ablation：定向回归 `19/19`，完整回归 `83/86`
  （仅既有 3 个 Weighted RRF 断言失败），资源数字、9-case 专项 ranks、Dense 30/30 与
  Hybrid 30/30 unchanged 均一致。正式报告位于 `results/rag_quality/` 下的
  `tokenizer-m1-identifiers-focused-baseline.json`、`tokenizer-m2a-final-main.json`、
  `tokenizer-m2a-final-focused.json` 与 `tokenizer-m2a-final-nomic.json`。

## 7. 测试矩阵

| 层级 | 正例 | 负例/守卫 | 目的 |
|---|---|---|---|
| Legacy characterization | ASCII、数字、CJK bigram、混合输入 | 空串、标点、非法 UTF-8 | 证明 M1 无行为变化 |
| Technical identifiers | whole+parts、namespace、path、flag、version | 普通 prose 不过度组合 | 证明技术 identity 可检索 |
| Resource guards | 合理长标识符 | hash/base64/minified、超长 run | 控制词表与 doc length |
| Unicode-lite | Han、Latin、Cyrillic、emoji 分段 | 截断/非法 UTF-8、跨 script | 不产生跨 script bigram |
| Retrieval | synthetic exact-token cases | 原 rank-1 regression | 将 token 变化追到 BM25 rank |
| Quality ladder | 30-query main set | Dense/chunk count 控制 | 做单变量整体评估 |

## 8. 实验变量与拒绝条件

比较 Legacy V1 与 Technical V2 时，只改变 tokenizer identity。固定语料、judgments、chunker、
embedding model/recipe、candidate K、RRF k/weights 和 rerank 参数。先看 BM25-only，再验证
Dense-only 逐条不变，最后观察 Hybrid 与 local rerank 的传播。

以下任一项需要拒绝或收缩规则：

- 新技术 case 没有可解释的 token/rank 收益；
- 既有 rank-1 case 丢失且无法逐条解释；
- Dense-only rank 或 chunk count 改变；
- vocabulary、平均 doc length、postings、build/query latency 显著膨胀；
- ordinary prose 产生大量 whole+parts token；
- 无法从变化的 BM25/rerank evidence 追溯到具体 token；
- 用当前全英文 30-query main set 对 CJK 质量作泛化结论。
