#pragma once
// my_agent::rag — 文档 RAG 主干（学习线：复刻 agentty/src/rag/ 的主干四片）。
//
// 本头文件是接缝契约：#40 chunker + tokenize + BM25（词匹配路），
// #41 混合检索（embed 语义路 + RRF 融合，本切片新增「语义路」一节），
// Corpus 建库落盘是 #42，search_docs 工具接线是 #43。
//
// 接口纯净原则（#41 拍板）：本头文件只用 std —— http/nlohmann 是 rag 的
// 实现依赖，PRIVATE 链接、一个 include 都不泄漏到这里。rag 的消费者
// （测试、bench、#43 接线处）看到的接缝永远是纯类型。
//
// 设计要点：
//   • tokenize 是索引端与查询端共用的唯一分词入口 —— 同一个函数保证两边
//     词表一致，这是 BM25 正确性的前提。
//       - ASCII：小写字母数字连续段为一个 token；不足 2 字节的 token
//         丢弃（单字符无检索信号）。
//       - 非 ASCII：按 UTF-8 码点解码，连续段切重叠二元组（CJK bigram，
//         Lucene CJKBigramFilter 思路：无词典、对新词免疫、索引与查询
//         切法必然一致）。孤立单字段原样发出。
//   • BM25 常量：k1 = 1.5（tf 饱和），b = 0.75（长度归一）。
//   • chunker：行对齐切块。max_lines 是软上限（绝不在代码围栏中间断开），
//     max_chars 是硬上限（超长单行先按 UTF-8 边界强制分片再进切块循环）。

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
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

    // 面包屑（contextual retrieval）：块所在的文档 + 标题链，
    // 形如 "guide.md › 安装 › Linux"。参与 BM25 索引（标题 token 以
    // 3 份副本入袋 = 字段加权），但绝不作为正文展示。
    std::string context;
};

void tokenize(std::string_view s, std::vector<std::string>& out);

struct Bm25Index {
    struct Posting { std::uint32_t doc; std::uint32_t tf; };
    std::vector<std::vector<Posting>> postings;  // term-id → 出现名单
    std::vector<std::uint32_t>        doc_len;   // chunk-id → token 总数（含重复）
    double avg_doc_len = 0.0;
    std::size_t doc_count = 0;
    std::unordered_map<std::string, std::uint32_t> term_ids;  // 词串 → term-id
};

[[nodiscard]] Bm25Index build_bm25(const std::vector<Chunk>& chunks);

// 打分检索：按分数降序返回 (chunk-id, score)，截断至前 k；零交集的块
// 不出现在结果里；同分按 chunk-id 升序。
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
bm25_search(const Bm25Index& idx, std::string_view query, std::size_t k);

// 切块：把一份文档拆成有界的行对齐块。max_lines 软上限（不打断代码
// 围栏），max_chars 硬上限（超长行先按 UTF-8 边界分片），overlap_lines
// 让跨块边界的事实落进相邻两个块。
[[nodiscard]] std::vector<Chunk>
chunk_document(const std::string& path, const std::string& body,
               std::size_t max_lines = 40, std::size_t max_chars = 1600,
               std::size_t overlap_lines = 4);

// ── 语义路（切片 #41：混合检索 + RRF）────────────────────────────────────
//
// dense 路与 BM25 词法路互补：BM25 死穴是词法鸿沟（「部署」vs「上线」零
// token 交集 → 零分），dense 死穴是丢字面精确匹配。两路死穴互斥，故混合。
// 分数不可通约（BM25 无上界，cosine 困在 [-1,1]），名次才是共同货币 → RRF。

// 语义路的角色。instruction-tuned 模型（nomic / e5 系）对文档侧与查询侧
// 使用不同的输入前缀 —— 不对称是训练出来的，裸文本对会可测地劣化匹配。
// 角色必须穿过接缝，由 backend（懂模型方言的一方）翻译成前缀。
enum class EmbedRole { Doc, Query };

