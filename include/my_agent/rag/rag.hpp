#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace my_agent::rag {

struct Chunk {
    std::string path;
    int line_start = 0;
    int line_end   = 0;
    std::string text;

    std::string context;
};

struct Bm25Index {
    struct Posting { std::uint32_t chunk_id; std::uint32_t tf; };
    std::vector<std::vector<Posting>> postings;
    std::vector<std::uint32_t>        doc_len;
    double avg_doc_len = 0.0;
    std::size_t doc_count = 0;
    std::unordered_map<std::string, std::uint32_t> term_ids;
};

[[nodiscard]] Bm25Index build_bm25(const std::vector<Chunk>& chunks);

[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
bm25_search(const Bm25Index& idx, std::string_view query, std::size_t k);

[[nodiscard]] std::vector<Chunk>
chunk_document(const std::string& path, const std::string& body,
               std::size_t max_lines = 40, std::size_t max_chars = 1600,
               std::size_t overlap_lines = 4);

enum class EmbedRole { Doc, Query };

struct EmbedConfig {
    std::string host = "localhost";
    int port = 11434;
    std::string model;
    int timeout_ms = 30000;
};

using EmbedBackend = std::function<
    std::expected<std::vector<std::vector<float>>, std::string>(
        const EmbedConfig&, const std::vector<std::string>&, EmbedRole)>;

inline constexpr std::size_t kEmbedBatch = 64;

inline constexpr double kRrfK = 60.0;

struct DenseIndex {
    std::vector<std::vector<float>> vecs;  // [chunk_id] → 模型维度（nomic 768）
};

// cosine 相似度 dot/(|a||b|)：完整归一，不假设输入已单位化（真实 Ollama
// 出厂归一，但接缝要容纳任意假向量）。维度不齐或空向量 → 0.0：
// 维度不齐 = 状态已腐（换模型后查询与库不对齐），0 = 零相似、该候选
// 退出竞争 —— 宁可少一个候选，绝不拿错位向量算分。
[[nodiscard]] double cosine_sim(const std::vector<float>& a,
                                const std::vector<float>& b);

// 从打分结果剥离分数 → 纯名次 id 列表（rrf_fuse 的输入形状）。
// bm25_search 的返回契约不变 —— 分数对 RRF 是冗余，对 bench 是资产。
[[nodiscard]] std::vector<std::uint32_t>
ranked_ids(const std::vector<std::pair<std::uint32_t, double>>& scored);

// RRF 名次融合：ranked_lists 内每个列表已按名次排好（第 0 位 = 第 1 名）。
// 融合分 = Σ 1/(k + rank_1based)；只消费名次不消费分数；同分按
// chunk-id 升序（确定性）；截断至 out_k；零命中返回空。
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
rrf_fuse(const std::vector<std::vector<std::uint32_t>>& ranked_lists,
         double k, std::size_t out_k);

// 建库：整批 chunk 向量化（64/批，Doc 角色）。喂给 backend 的输入串是
// 带面包屑前缀的正文：context + '\n' + text（context 空则裸 text；教材
// embed_input() 同款，contextual embeddings）。任一批失败 → 整体 err
// （不做半截 dense —— 那是语料中段的静默质量悬崖）；各向量维度必须
// 一致（含跨批）。err 串给人看，调用方降级 BM25-only。
[[nodiscard]] std::expected<DenseIndex, std::string>
embed_corpus(const std::vector<Chunk>& chunks,
             const EmbedBackend& backend, const EmbedConfig& cfg);

enum class SearchMode { Hybrid, Bm25Only };

struct RankedScore {
    double score = 0.0;
    std::size_t rank = 0; // 1-based
};

struct Candidate {
    std::uint32_t chunk_id = 0;
    double final_score = 0.0;
    std::optional<RankedScore> sparse;
    std::optional<RankedScore> dense;
};

struct CandidateSet {
    std::vector<Candidate> candidates;
    SearchMode mode = SearchMode::Bm25Only;
};

struct RerankEvidence {
    double lexical_coverage = 0.0;
    std::optional<double> model_score;
    double final_score = 0.0;
};

struct Provenance {
    std::string path;
    int line_start = 0;
    int line_end = 0;
    std::string context;
};

struct RankedPassage {
    Provenance provenance;
    std::string text;
    double score = 0.0;
    std::optional<RankedScore> sparse;
    std::optional<RankedScore> dense;
    std::optional<RerankEvidence> rerank;
};

struct RetrievalResult {
    std::vector<RankedPassage> passages;
    SearchMode mode = SearchMode::Bm25Only;
    std::size_t corpus_chunk_count = 0;
};

enum class RerankMode {
    Skipped,
    LocalOnly,
    ModelEnhanced,
    ModelFallback,
};

struct RerankReport {
    RerankMode mode = RerankMode::Skipped;
    std::string backend_error;
};

struct RerankResult {
    RetrievalResult retrieval;
    RerankReport report;
};

using RerankBackend = std::function<std::expected<std::vector<double>, std::string>(
                        std::string_view query, const std::vector<std::string>& passages)>;

struct TeiRerankConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
    int timeout_ms = 30000;
};

