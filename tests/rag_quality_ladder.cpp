#include "my_agent/rag/rag.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
namespace rag = my_agent::rag;
using Clock = std::chrono::steady_clock;

// Learning-oriented quality ladder, following Agentty's benchmark shape:
// run the same judged queries through progressively fuller public RAG seams,
// then compare ranking quality and latency one rung at a time.
//
// This is deliberately a measuring instrument, not a CTest pass/fail gate.
// Quality thresholds would freeze today's implementation instead of showing
// whether the next retrieval experiment helped or harmed it.

struct GoldTarget {
    std::string path;
    int line_start = 0;
    int line_end = 0;
};

struct Judgment {
    std::string query;
    std::vector<GoldTarget> gold;
};

struct Span {
    std::string path;
    int line_start = 0;
    int line_end = 0;
};

struct Options {
    fs::path docs_root;
    fs::path judgments_path;
    std::size_t candidate_k = 16;
    std::string label;
    std::optional<fs::path> output;
};

void print_usage(std::ostream& out) {
    out
        << "usage: rag_quality_ladder DOCS_ROOT PASSAGE_JUDGMENTS [candidate_k]"
           " [--label NAME] [--output REPORT.json]\n"
        << "  candidate_k defaults to 16 and must be in [10, 80]\n"
        << "  --output creates parent directories and refuses to overwrite a report\n"
        << "  MY_AGENT_EMBED_MODEL enables the real hybrid path; when unset, "
           "the ladder is deterministic BM25-only.\n";
}

std::expected<std::vector<Judgment>, std::string>
load_judgments(const fs::path& path) {
    std::ifstream input(path);
    if (!input) return std::unexpected("cannot open judgments: " + path.string());

    nlohmann::json root;
    try {
        input >> root;
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected("invalid judgments JSON: " + std::string{e.what()});
    }

    if (!root.is_object() || !root.contains("cases") || !root["cases"].is_array()) {
        return std::unexpected("judgments must contain a cases array");
    }

    std::vector<Judgment> out;
    for (std::size_t case_index = 0; case_index < root["cases"].size(); ++case_index) {
        const auto& item = root["cases"][case_index];
        if (!item.is_object() || !item.contains("q") || !item["q"].is_string()
            || item["q"].get_ref<const std::string&>().empty()) {
            return std::unexpected("case " + std::to_string(case_index)
                                   + " must have a non-empty string q");
        }
        if (!item.contains("gold") || !item["gold"].is_array()
            || item["gold"].empty()) {
            return std::unexpected("case " + std::to_string(case_index)
                                   + " must have a non-empty gold array");
        }

        Judgment judgment{.query = item["q"].get<std::string>()};
        for (std::size_t gold_index = 0; gold_index < item["gold"].size(); ++gold_index) {
            const auto& target = item["gold"][gold_index];
            if (!target.is_object()
                || !target.contains("path") || !target["path"].is_string()
                || !target.contains("line_start") || !target["line_start"].is_number_integer()
                || !target.contains("line_end") || !target["line_end"].is_number_integer()) {
                return std::unexpected(
                    "case " + std::to_string(case_index) + " gold "
                    + std::to_string(gold_index)
                    + " must be {path,line_start,line_end}; path-only judgments "
                      "cannot measure passage quality");
            }

            GoldTarget gold{
                .path = target["path"].get<std::string>(),
                .line_start = target["line_start"].get<int>(),
                .line_end = target["line_end"].get<int>(),
            };
            if (gold.path.empty() || gold.line_start < 1 || gold.line_end < gold.line_start) {
                return std::unexpected("case " + std::to_string(case_index)
                                       + " has an invalid gold interval");
            }
            judgment.gold.push_back(std::move(gold));
        }
        out.push_back(std::move(judgment));
    }

    if (out.empty()) return std::unexpected("judgments contain no cases");
    return out;
}

bool overlaps(const Span& hit, const GoldTarget& gold) {
    return hit.path == gold.path
        && hit.line_start <= gold.line_end
        && gold.line_start <= hit.line_end;
}

std::size_t first_relevant_rank(const std::vector<Span>& hits,
                                const std::vector<GoldTarget>& gold) {
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (std::ranges::any_of(gold, [&](const GoldTarget& target) {
                return overlaps(hits[i], target);
            })) {
            return i + 1;
        }
    }
    return 0;
}

std::vector<Span> spans_from_bm25(
    const std::vector<std::pair<std::uint32_t, double>>& hits,
    const std::vector<rag::Chunk>& chunks) {
    std::vector<Span> out;
    out.reserve(hits.size());
    for (const auto& [chunk_id, score] : hits) {
        (void)score;
        const auto& chunk = chunks.at(chunk_id);
        out.push_back({chunk.path, chunk.line_start, chunk.line_end});
    }
    return out;
}

