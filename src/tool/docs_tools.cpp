// 切片 #43（issue #43）search_docs 工具实现。接缝契约见
// include/my_agent/tool/search_docs.hpp；教材对照与两处拍板差异也记在那。
//
// 本文件先落 Red 占位（编译过、断言挂），Green 由用户按
// tests/search_docs_tool_test.cpp 的 Red 测试亲手实现；make_search_docs_tool
// 是纯组装胶水（授权块），直接实现。
#include "my_agent/rag/rag.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/tool.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <filesystem>
#include <vector>

namespace my_agent::tool::docs {
namespace fs = std::filesystem;

// ── Red 占位 ──────────────────────────────────────────────────────────────
// Green 说明（按 Red 测试实现，占位逻辑故意错）：
//   • resolve_config：解析顺序与探词语义见头文件；占位返回全默认值。
//   • docs_tree_fingerprint：门禁镜像 Corpus::build 的 walk；占位恒 0。
//   • render_search_results：格式契约见头文件；占位返回空串。
//   • execute_search_docs：校验→配置检查→lock→漂移才 build→search→render；
//     占位直接报「未实现」。
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

std::string render_search_results(const rag::Corpus& corpus,
                                  const rag::SearchResult& result,
                                  const DocsConfig& cfg) {
    std::string out;
    const auto& chunks = corpus.chunks();

    for (const auto& [id, score] : result.hits) {
        const auto& chunk = chunks[id];

        out += std::format(
            "{}:{}-{} (score {:.4f})\n",
            chunk.path,
            chunk.line_start,
            chunk.line_end,
            score
        );

        out += chunk.text;
        out += "\n\n";
    }

    if (result.hits.empty()) {
        out += "no matching passages\n\n";
    }

    std::string mode;
    if (result.mode == rag::SearchMode::Hybrid) {
        mode = "hybrid";
    } else {
        mode = cfg.embed.model.empty()
            ? "BM25-only (no embed model configured)"
            : "BM25-only (embed unavailable)";
    }

    out += std::format(
        "mode: {} — {} chunks from {}",
        mode,
        corpus.chunk_count(),
        cfg.root.string()
    );

    return out;
}

ExecResult execute_search_docs(const nlohmann::json& args,
                               const DocsConfig& cfg,
                               const rag::EmbedBackend& backend,
                               DocsIndex& index) {
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

    const rag::SearchResult res = index.corpus.search(args["query"].get<std::string>(), backend, cfg.embed, k);
    return ToolOutput{.text = render_search_results(index.corpus, res, cfg)};
}

// ── 组装胶水（授权块）────────────────────────────────────────────────────
ToolDef make_search_docs_tool() {
    return ToolDef{
        .name = "search_docs",
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
        // 诚实全声明（#43 拍板）：读文档 + 缓存文件写进 docs 根 +
        // 配了 embed 模型时打 localhost。默认 profile=Write 全放行。
        .effects = {Effect::ReadFs, Effect::WriteFs, Effect::Net},
        .execute = [](const nlohmann::json& args) -> ExecResult {
            // 进程级状态：与 registry 同寿命。配置不缓存 —— env 会话中
            // 可变，每次调用现场解析。
            static DocsIndex index;
            const DocsConfig cfg = resolve_config(
                [](const char* name) { return std::getenv(name); },
                std::filesystem::current_path(),
                [](const std::filesystem::path& p) {
                    std::error_code ec;
                    const bool ok = std::filesystem::is_directory(p, ec);
                    return !ec && ok;
                });
            return execute_search_docs(args, cfg, rag::ollama_embed, index);
        },
    };
}

} // namespace my_agent::tool::docs