using TeiRerankTransport = std::function<std::expected<std::string, std::string>(
    const TeiRerankConfig&, std::string_view request_body)>;

[[nodiscard]] RerankBackend
make_tei_rerank_backend(TeiRerankConfig config, TeiRerankTransport transport = {});

[[nodiscard]] RerankResult
rerank_passages(std::string_view query, RetrievalResult result, const RerankBackend& backend = {});

[[nodiscard]] CandidateSet
retrieve_candidates(const Bm25Index& bm25, const DenseIndex& dense,
                    std::string_view query, const EmbedBackend& backend,
                    const EmbedConfig& cfg, std::size_t k);

[[nodiscard]] std::string
build_tei_rerank_request_body(std::string_view query, const std::vector<std::string>& passages);

[[nodiscard]] std::expected<std::vector<double>, std::string>
parse_tei_rerank_response(std::string_view body, std::size_t expected_count);

// ── Ollama /api/embed 真实后端（组装处使用；测试注入假 backend）─────────
//
// 以下三个纯函数是给 Red 测试咬的接缝，也是「换 HTTP 客户端」的公共
// 积木：拼包/解析/方言与 HTTP 胶水彻底分离，网络路径的坑离线钉死。

// 真实后端：方言前缀 → 拼包 → POST /api/embed → 解析 + 数量守卫。
// 同步阻塞，由调用方决定放哪个线程（与 HttpClient 的约定一致）。
[[nodiscard]] std::expected<std::vector<std::vector<float>>, std::string>
ollama_embed(const EmbedConfig& cfg, const std::vector<std::string>& texts,
             EmbedRole role);

// 模型方言（纯函数）：按模型名分派 doc/query 前缀 —— nomic 系
// "search_document: "/"search_query: "，e5 系 "passage: "/"query: "，
// 其余不加（错误前缀会伤害别的模型）。匹配先小写化。
// TODO: 其实这里可以看做是针对于特定模型族的优化，可以自行测试效果
[[nodiscard]] std::string
embed_input_text(std::string_view model, EmbedRole role, std::string_view text);

// 拼包（纯函数）：构造 /api/embed 的 JSON 请求体（model + input 数组，
// 与输入顺序对齐）。
[[nodiscard]] std::string
build_embed_request_body(std::string_view model,
                         const std::vector<std::string>& texts);

// 解析（纯函数）：响应体 → 向量批。兼容旧版单数 "embedding" 形状（仅当
// expected_count == 1 合法）。err 条件：垃圾 JSON、缺字段、非数组、
// 行非数组、非数字元素、空行、行长不齐、数量 != expected_count（数量
// 失守会让 chunk-id↔向量对位静默错乱 —— 这是最危险的一种失败）。
[[nodiscard]] std::expected<std::vector<std::vector<float>>, std::string>
parse_embed_response(std::string_view body, std::size_t expected_count);

// ── Corpus：目录建库 + 增量缓存（切片 #42）────────────────────────────────
//
// build 是「重放目录」语义：walk 是唯一事实，缓存只决定每个文件「复用还是
// 重算」，绝不改变最终语料内容 —— 删除的文件 walk 看不见，块自然出局，
// 写回缓存时随之消失。
//
// 缓存身份分两层，失效半径不同（#42 设计核心）：
//   • chunker 身份：切块语义变了 → 缓存里的块本身就是错的 → 整体当 miss；
//   • embed 身份：模型/方言/输入拼法变了 → 块文本仍正确（BM25 侧与
//     embed 无关）→ 保块弃向量，全量重嵌（教材 kCacheMagic v5 的语义）。
//
// 错误处理三条（#42 拍板）：
//   • 缓存损坏/版本不符 → 静默整丢重建。解析与策略分离（纯解析层产出
//     「解析到哪了」+ 状态码，策略在 build 里一行决断），将来若升级
//     半截保留，只动那一行；
//   • embed 失败 → 降级 BM25-only（合法状态而非错误），且不写缓存 ——
//     旧缓存原样保留，环境恢复后直接命中；
//   • 写缓存走 tmp + rename 原子替换（写一半崩溃不会留下半截文件）。