std::vector<Span> spans_from_retrieval(const rag::RetrievalResult& result) {
    std::vector<Span> out;
    out.reserve(result.passages.size());
    for (const auto& passage : result.passages) {
        out.push_back({
            passage.provenance.path,
            passage.provenance.line_start,
            passage.provenance.line_end,
        });
    }
    return out;
}

struct Metrics {
    std::size_t cases = 0;
    std::size_t hit_at_one = 0;
    std::size_t hit_at_four = 0;
    std::size_t recall_at_ten = 0;
    std::size_t candidate_recall = 0;
    double reciprocal_rank_at_ten = 0.0;
    double ndcg_at_ten = 0.0;
    double total_us = 0.0;

    void add(std::size_t rank, std::size_t candidate_k, double elapsed_us) {
        ++cases;
        total_us += elapsed_us;
        if (rank == 0) return;

        if (rank == 1) ++hit_at_one;
        if (rank <= 4) ++hit_at_four;
        if (rank <= 10) {
            ++recall_at_ten;
            reciprocal_rank_at_ten += 1.0 / static_cast<double>(rank);
            // Gold intervals are alternatives for one information need, so the
            // ideal gain is one relevant passage at rank 1 (IDCG = 1).
            ndcg_at_ten += 1.0 / std::log2(static_cast<double>(rank) + 1.0);
        }
        if (rank <= candidate_k) ++candidate_recall;
    }
};

double ratio(std::size_t numerator, std::size_t denominator) {
    return denominator == 0 ? 0.0
                            : static_cast<double>(numerator)
                                / static_cast<double>(denominator);
}

void report_header(std::size_t candidate_k) {
    std::cout << "\n  " << std::left << std::setw(19) << "stage" << std::right
              << "  hit@1  hit@4  recall@10  MRR@10  nDCG@10"
              << "  recall@" << candidate_k << "  ms/query\n";
    std::cout << "  " << std::string(19, '-')
              << "  -----  -----  ---------  ------  -------  "
              << std::string(std::to_string(candidate_k).size() + 7, '-')
              << "  --------\n";
}

void report_row(std::string_view name, const Metrics& metrics) {
    const double n = static_cast<double>(metrics.cases);
    std::cout << "  " << std::left << std::setw(19) << name << std::right
              << "  " << std::fixed << std::setprecision(3)
              << ratio(metrics.hit_at_one, metrics.cases)
              << "  " << ratio(metrics.hit_at_four, metrics.cases)
              << "     " << ratio(metrics.recall_at_ten, metrics.cases)
              << "   " << (n == 0.0 ? 0.0 : metrics.reciprocal_rank_at_ten / n)
              << "    " << (n == 0.0 ? 0.0 : metrics.ndcg_at_ten / n)
              << "      " << ratio(metrics.candidate_recall, metrics.cases)
              << "     " << std::setprecision(2)
              << (n == 0.0 ? 0.0 : metrics.total_us / n / 1000.0)
              << "\n";
}

std::optional<int> parse_port(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    int port = 0;
    const char* end = text + std::char_traits<char>::length(text);
    const auto [parsed, error] = std::from_chars(text, end, port);
    if (error != std::errc{} || parsed != end || port < 1 || port > 65535) {
        return std::nullopt;
    }
    return port;
}

std::expected<std::size_t, std::string> parse_candidate_k(std::string_view text) {
    std::size_t value = 0;
    const auto [parsed, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || parsed != text.data() + text.size()
        || value < 10 || value > 80) {
        return std::unexpected("candidate_k must be an integer in [10, 80]");
    }
    return value;
}

std::expected<Options, std::string> parse_options(int argc, char** argv) {
    if (argc < 3) return std::unexpected("missing docs root or judgments path");

    Options options{
        .docs_root = argv[1],
        .judgments_path = argv[2],
    };
    bool candidate_k_seen = false;
    bool label_seen = false;

    for (int i = 3; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--label") {
            if (label_seen || i + 1 >= argc || argv[i + 1][0] == '\0'
                || std::string_view{argv[i + 1]}.starts_with("--")) {
                return std::unexpected("--label requires one non-empty value");
            }
            options.label = argv[++i];
            label_seen = true;
        } else if (argument == "--output") {
            if (options.output || i + 1 >= argc || argv[i + 1][0] == '\0'
                || std::string_view{argv[i + 1]}.starts_with("--")) {
                return std::unexpected("--output requires one report path");
            }
            options.output = fs::path{argv[++i]};
        } else if (argument.starts_with("--")) {
            return std::unexpected("unknown option: " + std::string{argument});
        } else {
            if (candidate_k_seen) {
                return std::unexpected("candidate_k may be supplied only once");
            }
            const auto parsed = parse_candidate_k(argument);
            if (!parsed) return std::unexpected(parsed.error());
            options.candidate_k = *parsed;
            candidate_k_seen = true;
        }
    }

    return options;
}

