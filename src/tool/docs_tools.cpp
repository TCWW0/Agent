// 切片 #43（issue #43）search_docs 工具实现。接缝契约见
// include/my_agent/tool/search_docs.hpp；教材对照与两处拍板差异也记在那。
//
// 本文件先落 Red 占位（编译过、断言挂），Green 由用户按
// tests/search_docs_tool_test.cpp 的 Red 测试亲手实现；原生注册 factory
// 直接组装 descriptor 和 handler。索引由注入的共享状态持有。
#include "my_agent/rag/rag.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <filesystem>
#include <vector>
#include <utility>

namespace my_agent::tool::docs {
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kCandidateMultiplier = 4;
constexpr std::size_t kMaxCandidateK = kMaxK * kCandidateMultiplier;

}// namespace

DocsConfig resolve_config(
    const std::function<const char*(const char*)>& getenv,
    const std::filesystem::path& cwd,
    const std::function<bool(const std::filesystem::path&)>& is_directory) {
    DocsConfig cfg;
    if (const char* env = getenv("MY_AGENT_DOCS_DIR"); env && env[0] != '\0'){
        cfg.root = env;
        cfg.root_missing = !is_directory(cfg.root);
    } else {
        const fs::path docs = cwd / "docs";
        if (is_directory(docs)) cfg.root = docs;
    }
    if (const char* m = getenv("MY_AGENT_EMBED_MODEL"); m && m[0] != '\0')
        cfg.embed.model = m;
    if (const char* h = getenv("MY_AGENT_OLLAMA_HOST"); h && h[0] != '\0')
        cfg.embed.host = h;
    if (const char* p = getenv("MY_AGENT_OLLAMA_PORT"); p && p[0] != '\0'){
        int port = 0;
        const auto [end,ec] = std::from_chars(p,p+std::strlen(p),port);
        if (ec == std::errc{} && end == p + std::strlen(p)&& port > 0 && port <= 65535)
            cfg.embed.port = port;
    }
    if (const char* host = getenv("MY_AGENT_RERANK_HOST"); host && host[0] != '\0') {
        cfg.rerank = rag::TeiRerankConfig{};
        cfg.rerank->host = host;

        if (const char* port_text = getenv("MY_AGENT_RERANK_PORT"); port_text && port_text[0] != '\0') {
            int port = 0;
            const char* end = port_text + std::strlen(port_text);
            const auto [parsed_end, error] = std::from_chars(port_text, end, port);

            if (error == std::errc{} && parsed_end == end && port > 0 && port <= 65535) {
                cfg.rerank->port = port;
            }
        }
    }
    return cfg;
}

std::uint64_t docs_tree_fingerprint(const std::filesystem::path& root) {
    struct FileInfo {
        std::string rel;
        std::uint64_t size;
        std::int64_t mtime;
    };

    std::vector<FileInfo> files;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(
            root, fs::directory_options::skip_permission_denied,ec);
            it != fs::recursive_directory_iterator(); it.increment(ec)){
        if (ec) {ec.clear(); continue;}

        const auto& entry = *it;
        std::error_code e2;

        if (entry.is_directory(e2)) {
            if (entry.path().filename().string().starts_with('.'))
                it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file(e2) || entry.path().extension() != ".md") {
            continue;
        }

        const std::string rel = fs::relative(entry.path(), root, e2).string();
        if (e2) {ec.clear(); continue;}
        const auto size = static_cast<std::uint64_t>(entry.file_size(e2));
        if (e2) {ec.clear(); continue;}
        const auto wt = fs::last_write_time(entry.path(),e2);
        if (e2) {ec.clear(); continue;}
        const auto mtime = static_cast<std::int64_t>(wt.time_since_epoch().count());

        files.push_back({rel,size,mtime});
    }

    std::ranges::sort(files,{},&FileInfo::rel);
    constexpr std::uint64_t kFnvOffsetBasis =14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime =1099511628211ULL;

    std::uint64_t fp = kFnvOffsetBasis;
    auto mix = [&fp](std::uint64_t v) {
        fp = (fp ^ v) * kFnvPrime;
    };

    auto mix_bytes = [&mix](std::string_view s) {
        for (unsigned char c : s)
            mix(static_cast<std::uint64_t>(c));
    };

    for (const auto& file : files) {
        mix_bytes(file.rel);
        mix(file.size);
        mix(static_cast<std::uint64_t>(file.mtime));
    }

    return fp;
}

