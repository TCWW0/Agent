// 切片 #41（issue #41）混合检索 —— cosine + RRF 名次融合 + hybrid 入口。
// 本文件先是可编译空桩（Red 阶段「编译过、断言挂」），Green 由用户按
// Red 测试亲手实现。教材对照：agentty/src/rag/bm25.cpp 的
// cosine / reciprocal_rank_fusion、corpus.cpp 的 Corpus::search
// （「retrieve wide, fuse, return narrow」，候选池 max(k*8, 32)）。
#include "my_agent/rag/rag.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <utility>

namespace my_agent::rag {

double cosine_sim(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0;

    double dot = 0.0, na = 0.0 , nb = 0.0;
    for (std::size_t i = 0;i<a.size();i++){
        const double x = static_cast<double>(a[i]);
        const double y = static_cast<double>(b[i]);
        dot += x*y;
        na += x*x;
        nb += y*y;
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot/(std::sqrt(na)*std::sqrt(nb));
}

std::vector<std::uint32_t>
ranked_ids(const std::vector<std::pair<std::uint32_t, double>>& scored) {
    std::vector<std::uint32_t> ids;
    ids.reserve(scored.size());
    for (const auto& hit:scored) ids.push_back(hit.first);
    return ids;
}

std::vector<std::pair<std::uint32_t, double>>
rrf_fuse(const std::vector<std::vector<std::uint32_t>>& ranked_lists, double k, std::size_t out_k) {
    std::unordered_map<std::uint32_t, double> fused;
    for (const auto& list: ranked_lists){
        for (std::size_t rank = 0; rank<list.size();rank++){
            fused[list[rank]] += 1.0 / (k+static_cast<double>(rank+1));
        }
    }

    std::vector<std::pair<std::uint32_t, double>> out(fused.begin(),fused.end());
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if (out.size() > out_k) out.resize(out_k);
    return out;
}

// mode 恒报 Bm25Only —— 三条降级路径这样恰好是对的，融合路径错。
// Green = 把「两路都活着且走到 rrf_fuse」的 return 改报 Hybrid
// （模式测试 Red 在 rag_test 的 SearchMode 一节）。
SearchResult
hybrid_search(const Bm25Index& bm25, const DenseIndex& dense,
              std::string_view query, const EmbedBackend& backend,
              const EmbedConfig& cfg, std::size_t k) {
    // 降级判定先于任何 backend 调用：dense 空 / 未配模型 → 纯词法路。
    // （查询向量化一次都不做 —— 那是纯浪费）
    if (dense.vecs.empty() || cfg.model.empty())
        return {bm25_search(bm25, query, k), SearchMode::Bm25Only};

    // 查询向量化一次（Query 角色）；失败/形状不对 → 该查询降级
    auto qv = backend(cfg, {std::string{query}}, EmbedRole::Query);
    if (!qv || qv->size() != 1)
        return {bm25_search(bm25, query, k), SearchMode::Bm25Only};
    const std::vector<float>& qvec = (*qv)[0];

    // 「retrieve wide, fuse, return narrow」：两路各拉 pool 深，融合后裁 k。
    // pool 深于 k —— 深位候选带着 1/(k+rank) 进场，两路叠加后可能反超
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);

    // 词法路（pool 深，分数对 RRF 是冗余 → ranked_ids 剥掉）
    const auto bm25_hits = bm25_search(bm25, query, pool);

    // 语义路：cosine 线性扫全库，只收 > 0（零 = 无信号/维度腐烂，负 = 反义）
    std::vector<std::pair<std::uint32_t, double>> dense_scored;
    dense_scored.reserve(dense.vecs.size());
    for (std::uint32_t id = 0; id < dense.vecs.size(); ++id) {
        const double s = cosine_sim(dense.vecs[id], qvec);
        if (s > 0.0) dense_scored.push_back({id, s});
    }
    std::sort(dense_scored.begin(), dense_scored.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;     // 同分 → id 升序，全链路同款
              });
    if (dense_scored.size() > pool) dense_scored.resize(pool);

    // 语义名次表为空（全库零相似，例如换模型后维度对不上）→ BM25 单路，
    // 返回原始 BM25 分而非 RRF 分 —— 降级要恒等，不要「差不多」
    if (dense_scored.empty()) {
        auto hits = bm25_hits;
        if (hits.size() > k) hits.resize(k);   // 裁前 k 条 ≡ bm25_search(k)
        return {std::move(hits), SearchMode::Bm25Only};
    }

    std::vector<std::vector<std::uint32_t>> lists;
    lists.push_back(ranked_ids(bm25_hits));
    lists.push_back(ranked_ids(dense_scored));
    return {rrf_fuse(lists, kRrfK, k), SearchMode::Hybrid};
}

} // namespace my_agent::rag
