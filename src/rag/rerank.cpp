#include "my_agent/rag/rag.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace my_agent::rag {
namespace {

std::vector<std::string> distinct_terms(std::string_view text) {
    std::vector<std::string> terms;
    tokenize(text, terms);

    std::sort(terms.begin(),terms.end());
    terms.erase(std::unique(terms.begin(),terms.end()),terms.end());
    return terms;
}

double lexical_coverage(const std::vector<std::string>& query_terms, std::string_view passage) {
    const std::vector<std::string> passage_terms = distinct_terms(passage);

    std::size_t covered = 0;
    for (const std::string& term: query_terms) {
        if (std::binary_search(passage_terms.begin(), passage_terms.end(), term)) {
            ++covered;
        }
    }

    return static_cast<double>(covered) / static_cast<double>(query_terms.size());
}

} // namespace

RerankResult
rerank_passages(std::string_view query, RetrievalResult result, const RerankBackend& backend) {
    const std::vector<std::string> query_terms = distinct_terms(query);

    if (result.passages.empty() || query_terms.empty()) {
        return RerankResult{
            .retrieval = std::move(result),
            .report = RerankReport{
                .mode = RerankMode::Skipped,
            },
        };
    }

    double low = result.passages.front().score;
    double high = low;

    for (const RankedPassage& passage : result.passages) {
        low = std::min(low,passage.score);
        high = std::max(high, passage.score);
    }

    const double range = high - low;
    constexpr double kCoverageWeight = 0.10;

    for (RankedPassage& passage: result.passages) {
        const double normalized_retrieval = range > 0.0 ? (passage.score - low) / range : 1.0;

        const double coverage = lexical_coverage(query_terms, passage.text);

        passage.rerank = RerankEvidence{
            .lexical_coverage = coverage,
            .model_score = std::nullopt,
            .final_score = (1.0 - kCoverageWeight) * normalized_retrieval + kCoverageWeight * coverage,
        };
    }

    RerankMode mode = RerankMode::LocalOnly;
    std::string backend_error;

    if (backend) {
        std::vector<std::string> passages;
        passages.reserve(result.passages.size());

        for (const RankedPassage& passage : result.passages) {
            passages.push_back(passage.text);
        }

        const auto model_scores = backend(query, passages);

        if (!model_scores) {
            mode = RerankMode::ModelFallback;
            backend_error = model_scores.error();
        } else if (model_scores->size() != result.passages.size()) {
            mode = RerankMode::ModelFallback;
            backend_error = "reranker returned " + std::to_string(model_scores->size())
                + " scores for " + std::to_string(result.passages.size()) + " passages";
        } else if (const auto invalid = std::find_if(model_scores->begin(), model_scores->end(),
                    [](double score){return !std::isfinite(score);}); invalid != model_scores->end()){
            mode = RerankMode::ModelFallback;
            backend_error = "reranker returned non-finite score at index "
                        + std::to_string(static_cast<std::size_t>(invalid - model_scores->begin()));
        } else {
            const auto [low_it, high_it] = std::minmax_element(model_scores->begin(), model_scores->end());

            const double model_low = *low_it;
            const double model_range = *high_it - *low_it;
            constexpr double kModelWeight = 0.50;

            for (std::size_t i = 0; i < result.passages.size(); ++i) {
                RankedPassage& passage = result.passages[i];
                const double raw_model_score = (*model_scores)[i];
                const double normalized_model = model_range > 0.0
                                                ? (raw_model_score - model_low) / model_range : 1.0;

                const double local_score = passage.rerank->final_score;
                passage.rerank->model_score = raw_model_score;
                passage.rerank->final_score = (1.0 - kModelWeight) * local_score + kModelWeight * normalized_model;
            }
            mode = RerankMode::ModelEnhanced;
        }
    }

    std::stable_sort(result.passages.begin(), result.passages.end(),
                     [](const RankedPassage& lhs, const RankedPassage& rhs) {
                        return lhs.rerank->final_score > rhs.rerank->final_score;
                    });

    return RerankResult{
        .retrieval = std::move(result),
        .report = RerankReport{
            .mode = mode,
            .backend_error = std::move(backend_error),
        },
    };
}
} // namespace my_agent::rag