// embedding 端点配置。model 为空 = 语义路关闭（BM25-only）。
struct EmbedConfig {
    std::string host = "localhost";
    int port = 11434;
    std::string model;        // 如 "nomic-embed-text"
    int timeout_ms = 30000;   // socket 级超时：连接建立与每次读各自的等待
                              // 上限（非全请求墙钟，慢滴漏不触发）；建库
                              // 批量可放宽
};

// 语义路后端：一批文本 → 每条一个向量（与输入顺序对齐）。
// 失败语义：整批失败；err 是给人看的诊断串（连接拒绝 / 模型不存在 /
// 超时各不相同），调用方只做一件事 —— 降级 BM25-only。
using EmbedBackend = std::function<
    std::expected<std::vector<std::vector<float>>, std::string>(
        const EmbedConfig&, const std::vector<std::string>&, EmbedRole)>;

// 建库批量：每 64 条文本一个 /api/embed 请求（教材 kBatch=64，限请求体积）。
inline constexpr std::size_t kEmbedBatch = 64;

// RRF 的 k：60 是 canonical 值（教材同款），压扁头部位次差距、让两路
// 意见都有话语权。本切片不调参，bench 打印它。
inline constexpr double kRrfK = 60.0;

// dense 侧存储：chunk-id → 向量。空 = BM25-only —— 「没有语义路」是
// 合法状态而非错误，空容器就是这个语义的最直白表示。
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

// 混合查询入口：query 向量化一次（Query 角色）→ cosine 线性扫出 dense
// 名次 → bm25_search 出词法名次 → 两路各截候选池 max(k*8, 32) 深 →
// rrf_fuse(kRrfK) 裁至 k。dense 名次表只收 cosine > 0 的块（零相似 =
// 无信号或维度腐烂，负相似 = 反义；全库为 0 时融合自动退化为 BM25 单路）。
// 降级语义在函数内部：dense.vecs 为空、cfg.model 为空、或本次 query
// embed 失败 → 直接返回 bm25_search 结果（此时返回的分数是 BM25 原始分；
// bench 的两级阶梯各自直调 bm25_search / hybrid_search，不经过降级路径）。
//
// 模式报告（#43）：降级是静默发生的，但调用方（search_docs 工具）必须
// 诚实标注结果走了哪条路 —— RRF 融合分与 BM25 原始分量纲不同，mode 行
// 就是量纲声明。所以降级真相只能从降级发生处带出来；调用方事后猜
// （has_embeddings() && model 非空 → hybrid）在瞬断时会撒谎。
enum class SearchMode { Hybrid, Bm25Only };

struct SearchResult {
    // (chunk-id, 分数)。hybrid = RRF 融合分（上界 ≈ 参与融合的路数/61）；
    // BM25-only = BM25 原始分（无上界）。量纲由 mode 声明，分数不做归一化。
    std::vector<std::pair<std::uint32_t, double>> hits;
    SearchMode mode = SearchMode::Bm25Only;
};

[[nodiscard]] SearchResult
hybrid_search(const Bm25Index& bm25, const DenseIndex& dense,
              std::string_view query, const EmbedBackend& backend,
              const EmbedConfig& cfg, std::size_t k);

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
    // hybrid_search 自己降级，与库里有没有向量是两回事）。
    [[nodiscard]] bool has_embeddings() const;
    [[nodiscard]] const std::vector<Chunk>& chunks() const;

    // 组装结果的只读视图：bench 量具拿既有检索原语（bm25_search /
    // hybrid_search）直接打分，保持度量口径与历史基线逐位可比；
    // #43 的 search_docs 工具走 search() 入口，不该用这两个。
    [[nodiscard]] const Bm25Index& bm25() const;
    [[nodiscard]] const DenseIndex& dense() const;

    // 薄委托 hybrid_search：dense 空/未配模型/查询 embed 失败 → 恒等
    // 降级 bm25_search 语义。模式随 SearchResult 一路带出 —— Corpus 不
    // 加判断、不丢信息。
    [[nodiscard]] SearchResult
    search(std::string_view query, const EmbedBackend& backend,
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
