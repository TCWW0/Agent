// 切片 #41（issue #41）混合检索 —— cosine + RRF 名次融合 + hybrid 入口。
// 本文件先是可编译空桩（Red 阶段「编译过、断言挂」），Green 由用户按
// Red 测试亲手实现。教材对照：agentty/src/rag/bm25.cpp 的
// cosine / reciprocal_rank_fusion、corpus.cpp 的 Corpus::retrieve
// （「retrieve wide, fuse, return narrow」，候选池 max(k*8, 32)）。
#include "my_agent/rag/rag.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace my_agent::rag {

namespace {

CandidateSet make_bm25_candidates(std::vector<std::pair<std::uint32_t,double>> hits,
                                  std::size_t k) {
    if (hits.size() > k){
        hits.resize(k);
    }

    CandidateSet result;
    result.mode = SearchMode::Bm25Only;
    result.candidates.reserve(hits.size());

    for (std::size_t i = 0; i < hits.size(); ++i) {
        const auto& [chunk_id, score] = hits[i];

        Candidate candidate;
        candidate.chunk_id = chunk_id;
        candidate.final_score = score;
        candidate.sparse = RankedScore{
            .score = score,
            .rank = i + 1,
        };

        result.candidates.push_back(std::move(candidate));
    }
    return result;
}
}

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

CandidateSet
retrieve_candidates(const Bm25Index& bm25, const DenseIndex& dense,
                    std::string_view query, const EmbedBackend& backend,
                    const EmbedConfig& cfg, std::size_t k){
    if (dense.vecs.empty() || cfg.model.empty()) {
        return make_bm25_candidates(bm25_search(bm25,query,k), k);
    }

    auto query_vectors = backend(cfg, {std::string{query}}, EmbedRole::Query);

    if (!query_vectors || query_vectors->size() != 1){
        return make_bm25_candidates(bm25_search(bm25,query,k), k);
    }

    const std::vector<float>& query_vector = (*query_vectors)[0];
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);

    auto sparse_hits = bm25_search(bm25, query, pool);

    std::vector<std::pair<std::uint32_t, double>> dense_hits;
    dense_hits.reserve(dense.vecs.size());

    for (std::size_t i = 0;i < dense.vecs.size(); ++i) {
        const double score = cosine_sim(dense.vecs[i], query_vector);

        if (score > 0.0) {
            dense_hits.emplace_back(static_cast<std::uint32_t>(i),score);
        }
    }

    std::sort(dense_hits.begin(), dense_hits.end(),
            [](const auto& lhs, const auto& rhs) {
                if (lhs.second != rhs.second) {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            }
    );

    if (dense_hits.size() > pool) {
        dense_hits.resize(pool);
    }

    if (dense_hits.empty()) {
        return make_bm25_candidates(std::move(sparse_hits), k);
    }

    std::vector<std::vector<std::uint32_t>> ranked_lists;
    ranked_lists.reserve(2);
    ranked_lists.push_back(ranked_ids(sparse_hits));
    ranked_lists.push_back(ranked_ids(dense_hits));

    const auto fused_hits = rrf_fuse(ranked_lists, kRrfK, k);

    std::unordered_map<std::uint32_t, RankedScore> sparse_evidence;
    std::unordered_map<std::uint32_t, RankedScore> dense_evidence;

    sparse_evidence.reserve(sparse_hits.size());
    dense_evidence.reserve(dense_hits.size());

    for (std::size_t i = 0; i < sparse_hits.size(); ++i) {
        const auto& [chunk_id, score] = sparse_hits[i];

        sparse_evidence.emplace(
            chunk_id,
            RankedScore{
                .score = score,
                .rank = i + 1,
            }
        );
    }

    for (std::size_t i = 0; i < dense_hits.size(); ++i) {
        const auto& [chunk_id, score] = dense_hits[i];

        dense_evidence.emplace(
            chunk_id,
            RankedScore{
                .score = score,
                .rank = i + 1,
            }
        );
    }

    CandidateSet result;
    result.mode = SearchMode::Hybrid;
    result.candidates.reserve(fused_hits.size());

    for (const auto& [chunk_id, final_score] : fused_hits) {
        Candidate candidate;
        candidate.chunk_id = chunk_id;
        candidate.final_score = final_score;

        if (const auto it = sparse_evidence.find(chunk_id); it != sparse_evidence.end()) {
            candidate.sparse = it->second;
        }

        if (const auto it = dense_evidence.find(chunk_id); it != dense_evidence.end()) {
            candidate.dense = it->second;
        }

        result.candidates.push_back(std::move(candidate));
    }
    return result;
}

} // namespace my_agent::rag
