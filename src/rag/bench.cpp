// rag_bench：检索质量基准。
// 用法: rag_bench <docs目录> [k] [probe_n] [embed_model]
//   k        截断名次（默认 5）
//   probe_n  改写探针样本数（默认 100；0 = 不跑探针）
//   embed_model  dense 路模型（默认 nomic-embed-text:latest）
//
// #42 起建库走 Corpus（目录 → 块 + BM25 + dense + 增量缓存）：首跑全量
// 嵌入（分钟级），此后未变文件直接搬运 —— [corpus] 行是 #42 的验收
// 数字：第二跑应 reused=全量、embedded=0、build 秒级。
// 三级阶梯的度量口径与 #41 基线逐位相同（同 pool、直调 bm25_search /
// hybrid_search，经 Corpus 的只读视图取索引），跨切片可比。
//
// 阶梯设计（#41 验收口径「差值可测且可解释」，非「hybrid 必须更高」）：
//   阶梯A 合成查询（BM25 主场）：查询词就是从块里按 tf-idf 挑的，词法
//         命中接近满分 —— hybrid 不加分甚至反降都在预期内（官方数据
//         hybrid+prf MRR 0.991→0.948 同向为证）。
//   阶梯B 改写探针（dense 主场）：LLM 针对同一批 gold 块重新措辞生成查询，
//         词法不再精确命中 —— 量的是 dense 路在 BM25 失手时的兜底能力。
// 改写按 gold 块身份（path:行区间 + 改写器版本）缓存到
// rag_bench_rewrites.json，首次生成耗 LLM 时间，之后命中缓存即时复现。
// 探针语言教训（2026-09-10 全量首跑）：中文 prompt 会把改写带成中文，
// 而语料是纯英文 —— 探针就变成了跨语言检索（最难情形），词法鸿沟与
// 语言鸿沟混淆。v2 起要求改写与文档同语言。
// 教材对照：agentty/src/rag/bench.cpp（无探针，为本片新增设计）。

#include "my_agent/http/http_client.hpp"
#include "my_agent/rag/rag.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rag = my_agent::rag;
namespace fs = std::filesystem;
namespace http = my_agent::http;

namespace {

constexpr const char* kOllamaHost = "localhost";
constexpr int kOllamaPort = 11434;
constexpr const char* kChatModel = "qwen3.5:latest";
constexpr const char* kRewriteCache = "rag_bench_rewrites.json";

struct Case { std::string query; std::uint32_t gold; };

bool covers(const rag::Chunk& c, const rag::Chunk& gold) {
    return c.path == gold.path
        && c.line_start <= gold.line_end
        && gold.line_start <= c.line_end;
}

std::string synthesize_query(const rag::Chunk& c, const rag::Bm25Index& idx) {
    std::vector<std::string> toks;
    rag::tokenize(c.text, toks);

    std::unordered_map<std::string, std::uint32_t> tf;
    for (const auto& t : toks) ++tf[t];

    const double N = static_cast<double>(idx.doc_count);
    struct Scored { double score; const std::string* term; };
    std::vector<Scored> scored;
    scored.reserve(tf.size());
    for (const auto& [term, count] : tf) {
        auto it = idx.term_ids.find(term);
        if (it == idx.term_ids.end()) continue;   // 理论不可达：词就在本块里
        const double df = static_cast<double>(idx.postings[it->second].size());
        scored.push_back({count * std::log((N + 1.0) / (df + 0.5)), &term});
    }
    // 排序全定：分数降序，同分按字典序 —— 同语料永远出同一组查询。
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return *a.term < *b.term;
    });

    std::string q;
    for (std::size_t i = 0; i < scored.size() && i < 4; ++i) {
        if (!q.empty()) q += ' ';
        q += *scored[i].term;
    }
    return q;
}

std::string miss_line(std::string_view tag, std::string_view query,
                      const rag::Chunk& gold) {
    std::string s = "    [";
    s += tag;
    s += "] \"";
    s += query;
    s += "\" -> ";
    s += gold.path;
    s += ":" + std::to_string(gold.line_start) + "-" + std::to_string(gold.line_end);
    return s;
}

// 一级阶梯的度量循环：search 回调抽象掉 bm25/hybrid 之分，指标口径唯一。
struct Ladder {
    double recall = 0.0;
    double mrr = 0.0;
    double us_per_q = 0.0;
    std::vector<std::string> misses;
};