std::string rank_change(std::size_t before, std::size_t after) {
    if (before == after) return "unchanged";
    if (before == 0 && after > 0) return "recovered";
    if (before > 0 && after == 0) return "lost";
    return after < before ? "improved" : "degraded";
}

nlohmann::json metrics_json(const Metrics& metrics, std::size_t candidate_k) {
    const double n = static_cast<double>(metrics.cases);
    return {
        {"cases", metrics.cases},
        {"hit_at_1", {
            {"hits", metrics.hit_at_one},
            {"score", ratio(metrics.hit_at_one, metrics.cases)},
        }},
        {"hit_at_4", {
            {"hits", metrics.hit_at_four},
            {"score", ratio(metrics.hit_at_four, metrics.cases)},
        }},
        {"recall_at_10", {
            {"hits", metrics.recall_at_ten},
            {"score", ratio(metrics.recall_at_ten, metrics.cases)},
        }},
        {"mrr_at_10", {
            {"reciprocal_rank_sum", metrics.reciprocal_rank_at_ten},
            {"score", n == 0.0 ? 0.0 : metrics.reciprocal_rank_at_ten / n},
        }},
        {"ndcg_at_10", {
            {"discounted_gain_sum", metrics.ndcg_at_ten},
            {"score", n == 0.0 ? 0.0 : metrics.ndcg_at_ten / n},
        }},
        {"candidate_recall", {
            {"k", candidate_k},
            {"hits", metrics.candidate_recall},
            {"score", ratio(metrics.candidate_recall, metrics.cases)},
        }},
        {"mean_ms_per_query", n == 0.0 ? 0.0 : metrics.total_us / n / 1000.0},
    };
}