// 切块语义身份：任何改动 chunk_document 行为（边界、overlap、面包屑
// 组成、默认参数……）的人必须修改这个串 —— 否则旧缓存里的块就是新
// 代码下的错块，而 (size, mtime) 指纹对此完全无感。
inline constexpr std::string_view kChunkerIdentity = "chunker-v1";

// embed 输入串组成身份：context+'\n'+text 拼法（context 空则裸 text）。
// 改拼法就得 bump —— 拼法变了，同一文本产出不同向量，旧向量不可比。
inline constexpr std::string_view kEmbedInputRecipe = "ctx_nl_text-v1";

// 缓存文件名（落在 root 下）。测试篡改/删缓存用 —— 单一事实来源，
// 别处不要复写这个字符串。
inline constexpr std::string_view kCorpusCacheName = ".my_agent_rag_cache.bin";

// embed 身份串（纯函数）：model + '\x1f' + doc 前缀串 + '\x1f' + 输入拼法
// 版本。前缀串直接取 embed_input_text(model, Doc, "") 的输出 —— 方言
// 映射改了身份自动跟着变（单一事实来源，不搞第二份风格枚举）。
// model 为空 → 空串：BM25-only 会话没有 embed 身份，缓存身份由
// build 的「身份穿透」语义保护（沿用 cached 身份落盘，不被空 model 抹掉）。
[[nodiscard]] std::string embed_identity(std::string_view model);

class Corpus {
public:
    // 建库账本：bench 的验收数字直接来自这里（第二次全量跑应为
    // reused=全量、chunks_embedded=0、build 秒级）。
    struct BuildStats {
        std::size_t files_seen      = 0;  // walk 到的 .md 文件数
        std::size_t files_reused    = 0;  // (size,mtime) 指纹命中：块来自缓存
        std::size_t files_rechunked = 0;  // 新增/变更/缓存失效 → 重切
        std::size_t chunks_embedded = 0;  // 本次成功嵌入并进库的块数（失败=0）
        bool        dense_ok        = false;  // dense 路可用（全库向量对位成功）
    };

    // 目录建库：递归 walk root（只收 .md，跳过点开头目录）→ (size,mtime)
    // 指纹比对 → 变更部分重切/重嵌 → 原子落盘缓存。目录不存在/为空 →
    // 空 corpus（合法状态，不报错）。embed 失败 → 降级 BM25-only，
    // dense_ok=false 且本次不写缓存。块路径一律记相对 root 的路径。
    BuildStats build(const std::filesystem::path& root,
                     const EmbedBackend& backend, const EmbedConfig& cfg);

    // 内存建库：无目录、无缓存（测试与非目录场景）。docs 为 (路径, 正文)
    // 序列；重嵌无增量可言，语义上等价于「永远 miss 的 build」。
    BuildStats build_from_memory(
        const std::vector<std::pair<std::string, std::string>>& docs,
        const EmbedBackend& backend, const EmbedConfig& cfg);

    [[nodiscard]] std::size_t chunk_count() const;
    // 库中存在已对位的 dense 向量 —— 与本次会话是否配模型无关：
    // BM25-only 会话搬运缓存向量后仍为 true（查询侧配空模型时
    // retrieve_candidates 自己降级，与库里有没有向量是两回事）。
    [[nodiscard]] bool has_embeddings() const;
    [[nodiscard]] const std::vector<Chunk>& chunks() const;

    // 组装结果的只读视图：bench 量具拿既有检索原语直接打分，保持度量
    // 口径与历史基线逐位可比；search_docs 工具走 retrieve() 入口。
    [[nodiscard]] const Bm25Index& bm25() const;
    [[nodiscard]] const DenseIndex& dense() const;

    [[nodiscard]] RetrievalResult
    retrieve(std::string_view query, const EmbedBackend& backend,
            const EmbedConfig& cfg, std::size_t k) const;

private:
    std::filesystem::path root_;    // 空 = 内存建库（不读不写缓存）
    std::vector<Chunk> chunks_;
    Bm25Index bm25_;
    DenseIndex dense_;
    std::string embed_id_;          // 产出 dense_ 向量的身份；空 = 从未嵌入
    bool dense_ok_ = false;
};

} // namespace my_agent::rag