template <typename Search>
Ladder run_ladder(const std::vector<Case>& cases,
                  const std::vector<rag::Chunk>& chunks,
                  std::size_t k, std::size_t pool, Search&& search) {
    Ladder out;
    double recall = 0.0, mrr = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& [query, gold] : cases) {
        const auto ranked = search(query);
        bool found = false;
        std::size_t rank = 0;
        for (; rank < ranked.size(); ++rank)
            if (covers(chunks[ranked[rank].first], chunks[gold])) { found = true; break; }
        if (!found) {
            out.misses.push_back(miss_line("rank>" + std::to_string(pool),
                                           query, chunks[gold]));
            continue;
        }
        mrr += 1.0 / static_cast<double>(rank + 1);
        if (rank < k) {
            recall += 1.0;
        } else {
            out.misses.push_back(miss_line("rank " + std::to_string(rank + 1),
                                           query, chunks[gold]));
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double n = static_cast<double>(cases.size());
    if (n > 0) {
        out.recall = recall / n;
        out.mrr = mrr / n;
        out.us_per_q = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                           .count() / n;
    }
    return out;
}

void report_row(std::string_view name, const Ladder& l, std::size_t k) {
    std::cout << "  " << std::left << std::setw(10) << name << std::right
              << " recall@" << k << "=" << std::fixed << std::setprecision(3) << l.recall
              << "  MRR=" << l.mrr
              << "  " << std::setprecision(0) << l.us_per_q << " us/q\n";
    for (std::size_t i = 0; i < l.misses.size() && i < 5; ++i)
        std::cout << l.misses[i] << "\n";
}

// ── 改写探针：LLM 针对 gold 块重新措辞 ───────────────────────────────────

std::string chunk_key(const fs::path& root, const rag::Chunk& c) {
    // "|v3" 是改写器版本：prompt 语义变了就必须换键，否则旧改写命中缓存
    // 把修复静默吞掉。v2 教训：「与文档相同语言」的指令写在中文 prompt 里
    // 会被 prompt 自身的语言锚压过（模型跟着 prompt 语言走）——v3 起由
    // 代码检测文档语言、直接用该语言写 prompt，指令与锚强制一致。
    // 路径用绝对形式：#41 时期的键就是绝对路径，保持一致才能复用存量
    // 改写缓存（#42 起 Corpus 块路径是相对 root 的，这里拼回去）。
    return (root / c.path).string() + ":" + std::to_string(c.line_start)
         + "-" + std::to_string(c.line_end) + "|v3";
}

// 文档里有没有 CJK 字符（UTF-8 首字节 E4..E9 覆盖 U+4E00..U+9FFF 常用区）。
// 只做语言选择，不做精确分词，够用即可。
bool has_cjk(std::string_view s) {
    for (const char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        if (b >= 0xE4 && b <= 0xE9) return true;
    }
    return false;
}

std::expected<std::string, std::string> chat_rewrite(const rag::Chunk& c) {
    const std::string doc = c.context.empty() ? c.text : c.context + "\n" + c.text;
    // prompt 语言 = 目标语言：语言锚比文字指令更强势，让两者强制对齐。
    const std::string prompt = has_cjk(doc)
        ? "你是检索评测的查询改写器。给你一段文档，请写一条用户会用来检索到它的"
          "中文自然语言查询。用自己的话概括主题，禁止照抄原文中连续 4 个字以上的"
          "片段；不超过 25 个字；只输出查询本身，不要任何解释。\n文档：" + doc
        : "You are a query rewriter for retrieval evaluation. Given the document "
          "below, write a natural-language query that a user would type to find it. "
          "Paraphrase in your own words: do NOT copy any run of 3 or more "
          "consecutive words from the document. Keep it under 12 words. Output "
          "only the query itself, nothing else.\nDocument: " + doc;

    const nlohmann::json body = {
        {"model", kChatModel},
        {"stream", false},
        {"think", false},
        {"messages", nlohmann::json::array({
            nlohmann::json{{"role", "user"}, {"content", prompt}},
        })},
    };

    http::HttpClient client;
    const auto result = client.post(http::HttpRequest{
        .host       = kOllamaHost,
        .port       = kOllamaPort,
        .path       = "/api/chat",
        .headers    = {{"content-type", "application/json"}},
        .body       = body.dump(),
        .use_tls    = false,
        .timeout_ms = 60000,
    });
    if (!result)
        return std::unexpected("chat HTTP 失败：" + result.error().message);
    try {
        const auto j = nlohmann::json::parse(*result);
        std::string q = j.at("message").at("content").get<std::string>();
        // 模型偶尔加引号/换行：取首个非空行，剥包裹引号。
        const auto nl = q.find('\n');
        if (nl != std::string::npos) q.resize(nl);
        while (!q.empty() && (q.front() == ' ' || q.back() == ' ')) {
            if (q.front() == ' ') q.erase(0, 1);
            if (!q.empty() && q.back() == ' ') q.pop_back();
        }
        while (q.size() >= 2
               && ((q.front() == '"' && q.back() == '"')
                   || (q.front() == '\'' && q.back() == '\''))) {
            q = q.substr(1, q.size() - 2);
        }
        if (q.empty())
            return std::unexpected(std::string{"chat 返回空查询"});
        return q;
    } catch (const std::exception& e) {
        return std::unexpected(std::string{"chat 响应解析失败："} + e.what());
    }
}

std::unordered_map<std::string, std::string> load_rewrite_cache() {
    std::unordered_map<std::string, std::string> map;
    std::ifstream in(kRewriteCache);
    if (!in) return map;
    try {
        const auto j = nlohmann::json::parse(in);
        for (const auto& e : j.at("rewrites"))
            map.emplace(e.at("key").get<std::string>(), e.at("q").get<std::string>());
    } catch (const std::exception&) {
        // 损坏缓存当不存在：重新生成就是了，量具不值得为它崩。
    }
    return map;
}

void save_rewrite_cache(const std::unordered_map<std::string, std::string>& map) {
    nlohmann::json j = nlohmann::json::object();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [key, q] : map)
        arr.push_back({{"key", key}, {"q", q}});
    j["rewrites"] = std::move(arr);
    std::ofstream out(kRewriteCache);
    out << j.dump(2) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法: rag_bench <docs目录> [k] [probe_n] [embed_model]\n";
        return 1;
    }
    const fs::path root = argv[1];
    const std::size_t k = argc >= 3 ? std::stoul(argv[2]) : 5;
    const std::size_t probe_n = argc >= 4 ? std::stoul(argv[3]) : 100;
    const std::string embed_model = argc >= 5 ? argv[4] : "nomic-embed-text:latest";

    // 1)+2)+4) Corpus 建库（#42 起，原「收语料 + 词法建库 + 语义建库」
    //    三步合一）：目录 → 块 + BM25 + dense + 增量缓存。embed 失败 →
    //    Corpus 内部降级 BM25-only（dense_ok=false），下面两级阶梯跳过。
    rag::EmbedConfig cfg{.host = kOllamaHost, .port = kOllamaPort,
                         .model = embed_model, .timeout_ms = 60000};
    rag::Corpus corpus;
    rag::Corpus::BuildStats st;
    double build_s = 0.0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        st = corpus.build(root, rag::ollama_embed, cfg);
        build_s = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();
    }
    const std::vector<rag::Chunk>& chunks = corpus.chunks();
    const rag::Bm25Index&          idx    = corpus.bm25();
    const rag::DenseIndex&         dense  = corpus.dense();
    const bool dense_ok = st.dense_ok;
    if (chunks.empty()) {
        std::cerr << "目录里没有可用 .md: " << root << "\n";
        return 1;
    }

    // 3) 合成已知答案查询（过短块词袋贫乏，只会污染指标）。
    std::vector<Case> cases;
    for (std::uint32_t d = 0; d < chunks.size(); ++d)
        if (chunks[d].text.size() >= 120)
            cases.push_back({synthesize_query(chunks[d], idx), d});

    const std::size_t pool = std::max<std::size_t>(k * 5, 30);
    const char* cache_state =
        st.files_reused == 0                        ? "miss(全量重建)"
        : (st.files_reused == st.files_seen && st.chunks_embedded == 0)
                                                   ? "hit(全命中)"
                                                    : "partial(部分命中)";
    std::cout << "files=" << st.files_seen
              << " chunks=" << chunks.size()
              << " queries=" << cases.size()
              << " k=" << k << " probe_n=" << probe_n
              << " rrf_k=" << rag::kRrfK << "\n";
    std::cout << "[corpus] model=" << embed_model
              << (st.dense_ok ? " dense=ok" : " dense=降级bm25-only")
              << " dim=" << (dense.vecs.empty() ? 0 : dense.vecs[0].size())
              << "  reused=" << st.files_reused << "/" << st.files_seen
              << " rechunked=" << st.files_rechunked
              << " embedded=" << st.chunks_embedded
              << "  build=" << build_s << "s"
              << "  cache=" << cache_state << "\n";

    // dense 单路检索：query 向量 + 全库 cosine 线性扫 —— 与 hybrid_search
    // 内部同口径（只收 cosine>0、截断 pool、同分按 chunk-id 升序）。
    // 矩阵里补上这列，hybrid 的差值才能直接分解到 dense 单路质量。
    const auto dense_search = [&](const std::string& q)
        -> std::vector<std::pair<std::uint32_t, double>> {
        auto v = rag::ollama_embed(cfg, {q}, rag::EmbedRole::Query);
        if (!v || v->empty()) return {};
        std::vector<std::pair<std::uint32_t, double>> scored;
        for (std::uint32_t i = 0; i < dense.vecs.size(); ++i) {
            const double s = rag::cosine_sim((*v)[0], dense.vecs[i]);
            if (s > 0.0) scored.push_back({i, s});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        if (scored.size() > pool) scored.resize(pool);
        return scored;
    };

    // 5) 阶梯A：合成查询 —— BM25 主场。
    std::cout << "── 合成查询（BM25 主场）──\n";
    const Ladder a_bm25 = run_ladder(cases, chunks, k, pool,
        [&](const std::string& q) { return rag::bm25_search(idx, q, pool); });
    report_row("bm25-only", a_bm25, k);
    if (dense_ok) {
        report_row("dense-only", run_ladder(cases, chunks, k, pool, dense_search), k);
        const Ladder a_hybrid = run_ladder(cases, chunks, k, pool,
            [&](const std::string& q) {
                return rag::hybrid_search(idx, dense, q, rag::ollama_embed,
                                          cfg, pool).hits;
            });
        report_row("hybrid", a_hybrid, k);
        std::cout << "  (dense/hybrid 计时含每查询一次 embed 网络往返)\n";
    }

    // 6) 阶梯B：改写探针 —— dense 主场。固定种子抽样，缓存按 gold 块身份
    //    复用；LLM 改写失败重试一次，仍失败则丢弃该样本（计数报告）。
    if (probe_n == 0 || cases.empty()) return 0;
    std::vector<std::uint32_t> probe_pick(cases.size());
    std::iota(probe_pick.begin(), probe_pick.end(), 0u);
    std::shuffle(probe_pick.begin(), probe_pick.end(), std::mt19937{42u});
    probe_pick.resize(std::min(probe_n, probe_pick.size()));

    auto cache = load_rewrite_cache();
    std::vector<Case> probes;
    std::size_t fresh = 0, dropped = 0;
    for (std::size_t i = 0; i < probe_pick.size(); ++i) {
        const rag::Chunk& gold = chunks[cases[probe_pick[i]].gold];
        const std::string key = chunk_key(root, gold);
        if (auto it = cache.find(key); it != cache.end()) {
            probes.push_back({it->second, cases[probe_pick[i]].gold});
            continue;
        }
        auto q = chat_rewrite(gold);
        if (!q) q = chat_rewrite(gold);   // 一次重试：偶发空响应不值得弃样本
        if (!q) { ++dropped; continue; }
        cache.emplace(key, *q);
        ++fresh;
        probes.push_back({std::move(*q), cases[probe_pick[i]].gold});
        if (fresh % 10 == 0) {
            save_rewrite_cache(cache);   // 增量落盘：探针段被中断（杀进程/
                                         // 网络事故）不至于丢掉已生成的改写
            std::cerr << "  改写生成 " << fresh << "/" << probe_pick.size() << "\n";
        }
    }
    if (fresh > 0) save_rewrite_cache(cache);
    if (dropped > 0)
        std::cout << "[probe] " << dropped << " 个样本改写失败已丢弃\n";
    if (probes.empty()) {
        std::cout << "[probe] 无可用改写样本，探针跳过\n";
        return 0;
    }

    std::cout << "── 改写探针 N=" << probes.size() << "（dense 主场）──"
              << "  cache命中=" << probes.size() - fresh << " 新生成=" << fresh << "\n";
    for (std::size_t i = 0; i < probes.size() && i < 3; ++i) {
        const rag::Chunk& g = chunks[probes[i].gold];
        std::cout << "  样例: \"" << probes[i].query << "\" -> " << g.path << ":"
                  << g.line_start << "-" << g.line_end << "\n";
    }
    const Ladder b_bm25 = run_ladder(probes, chunks, k, pool,
        [&](const std::string& q) { return rag::bm25_search(idx, q, pool); });
    report_row("bm25-only", b_bm25, k);
    if (dense_ok) {
        report_row("dense-only", run_ladder(probes, chunks, k, pool, dense_search), k);
        const Ladder b_hybrid = run_ladder(probes, chunks, k, pool,
            [&](const std::string& q) {
                return rag::hybrid_search(idx, dense, q, rag::ollama_embed,
                                          cfg, pool).hits;
            });
        report_row("hybrid", b_hybrid, k);
    }
    return 0;
}