std::expected<void, std::string>
write_report(const fs::path& path, const nlohmann::json& report) {
    std::error_code error;
    if (fs::exists(path, error)) {
        return std::unexpected("refusing to overwrite existing report: " + path.string());
    }
    if (error) {
        return std::unexpected("cannot inspect report path: " + error.message());
    }

    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            return std::unexpected("cannot create report directory: " + error.message());
        }
    }

    std::ofstream output(path);
    if (!output) return std::unexpected("cannot create report: " + path.string());
    output << report.dump(2) << '\n';
    if (!output) return std::unexpected("cannot write report: " + path.string());
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        print_usage(std::cout);
        return 0;
    }

    const auto parsed_options = parse_options(argc, argv);
    if (!parsed_options) {
        std::cerr << parsed_options.error() << "\n";
        print_usage(std::cerr);
        return 2;
    }
    const Options& options = *parsed_options;

    if (options.output) {
        std::error_code error;
        if (fs::exists(*options.output, error)) {
            std::cerr << "refusing to overwrite existing report: "
                      << options.output->string() << "\n";
            return 2;
        }
        if (error) {
            std::cerr << "cannot inspect report path: " << error.message() << "\n";
            return 2;
        }
    }

    const auto judgments = load_judgments(options.judgments_path);
    if (!judgments) {
        std::cerr << judgments.error() << "\n";
        return 2;
    }

    rag::EmbedConfig embed;
    if (const char* model = std::getenv("MY_AGENT_EMBED_MODEL"); model && *model) {
        embed.model = model;
    }
    if (const char* host = std::getenv("MY_AGENT_OLLAMA_HOST"); host && *host) {
        embed.host = host;
    }
    if (const auto port = parse_port(std::getenv("MY_AGENT_OLLAMA_PORT"))) {
        embed.port = *port;
    }

    rag::Corpus corpus;
    const auto build_started = Clock::now();
    const rag::Corpus::BuildStats build =
        corpus.build(options.docs_root, rag::ollama_embed, embed);
    const double build_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - build_started).count();

    if (corpus.chunk_count() == 0) {
        std::cerr << "no Markdown chunks found under " << options.docs_root << "\n";
        return 2;
    }

    std::cout << "rag-quality-ladder: cases=" << judgments->size()
              << " chunks=" << corpus.chunk_count()
              << " candidate_k=" << options.candidate_k
              << " dense=" << (build.dense_ok ? "ready" : "off/fallback")
              << " build_ms=" << std::fixed << std::setprecision(1) << build_ms
              << "\n";

    Metrics bm25_metrics;
    Metrics retrieval_metrics;
    Metrics rerank_metrics;
    std::size_t hybrid_queries = 0;
    std::map<std::string, std::size_t> retrieval_changes;
    std::map<std::string, std::size_t> rerank_changes;
    nlohmann::json case_reports = nlohmann::json::array();

    for (const Judgment& judgment : *judgments) {
        const auto bm25_started = Clock::now();
        const auto bm25_hits = rag::bm25_search(
            corpus.bm25(), judgment.query, options.candidate_k);
        const double bm25_us = std::chrono::duration<double, std::micro>(
            Clock::now() - bm25_started).count();
        const std::size_t bm25_rank = first_relevant_rank(
            spans_from_bm25(bm25_hits, corpus.chunks()), judgment.gold);
        bm25_metrics.add(bm25_rank, options.candidate_k, bm25_us);

        const auto retrieval_started = Clock::now();
        rag::RetrievalResult retrieval = corpus.retrieve(
            judgment.query, rag::ollama_embed, embed, options.candidate_k);
        const double retrieval_us = std::chrono::duration<double, std::micro>(
            Clock::now() - retrieval_started).count();
        if (retrieval.mode == rag::SearchMode::Hybrid) ++hybrid_queries;
        const std::size_t retrieval_rank = first_relevant_rank(
            spans_from_retrieval(retrieval), judgment.gold);
        retrieval_metrics.add(retrieval_rank, options.candidate_k, retrieval_us);

        const auto rerank_started = Clock::now();
        const rag::RerankResult reranked =
            rag::rerank_passages(judgment.query, std::move(retrieval));
        const double rerank_total_us = retrieval_us
            + std::chrono::duration<double, std::micro>(
                Clock::now() - rerank_started).count();
        const std::size_t rerank_rank = first_relevant_rank(
            spans_from_retrieval(reranked.retrieval), judgment.gold);
        rerank_metrics.add(rerank_rank, options.candidate_k, rerank_total_us);

        ++retrieval_changes[rank_change(bm25_rank, retrieval_rank)];
        ++rerank_changes[rank_change(retrieval_rank, rerank_rank)];

        const nlohmann::json report{
            {"q", judgment.query},
            {"ranks", {
                {"bm25_only", bm25_rank},
                {"retrieval", retrieval_rank},
                {"local_rerank", rerank_rank},
            }},
            {"changes", {
                {"retrieval_vs_bm25", rank_change(bm25_rank, retrieval_rank)},
                {"rerank_vs_retrieval", rank_change(retrieval_rank, rerank_rank)},
            }},
        };
        case_reports.push_back(report);
        std::cout << "[quality-case] " << report.dump() << "\n";
    }

    report_header(options.candidate_k);
    report_row("bm25-only", bm25_metrics);
    report_row("retrieval", retrieval_metrics);
    report_row("+local-rerank", rerank_metrics);

    std::cout << "\n  retrieval queries using hybrid mode: "
              << hybrid_queries << "/" << judgments->size() << "\n";
    std::cout << "  rank transitions (retrieval vs bm25): "
              << nlohmann::json(retrieval_changes).dump() << "\n";
    std::cout << "  rank transitions (rerank vs retrieval): "
              << nlohmann::json(rerank_changes).dump() << "\n";
    std::cout
        << "\n  Read each row against the row above it. recall@" << options.candidate_k
        << " is the candidate ceiling; hit@1/MRR/nDCG show ordering quality.\n";

    if (options.output) {
        const auto generated_at_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        const nlohmann::json report{
            {"schema_version", 1},
            {"run", {
                {"label", options.label},
                {"generated_at_unix_ms", generated_at_unix_ms},
                {"candidate_k", options.candidate_k},
                {"embed_model", embed.model},
                {"dense_ready", build.dense_ok},
                {"hybrid_queries", hybrid_queries},
                {"build_ms", build_ms},
            }},
            {"dataset", {
                {"docs_root", options.docs_root.generic_string()},
                {"judgments_path", options.judgments_path.generic_string()},
                {"chunks", corpus.chunk_count()},
                {"cases", judgments->size()},
                {"chunker_identity", rag::kChunkerIdentity},
            }},
            {"stages", {
                {"bm25_only", metrics_json(bm25_metrics, options.candidate_k)},
                {"retrieval", metrics_json(retrieval_metrics, options.candidate_k)},
                {"local_rerank", metrics_json(rerank_metrics, options.candidate_k)},
            }},
            {"transitions", {
                {"retrieval_vs_bm25", retrieval_changes},
                {"rerank_vs_retrieval", rerank_changes},
            }},
            {"cases", std::move(case_reports)},
        };

        const auto written = write_report(*options.output, report);
        if (!written) {
            std::cerr << written.error() << "\n";
            return 2;
        }
        std::cout << "[report] wrote " << options.output->string();
        if (!options.label.empty()) std::cout << " (" << options.label << ")";
        std::cout << "\n";
    }
    return 0;
}
