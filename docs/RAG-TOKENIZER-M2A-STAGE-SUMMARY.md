# RAG Tokenizer M2-A 阶段总结

日期：2026-11-08
状态：M0、M1、M2-A 已完成；下一阶段从 M2-B resource guard 开始。

## 1. 本阶段完成了什么

Tokenizer 已从 BM25 implementation 中提取为独立深模块。生产调用方只依赖：

```cpp
std::vector<std::string> tokenize(std::string_view text);
std::string_view tokenizer_identity() noexcept;
```

BM25 建库、BM25 查询、bench 合成查询和 local rerank 共用这一入口。当前行为身份为：

```text
technical-tokenizer-v2-identifier-parts
```

M2-A 在保留 Legacy V1 ASCII、CJK bigram、顺序和重复语义的基础上，增加 camel/Pascal、
acronym 以及结构分隔符内部 camel segment 的 semantic parts。

## 2. 最终行为

| 输入 | 最终输出 |
|---|---|
| `MY_AGENT_DOCS_DIR` | `my, agent, docs, dir` |
| `HybridRrfWeights` | `hybridrrfweights, hybrid, rrf, weights` |
| `getHTTPResponseCode` | `gethttpresponsecode, get, http, response, code` |
| `HTTPServerError` | `httpservererror, http, server, error` |
| `std::string_view` | `std, string, view` |
| `IconName::AlertCircle` | `iconname, icon, name, alertcircle, alert, circle` |
| `retrieval-cache` | `retrieval, cache` |

独立 camel/Pascal candidate 保留 Legacy whole，并增加 semantic parts。snake、kebab、namespace
不新增完整 structured whole；如果其内部 segment 又发生 camel 拆分，则保留 Legacy segment
whole，再增加细粒度 parts。

## 3. 为什么拒绝 structured whole

初版 M2-A 为 `_`、`-`、`::` candidate 同时生成完整 whole 与 parts。这个设计看似同时支持
精确和部分匹配，但 `tokenize()` 返回的是扁平 token 序列，BM25 不知道这些 token 是同一个
原始 identifier 的替代表达。

未被 query 命中的 structured whole 不贡献分数，却会增加文档长度。identifier 密集的配置
文档增长得比全库平均值更快，从而受到更强的 BM25 length-normalization 惩罚。route rank 的
小幅变化又会在 RRF top-K 边界被放大，曾使一个相关 passage 从 Hybrid 第 16 名掉出候选集。

Ablation 表明收益来自 semantic parts 和 nested segment 中间层，而不是 structured whole：

| 方案 | Vocabulary | Avg doc length | Postings |
|---|---:|---:|---:|
| Legacy V1 | 11,185 | 85.7617 | 407,520 |
| 完整 whole+parts | 17,274（+54.44%） | 92.3259（+7.65%） | 443,438（+8.81%） |
| 最终 identifier-parts | 11,300（+1.03%） | 87.2189（+1.70%） | 413,660（+1.51%） |

最终方案保留了专项质量收益，同时移除了完整方案的词表膨胀和 Hybrid 召回回退。

## 4. 验收证据

- `Tokenizer.*:Bm25.*:Rerank.*`：19/19 通过；
- 完整 `rag_test`：83/86 通过；剩余 3 个是 tokenizer 工作前已存在的 Weighted RRF 默认权重
  断言失败；
- 10,905 chunks 的完整词表审计：`legacy-only=0`；
- 9-case identifier 专项集：6 improved / 0 degraded / 3 unchanged；Hit@1 从 0.3333 提升到
  0.7778，Recall@16 从 0.6667 提升到 1.0000；
- 30-query BM25 主集：Hit@1/4、Recall@10/16 与 Legacy 相同，MRR@10、nDCG@10 各提高约
  0.0006；
- `nomic-embed-text:latest`、K=16、等权 RRF live 控制：Dense-only 30/30 ranks unchanged，
  Hybrid 30/30 ranks unchanged；local rerank MRR@10 提高 0.0036、nDCG@10 提高 0.0031；
- chunk count 保持 10,905。

专项 judgments 位于：

```text
eval/agentty_docs_4205c612/judgments/identifiers.passages.json
```

正式报告位于：

```text
results/rag_quality/tokenizer-m1-identifiers-focused-baseline.json
results/rag_quality/tokenizer-m2a-final-main.json
results/rag_quality/tokenizer-m2a-final-focused.json
results/rag_quality/tokenizer-m2a-final-nomic.json
```

## 5. 验证命令

```bash
cmake --build build --target rag_test rag_quality_ladder -j2
./build/rag_test --gtest_filter='Tokenizer.*:Bm25.*:Rerank.*'
./build/rag_quality_ladder \
  eval/agentty_docs_4205c612/docs \
  eval/agentty_docs_4205c612/judgments/identifiers.passages.json \
  16
```

live Nomic 路径使用宿主机 Ollama 默认端口 11434：

```bash
MY_AGENT_EMBED_MODEL=nomic-embed-text:latest \
MY_AGENT_OLLAMA_HOST=127.0.0.1 \
MY_AGENT_OLLAMA_PORT=11434 \
./build/rag_quality_ladder \
  eval/agentty_docs_4205c612/docs \
  eval/agentty_docs_4205c612/judgments/main.passages.json \
  16
```

## 6. 下一阶段边界

M2-B 先建立 resource guard Red，再扩展 symbolic technical forms。优先考虑 max token length 与
单 candidate 派生 token 数上限；随后再分别评估 `C++`、`C#`、flags、versions、protocols、
paths 和 error codes。不要在没有资源上限时继续扩大 scanner grammar。

更丰富的 token positions、overlap/synonym token、identifier exact field 或 BM25F 属于后续
analyzer/index 设计，不在 M2-B 顺手加入。