std::string render_search_results(const rag::RerankResult& result,
                                  const DocsConfig& cfg) {
    const rag::RetrievalResult& retrieval = result.retrieval;
    std::string out;

    for (const rag::RankedPassage& passage : retrieval.passages) {
        const rag::Provenance& provenance = passage.provenance;
        const double displayed_score = passage.rerank
            ? passage.rerank->final_score
            : passage.score;

        out += std::format(
            "{}:{}-{} (score {:.4f})\n",
            provenance.path,
            provenance.line_start,
            provenance.line_end,
            displayed_score
        );

        out += passage.text;
        out += "\n\n";
    }

    if (retrieval.passages.empty()) {
        out += "no matching passages\n\n";
    }

    std::string mode;
    if (retrieval.mode == rag::SearchMode::Hybrid) {
        mode = "hybrid";
    } else {
        mode = cfg.embed.model.empty()
            ? "BM25-only (no embed model configured)"
            : "BM25-only (embed unavailable)";
    }

    out += std::format(
        "mode: {} — {} chunks from {}",
        mode,
        retrieval.corpus_chunk_count,
        cfg.root.string()
    );

    if (result.report.mode == rag::RerankMode::ModelFallback) {
        out += "\nrerank: model-fallback";

        if (!result.report.backend_error.empty()) {
            out += " — ";
            out += result.report.backend_error;
        }
    }

    return out;
}

ExecResult execute_search_docs(const nlohmann::json& args,
                               const DocsConfig& cfg,
                               const rag::EmbedBackend& backend,
                               DocsIndex& index,
                               const rag::RerankBackend& rerank_backend) {
    if (!args.is_object() || !args.contains("query") || !args["query"].is_string()
        || args["query"].get<std::string>().empty())
        return std::unexpected(ToolError{ErrorKind::InvalidArgs,
                     "Docs query requires non-empty string query"});

    std::size_t k = kDefaultK;
    if (args.contains("k")) {
        if (!args["k"].is_number_integer()) 
            return std::unexpected(ToolError{ErrorKind::InvalidArgs,
                        "If a K value is provided, it must be an integer."});
        const long long kv = args["k"];
        if (kv < 1 || kv > (long long)kMaxK)
            return std::unexpected(ToolError{ErrorKind::InvalidArgs,
                        "The value of K must be greater than zero and less than "+std::to_string(kMaxK)+"."});
        k = (std::size_t) kv;
    }

    if (cfg.root_missing)
        return std::unexpected(ToolError{ErrorKind::ExecutionFailed,
                    "MY_AGENT_DOCS_DIR points to a missing directory: " + cfg.root.string()});
    
    if (cfg.root.empty())
        return std::unexpected(ToolError{ErrorKind::ExecutionFailed,
                    "no docs configured - set MY_AGENT_DOCS_DIR or create ./docs..."});
    
    std::lock_guard<std::mutex> lock(index.mu);
    const std::uint64_t fp = docs_tree_fingerprint(cfg.root);
    if (index.indexed_root != cfg.root.string() || index.fingerprint != fp){
        index.corpus.build(cfg.root, backend, cfg.embed);
        index.indexed_root = cfg.root.string();
        index.fingerprint = fp;
    }

    const std::string query = args["query"].get<std::string>();

    const std::size_t candidate_k = std::min(k * kCandidateMultiplier, kMaxCandidateK);

    rag::RetrievalResult retrieved = index.corpus.retrieve(query, backend, cfg.embed, candidate_k);

    rag::RerankResult reranked = rag::rerank_passages(query, std::move(retrieved), rerank_backend);

    if (reranked.retrieval.passages.size() > k) {
        reranked.retrieval.passages.resize(k);
    }

    return ToolOutput{
        .text = render_search_results(reranked, cfg),
    };
}

// ── 原生注册 ─────────────────────────────────────────────────────────────
RegisteredTool make_search_docs_tool(
    DocsConfig cfg,
    std::shared_ptr<DocsIndex> index,
    rag::EmbedBackend backend,
    rag::RerankBackend rerank_backend) {
    return RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "search_docs"},
            .description =
                "Search the project's documentation (markdown files under the "
                "configured docs root) for passages relevant to a query. Returns "
                "ranked passages, each with its file path, line range, text and "
                "relevance score. Use `read` on a returned path to see the full "
                "document around a passage. A trailing `mode:` line tells whether "
                "results came from hybrid (lexical+semantic) or BM25-only search.",
        .input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"query", nlohmann::json{
                    {"type", "string"},
                    {"description", "What to look for in the documentation."},
                }},
                {"k", nlohmann::json{
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", static_cast<int>(kMaxK)},
                    {"description", "Number of passages to return (default "
                                    + std::to_string(kDefaultK) + ")."},
                }},
            }},
            {"required", nlohmann::json::array({"query"})},
            {"additionalProperties", false},
        },
            .effects = {Effect::ReadFs, Effect::WriteFs, Effect::Net},
            .replay = {},
        },
        .handler = [cfg = std::move(cfg), index = std::move(index),
                    backend = std::move(backend), rerank_backend = std::move(rerank_backend)](
            const nlohmann::json& args,
            const ToolExecutionContext&) -> ExecResult {
            return execute_search_docs(args, cfg, backend, *index, rerank_backend);
        },
    };
}

} // namespace my_agent::tool::docs
