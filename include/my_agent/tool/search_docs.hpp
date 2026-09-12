#pragma once
// search_docs 工具接缝（切片 #43，issue #43）。src/tool/ 下的 detail 头
// （read.hpp 等）测试够不着 —— CMake 只导出 include/ —— 所以这里放公共头。
//
// 契约分两层：
//   • 纯函数层（resolve_config / render_search_results）—— env 与文件系统
//     探针全部注入，Red 完全离线钉死；
//   • 状态层（execute_search_docs + DocsIndex）—— 懒建库、指纹漂移检测、
//     互斥，测试用临时目录 + 假 backend 离线驱动，不碰真实环境与网络。
//
// 教材对照：agentty/src/tool/mcp_tools_backends.cpp 的 AgenttyDocRetriever
// ——只借「进程级 Corpus + 互斥 + ensure_docs_index_locked_ 漂移检测」骨架
// （FRESHNESS 注释，392-411 行）；其 router/pipeline/rerank/CRAG/查询缓存
// 漏斗全部超出 PRD 范围，不抄。
//
// ★ 与教材的两处拍板差异（issue #43 设计讨论）：
//   ① MY_AGENT_EMBED_MODEL 空/未设 = BM25-only（教材默认 nomic）——
//      默认零网络惊喜，语义路显式开启；
//   ② 显式配置指向不存在的目录是配置错误（可操作报错），教材静默拿去
//      build 出空 corpus —— 用户明确配错了却零信号是坑。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "my_agent/rag/rag.hpp"
#include "my_agent/tool/tool.hpp"

namespace my_agent::tool::docs {

// 一次 resolve，一次用完 —— execute 不再碰环境，env 可变也不影响已解析
// 的这份配置。
struct DocsConfig {
    std::filesystem::path root;
    bool root_missing = false;
    rag::EmbedConfig embed;
};

// 纯函数：解析顺序 MY_AGENT_DOCS_DIR 非空 → 用它（存在性由探针判定）；
// 否则 {cwd}/docs 是目录 → 用它；否则 root 为空。getenv 语义同
// std::getenv（未设 = nullptr，设了空串 = ""）。env 设了空串视同未设。
// 端口非数字/越界 → 回落默认 11434（配置垃圾不该崩工具）。
[[nodiscard]] DocsConfig resolve_config(
    const std::function<const char*(const char*)>& getenv,
    const std::filesystem::path& cwd,
    const std::function<bool(const std::filesystem::path&)>& is_directory);

// 目录树指纹（FNV-1a/64，教材 docs_tree_fingerprint_ 同款思路）：
// (路径, size, mtime) 逐文件混合。门禁必须与 Corpus::build 的 walk 逐字
// 一致 —— 正则 .md 文件、跳过点开头目录。注意 build 只跳点目录不跳点
// 文件：.hidden.md 参与索引，指纹也必须收，否则改它检测不到漂移。
// 缓存文件不是 .md，天然不进指纹 —— build 写缓存不会自我触发「漂移」。
// 这份 walk 是 build 之外的第二份门禁副本，同步耦合由
// search_docs_tool_test 的指纹一节钉住：改门禁必改测试。
[[nodiscard]] std::uint64_t
docs_tree_fingerprint(const std::filesystem::path& root);

// 进程级索引状态。registry 闭包持有一份（函数局部 static，与 registry
// 同寿命）；execute_search_docs 显式收引用 —— 测试不必碰全局。
struct DocsIndex {
    // 串行化「指纹检查 + build + search」整段：并发第二个调用应该等
    // 索引，而不是各建各的。今天 agent 状态机本来就一次只派发一个工具，
    // 但那是三层之外的涌现性质 —— 锁把它兑换成局部不变式（教材同款）。
    std::mutex mu;
    rag::Corpus corpus;               // 懒建：首次调用才 build
    std::string indexed_root;         // 已建库的根（字符串比较）
    std::uint64_t fingerprint = 0;    // 已建库时的目录指纹
};

inline constexpr std::size_t kDefaultK = 5;
inline constexpr std::size_t kMaxK = 20;

// 纯渲染：检索结果 → 模型可读文本。每段（按名次序）：
//     {path}:{line_start}-{line_end} (score {分数，4 位小数})
//     {正文}
// 段间空行分隔。尾部 mode 行是量纲声明（分数不归一化，量纲靠它）：
//     mode: hybrid — {chunk 总数} chunks from {root}
//     mode: BM25-only (no embed model configured) — …
//     mode: BM25-only (embed unavailable) — …
// 「embed unavailable」= 配了模型但这次降级了（瞬断/建库失败）—— 模型
// 看到它该知道换个查询试试或怀疑环境。chunk 总数让模型在空语料时知道
// 零结果不可信。零命中 → 正文区只写 "no matching passages"，mode 行照出。
[[nodiscard]] std::string render_search_results(
    const rag::Corpus& corpus, const rag::SearchResult& result,
    const DocsConfig& cfg);

// 工具主体：参数校验（InvalidArgs）→ 配置检查（零配置/配错 →
// ExecutionFailed 可操作错误，不崩）→ lock → 根或指纹漂了才 build →
// search → render。backend 注入：生产传 rag::ollama_embed，测试传假件。
[[nodiscard]] ExecResult execute_search_docs(
    const nlohmann::json& args, const DocsConfig& cfg,
    const rag::EmbedBackend& backend, DocsIndex& index);

// registry 组装（胶水）：闭包持有进程级 DocsIndex，每次调用现场
// resolve_config —— env 是会话中可变的，配置不缓存。
[[nodiscard]] ToolDef make_search_docs_tool();

} // namespace my_agent::tool::docs
