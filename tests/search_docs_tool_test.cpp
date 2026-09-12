// 切片 #43（issue #43）search_docs 工具 Red 测试。
//
// 语义总纲（Green 按此实现，占位逻辑故意错）：
//   • resolve_config 是纯函数：env 与文件系统探针全注入，测试离线钉死
//     解析顺序（env → ./docs → 空）、配错与零配置的区分、embed 端点语义。
//   • docs_tree_fingerprint 的门禁是 Corpus::build walk 的镜像副本
//     （.md、跳点目录、点文件照收）—— 同步耦合由本文件钉住。
//   • render_search_results 的格式是给模型看的契约：行号出处 + 分数 +
//     mode 量纲声明。分数不归一化（hybrid=RRF 融合分 / BM25-only=原始分，
//     量纲由 mode 行承担）。
//   • execute_search_docs 全链路离线可测：临时目录 + 假 backend，零网络
//     零真实 env。懒建库与漂移重建用「缓存文件 mtime 不动 / 新内容可检」
//     两个可观测量钉住。
#include "my_agent/rag/rag.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <expected>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
namespace rag = my_agent::rag;
using my_agent::tool::docs::DocsConfig;

// ── 夹具 ──────────────────────────────────────────────────────────────────

class TempDir {
public:
    TempDir() {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                    ("my_agent_docs_tool_" + std::to_string(nonce) + "_" +
                     std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(path_, ec)) return;
        }
        throw std::runtime_error{"cannot create docs tool temp dir"};
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

void append_file(const fs::path& p, const std::string& body) {
    std::ofstream f(p, std::ios::app);
    f << body;
}

// 主题词互斥的固定语料：检索断言才有唯一答案。
constexpr const char* kDocFlux =
    "fluxonium circuits store qubits in a nonlinear inductor\n"
    "coherence times improve with better shielding\n";
constexpr const char* kDocTides =
    "harbor tides follow the lunar schedule closely\n"
    "beacons guide ships through the channel at night\n";
constexpr const char* kDocEmber =
    "ember kilns fire pottery over three slow days\n"
    "temperature curves matter more than fuel choice\n";

// env 探针：map 命中返回对应串，未命中 nullptr。
class EnvMap {
public:
    EnvMap& set(const char* name, const char* value) {
        entries_[name] = value;
        return *this;
    }
    [[nodiscard]] const char* get(const char* name) const {
        if (auto it = entries_.find(name); it != entries_.end())
            return it->second.c_str();
        return nullptr;
    }
    [[nodiscard]] std::function<const char*(const char*)> getenv() const {
        return [this](const char* name) { return get(name); };
    }

private:
    std::map<std::string, std::string> entries_;
};

// 目录探针：只有命中集合里的路径返回 true。
class DirProbe {
public:
    DirProbe& add(const fs::path& p) {
        dirs_.insert(p);
        return *this;
    }
    [[nodiscard]] std::function<bool(const fs::path&)> is_directory() const {
        return [this](const fs::path& p) {
            return dirs_.find(p) != dirs_.end();
        };
    }
    [[nodiscard]] static std::function<bool(const fs::path&)> none() {
        return [](const fs::path&) { return false; };
    }

private:
    std::set<fs::path> dirs_;
};

// 假 backend：Doc 角色按标记词出正交向量，Query 角色固定打 fluxonium 轴
// —— 词法零交集的查询靠语义路命中，hybrid 的价值就在这一格。
class MarkerBackend {
public:
    std::size_t calls = 0;

    [[nodiscard]] rag::EmbedBackend fn() {
    return [this](const rag::EmbedConfig&,
                    const std::vector<std::string>& texts,
                    rag::EmbedRole role)
        -> std::expected<std::vector<std::vector<float>>, std::string> {
        ++calls;
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (const std::string& t : texts) {
            if (t.find("fluxonium") != std::string::npos)
                out.push_back({1.f, 0.f, 0.f});
            else if (t.find("harbor") != std::string::npos)
                out.push_back({0.f, 1.f, 0.f});
            else if (t.find("ember") != std::string::npos)
                out.push_back({0.f, 0.f, 1.f});
            else if (role == rag::EmbedRole::Query)
                out.push_back({1.f, 0.f, 0.f});   // 查询永远指向 fluxonium
            else
                out.push_back({0.5f, 0.5f, 0.5f});
        }
        return out;
        };
    }
};

// 断言「BM25-only 全链路不碰 embed」的哨兵 backend：被调用即失败。
struct FailIfCalledBackend {
    [[nodiscard]] rag::EmbedBackend fn() {
        return [this](const rag::EmbedConfig&,
                      const std::vector<std::string>&,
                      rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            called = true;
            return std::unexpected(std::string{"BM25-only 不该调 embed"});
        };
    }
    bool called = false;
};

DocsConfig cfg_for_root(const fs::path& root) {
    DocsConfig cfg;
    cfg.root = root;
    return cfg;
}

// ── resolve_config：纯函数，离线 ──────────────────────────────────────────

TEST(SearchDocsConfig, EnvDocsDirWinsAndExistenceIsProbed) {
    const EnvMap env = EnvMap{}.set("MY_AGENT_DOCS_DIR", "/x/docs");
    const DirProbe probe;   // 空探针：什么目录都不存在
    const DocsConfig cfg =
        my_agent::tool::docs::resolve_config(env.getenv(), "/w", probe.none());

    EXPECT_EQ(cfg.root, fs::path{"/x/docs"});
    EXPECT_TRUE(cfg.root_missing);   // env 指向不存在目录 = 配置错误
}

TEST(SearchDocsConfig, EmptyEnvStringFallsBackToCwdDocs) {
    const EnvMap env = EnvMap{}.set("MY_AGENT_DOCS_DIR", "");
    const DirProbe probe = DirProbe{}.add("/w/docs");
    const DocsConfig cfg =
        my_agent::tool::docs::resolve_config(env.getenv(), "/w", probe.is_directory());

    EXPECT_EQ(cfg.root, fs::path{"/w/docs"});
    EXPECT_FALSE(cfg.root_missing);
}

TEST(SearchDocsConfig, NoConfigYieldsEmptyRootNotError) {
    const DocsConfig cfg = my_agent::tool::docs::resolve_config(
        EnvMap{}.getenv(), "/w", DirProbe{}.none());

    EXPECT_TRUE(cfg.root.empty());
    EXPECT_FALSE(cfg.root_missing);   // 零配置是合法初始态，不是配错
}

TEST(SearchDocsConfig, MissingCwdDocsIsAlsoJustNoConfig) {
    // ./docs 不存在 ≠ 配错 —— 只有显式 env 指空才置 root_missing
    const DocsConfig cfg = my_agent::tool::docs::resolve_config(
        EnvMap{}.getenv(), "/w", DirProbe{}.none());
    EXPECT_TRUE(cfg.root.empty());
    EXPECT_FALSE(cfg.root_missing);
}

TEST(SearchDocsConfig, EmbedModelEmptyMeansBm25Only) {
    const DocsConfig unset = my_agent::tool::docs::resolve_config(
        EnvMap{}.getenv(), "/w", DirProbe{}.none());
    EXPECT_TRUE(unset.embed.model.empty());

    const DocsConfig empty = my_agent::tool::docs::resolve_config(
        EnvMap{}.set("MY_AGENT_EMBED_MODEL", "").getenv(), "/w",
        DirProbe{}.none());
    EXPECT_TRUE(empty.embed.model.empty());
}

TEST(SearchDocsConfig, EmbedModelEnvHonored) {
    const DocsConfig cfg = my_agent::tool::docs::resolve_config(
        EnvMap{}.set("MY_AGENT_EMBED_MODEL", "bge-m3").getenv(), "/w",
        DirProbe{}.none());
    EXPECT_EQ(cfg.embed.model, "bge-m3");
}

TEST(SearchDocsConfig, OllamaHostPortHonoredWithDefaults) {
    const DocsConfig dflt = my_agent::tool::docs::resolve_config(
        EnvMap{}.getenv(), "/w", DirProbe{}.none());
    EXPECT_EQ(dflt.embed.host, "localhost");
    EXPECT_EQ(dflt.embed.port, 11434);

    const DocsConfig cfg = my_agent::tool::docs::resolve_config(
        EnvMap{}
            .set("MY_AGENT_OLLAMA_HOST", "ollama.local")
            .set("MY_AGENT_OLLAMA_PORT", "11400")
            .getenv(),
        "/w", DirProbe{}.none());
    EXPECT_EQ(cfg.embed.host, "ollama.local");
    EXPECT_EQ(cfg.embed.port, 11400);
}

TEST(SearchDocsConfig, GarbagePortFallsBackToDefault) {
    const DocsConfig cfg = my_agent::tool::docs::resolve_config(
        EnvMap{}.set("MY_AGENT_OLLAMA_PORT", "not-a-port").getenv(), "/w",
        DirProbe{}.none());
    EXPECT_EQ(cfg.embed.port, 11434);   // 配置垃圾不崩工具
}

// ── docs_tree_fingerprint：门禁镜像 build 的 walk ────────────────────────

TEST(DocsFingerprint, StableWithoutChanges) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_EQ(fp1, fp2);
}

TEST(DocsFingerprint, MarkdownEditDrifts) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    append_file(dir.path() / "a.md", "a new tail line changes size\n");
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_NE(fp1, fp2);
}

TEST(DocsFingerprint, NonMarkdownAndCacheFileIgnored) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    write_file(dir.path() / "notes.txt", kDocTides);
    write_file(dir.path() / ".my_agent_rag_cache.bin",
               std::string(64, '\xAB'));   // build 写缓存不得自我触发漂移
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_EQ(fp1, fp2);
}

TEST(DocsFingerprint, DotDirectoriesIgnored) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    write_file(dir.path() / ".git" / "hook.md", kDocTides);
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_EQ(fp1, fp2);
}

TEST(DocsFingerprint, HiddenMarkdownCounts) {
    // build 只跳点目录不跳点文件：.hidden.md 参与索引，指纹也必须收
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    write_file(dir.path() / ".hidden.md", kDocTides);
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_NE(fp1, fp2);
}

TEST(DocsFingerprint, NestedMarkdownCounts) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocFlux);
    const auto fp1 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    write_file(dir.path() / "sub" / "deep" / "c.md", kDocEmber);
    const auto fp2 = my_agent::tool::docs::docs_tree_fingerprint(dir.path());
    EXPECT_NE(fp1, fp2);
}

// ── render_search_results：给模型的输出契约 ──────────────────────────────

class RenderFixture : public ::testing::Test {
protected:
    void SetUp() override {
        corpus_.build_from_memory(
            {{"flux.md", kDocFlux}, {"tides.md", kDocTides}},
            [](const rag::EmbedConfig&, const std::vector<std::string>&,
               rag::EmbedRole)
                -> std::expected<std::vector<std::vector<float>>, std::string> {
                return std::unexpected(std::string{"render 夹具不嵌"});
            },
            rag::EmbedConfig{});   // model 空 → BM25-only，chunks 照常有
        ASSERT_GE(corpus_.chunk_count(), 2u);
    }

    [[nodiscard]] std::string header_of(std::size_t chunk_id) const {
        const rag::Chunk& c = corpus_.chunks()[chunk_id];
        return c.path + ":" + std::to_string(c.line_start) + "-" +
               std::to_string(c.line_end);
    }

    rag::Corpus corpus_;
    DocsConfig cfg_;
};

TEST_F(RenderFixture, PassageCarriesPathLineRangeScoreAndText) {
    cfg_.root = "/w/docs";
    rag::SearchResult r;
    r.mode = rag::SearchMode::Hybrid;
    r.hits = {{0, 0.0312}};
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);

    EXPECT_NE(out.find(header_of(0) + " (score 0.0312)"), std::string::npos);
    EXPECT_NE(out.find("fluxonium circuits store qubits"),
              std::string::npos);   // 正文要真的在
    EXPECT_NE(out.find("mode: hybrid"), std::string::npos);
}

TEST_F(RenderFixture, PassagesComeInRankedOrder) {
    cfg_.root = "/w/docs";
    rag::SearchResult r;
    r.mode = rag::SearchMode::Hybrid;
    r.hits = {{1, 0.03}, {0, 0.02}};
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);

    const auto pos1 = out.find(header_of(1));
    const auto pos0 = out.find(header_of(0));
    ASSERT_NE(pos1, std::string::npos);
    ASSERT_NE(pos0, std::string::npos);
    EXPECT_LT(pos1, pos0);   // 名次序，不是 chunk-id 序
}

TEST_F(RenderFixture, Bm25OnlyWithoutModelSaysWhy) {
    cfg_.root = "/w/docs";
    rag::SearchResult r;
    r.mode = rag::SearchMode::Bm25Only;
    r.hits = {{0, 12.7}};
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);
    EXPECT_NE(out.find("mode: BM25-only (no embed model configured)"),
              std::string::npos);
}

TEST_F(RenderFixture, Bm25OnlyWithModelSaysEmbedUnavailable) {
    cfg_.root = "/w/docs";
    cfg_.embed.model = "nomic-embed-text";
    rag::SearchResult r;
    r.mode = rag::SearchMode::Bm25Only;   // 配了模型但这次降级（瞬断）
    r.hits = {{0, 12.7}};
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);
    EXPECT_NE(out.find("mode: BM25-only (embed unavailable)"),
              std::string::npos);
}

TEST_F(RenderFixture, ModeLineCarriesProvenance) {
    cfg_.root = "/w/docs";
    rag::SearchResult r;
    r.mode = rag::SearchMode::Hybrid;
    r.hits = {{0, 0.03}};
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);
    EXPECT_NE(out.find(std::to_string(corpus_.chunk_count()) +
                       " chunks from /w/docs"),
              std::string::npos);   // 空语料时模型据此不信任零结果
}

TEST_F(RenderFixture, EmptyHitsSayNoMatch) {
    cfg_.root = "/w/docs";
    rag::SearchResult r;   // 空命中
    r.mode = rag::SearchMode::Bm25Only;
    const std::string out =
        my_agent::tool::docs::render_search_results(corpus_, r, cfg_);
    EXPECT_NE(out.find("no matching passages"), std::string::npos);
    EXPECT_NE(out.find("mode: BM25-only"), std::string::npos);
}

// ── execute_search_docs：全链路，临时目录 + 假 backend，零网络 ───────────

TEST(ExecuteSearchDocs, RejectsBadArgsBeforeAnythingElse) {
    my_agent::tool::docs::DocsIndex index;
    const DocsConfig cfg;   // 零配置也能先报参数错 —— 校验在最前
    FailIfCalledBackend backend;

    const nlohmann::json bad[] = {
        nlohmann::json{},                           // 没 query
        nlohmann::json{{"query", 42}},              // query 不是 string
        nlohmann::json{{"query", ""}},              // 空 query
        nlohmann::json{{"query", "x"}, {"k", 0}},   // k 下界
        nlohmann::json{{"query", "x"},
                       {"k", 21}},                  // k 上界（kMaxK=20）
        nlohmann::json{{"query", "x"}, {"k", 1.5}}, // k 不是整数
        nlohmann::json{{"query", "x"}, {"k", "5"}}, // k 不是数字
    };
    for (const auto& args : bad) {
        const auto result = my_agent::tool::docs::execute_search_docs(
            args, cfg, backend.fn(), index);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, my_agent::tool::ErrorKind::InvalidArgs)
            << "args=" << args.dump();
    }
    EXPECT_FALSE(backend.called);   // 参数错连 embed 都不该碰
}

TEST(ExecuteSearchDocs, ZeroConfigIsActionableError) {
    my_agent::tool::docs::DocsIndex index;
    const DocsConfig cfg;   // root 空
    FailIfCalledBackend backend;
    const auto result = my_agent::tool::docs::execute_search_docs(
        nlohmann::json{{"query", "anything"}}, cfg, backend.fn(), index);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, my_agent::tool::ErrorKind::ExecutionFailed);
    EXPECT_NE(result.error().message.find("MY_AGENT_DOCS_DIR"),
              std::string::npos);   // 告诉用户怎么配
    EXPECT_NE(result.error().message.find("./docs"), std::string::npos);
    EXPECT_FALSE(backend.called);
}

TEST(ExecuteSearchDocs, MissingConfiguredDirNamesThePath) {
    my_agent::tool::docs::DocsIndex index;
    DocsConfig cfg = cfg_for_root("/no/such/docs");
    cfg.root_missing = true;
    FailIfCalledBackend backend;
    const auto result = my_agent::tool::docs::execute_search_docs(
        nlohmann::json{{"query", "anything"}}, cfg, backend.fn(), index);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, my_agent::tool::ErrorKind::ExecutionFailed);
    EXPECT_NE(result.error().message.find("/no/such/docs"),
              std::string::npos);   // 报出配错的路径
    EXPECT_FALSE(backend.called);
}

TEST(ExecuteSearchDocs, Bm25OnlyEndToEndWithoutTouchingEmbed) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    write_file(dir.path() / "tides.md", kDocTides);
    my_agent::tool::docs::DocsIndex index;
    FailIfCalledBackend backend;   // model 空 = 全链路零 embed 调用

    const auto result = my_agent::tool::docs::execute_search_docs(
        nlohmann::json{{"query", "harbor tides"}},
        cfg_for_root(dir.path()), backend.fn(), index);
    ASSERT_TRUE(result.has_value()) << result.error().render();

    const std::string& out = result->text;
    EXPECT_NE(out.find("tides.md:1-"), std::string::npos);   // 行号出处
    EXPECT_NE(out.find("lunar schedule"), std::string::npos);
    EXPECT_NE(out.find("mode: BM25-only (no embed model configured)"),
              std::string::npos);
    EXPECT_EQ(out.find("flux.md:"), std::string::npos);   // 主题词互斥
    EXPECT_FALSE(backend.called);   // BM25-only 一次都不嵌
}

TEST(ExecuteSearchDocs, HybridFindsWhatLexicalCannot) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    write_file(dir.path() / "tides.md", kDocTides);
    write_file(dir.path() / "ember.md", kDocEmber);
    my_agent::tool::docs::DocsIndex index;
    MarkerBackend backend;

    DocsConfig cfg = cfg_for_root(dir.path());
    cfg.embed.model = "fake-embed";

    // 「传送门」与三份 ASCII 文档词法零交集（CJK bigram vs ASCII token）——
    // 词法路必然空手，命中只能来自语义路（Query 角色指向 fluxonium 轴）。
    // hybrid 的存在意义就在这一格。
    const auto result = my_agent::tool::docs::execute_search_docs(
        nlohmann::json{{"query", "传送门"}}, cfg, backend.fn(), index);
    ASSERT_TRUE(result.has_value()) << result.error().render();

    const std::string& out = result->text;
    EXPECT_NE(out.find("flux.md:1-"), std::string::npos);
    EXPECT_NE(out.find("mode: hybrid"), std::string::npos);
    EXPECT_EQ(out.find("tides.md:"), std::string::npos);
    EXPECT_EQ(out.find("ember.md:"), std::string::npos);
    EXPECT_GT(backend.calls, 0u);   // 语义路真的干了活
}

TEST(ExecuteSearchDocs, SecondCallSkipsRebuildWhenNoDrift) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    my_agent::tool::docs::DocsIndex index;
    FailIfCalledBackend backend;

    const auto args = nlohmann::json{{"query", "fluxonium"}};
    ASSERT_TRUE(my_agent::tool::docs::execute_search_docs(
                    args, cfg_for_root(dir.path()), backend.fn(), index)
                    .has_value());
    const fs::path cache = dir.path() / ".my_agent_rag_cache.bin";
    ASSERT_TRUE(fs::exists(cache));   // 首次调用建了库（懒建库的物证）

    // 无漂移的第二次调用不得重建：重建会重写缓存文件 → mtime 变。
    // 无漂移的指纹 walk 是毫秒级，1.4s 的缓存解析+BM25 重建必须跳过。
    std::error_code ec;
    const auto mtime1 = fs::last_write_time(cache, ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(my_agent::tool::docs::execute_search_docs(
                    args, cfg_for_root(dir.path()), backend.fn(), index)
                    .has_value());
    const auto mtime2 = fs::last_write_time(cache, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(mtime1, mtime2);   // 缓存没被重写 = 没重建
}

TEST(ExecuteSearchDocs, DriftEditBecomesSearchable) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    my_agent::tool::docs::DocsIndex index;
    FailIfCalledBackend backend;
    const DocsConfig cfg = cfg_for_root(dir.path());

    const auto query = nlohmann::json{{"query", "zephyrium"}};
    {
        const auto r = my_agent::tool::docs::execute_search_docs(
            query, cfg, backend.fn(), index);
        ASSERT_TRUE(r.has_value());
        EXPECT_NE(r->text.find("no matching passages"), std::string::npos);
    }

    // 会话中改文档：下次调用必须增量重建并检索到新内容，不服务过期 chunks
    append_file(dir.path() / "flux.md",
                "zephyrium admixture doubles the anharmonicity\n");
    const auto r2 = my_agent::tool::docs::execute_search_docs(
        query, cfg, backend.fn(), index);
    ASSERT_TRUE(r2.has_value()) << r2.error().render();
    EXPECT_NE(r2->text.find("zephyrium"), std::string::npos);
    EXPECT_NE(r2->text.find("doubles the anharmonicity"), std::string::npos);
}

TEST(ExecuteSearchDocs, DriftDeleteDropsPassages) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    write_file(dir.path() / "tides.md", kDocTides);
    my_agent::tool::docs::DocsIndex index;
    FailIfCalledBackend backend;
    const DocsConfig cfg = cfg_for_root(dir.path());

    const auto query = nlohmann::json{{"query", "harbor tides"}};
    {
        const auto r = my_agent::tool::docs::execute_search_docs(
            query, cfg, backend.fn(), index);
        ASSERT_TRUE(r.has_value());
        EXPECT_NE(r->text.find("tides.md:1-"), std::string::npos);
    }

    std::error_code ec;
    fs::remove(dir.path() / "tides.md", ec);
    ASSERT_FALSE(ec);
    const auto r2 = my_agent::tool::docs::execute_search_docs(
        query, cfg, backend.fn(), index);
    ASSERT_TRUE(r2.has_value()) << r2.error().render();
    EXPECT_EQ(r2->text.find("tides.md:"), std::string::npos);   // 删除出局
}

TEST(ExecuteSearchDocs, DefaultKTruncatesToFive) {
    TempDir dir;
    // 8 个互不重叠块的文档，每个块都含 gamma —— BM25 必然全部命中，
    // 截断只能靠 k（默认 5）。
    std::string body;
    for (int block = 0; block < 8; ++block) {
        body += "# section " + std::to_string(block) + "\n";
        for (int line = 0; line < 44; ++line) body += "gamma padding line\n";
    }
    write_file(dir.path() / "big.md", body);

    my_agent::tool::docs::DocsIndex index;
    FailIfCalledBackend backend;
    const auto r = my_agent::tool::docs::execute_search_docs(
        nlohmann::json{{"query", "gamma"}}, cfg_for_root(dir.path()),
        backend.fn(), index);
    ASSERT_TRUE(r.has_value()) << r.error().render();
    // 数 "(score " 出现次数 = 段落数（mode 行的括号不背锅）
    std::size_t passages = 0;
    for (std::size_t pos = 0;
         (pos = r->text.find("(score ", pos)) != std::string::npos;
         pos += 7)
        ++passages;
    EXPECT_EQ(passages, my_agent::tool::docs::kDefaultK);
}

TEST(ExecuteSearchDocs, ConcurrentCallsSerializeSafely) {
    TempDir dir;
    write_file(dir.path() / "flux.md", kDocFlux);
    write_file(dir.path() / "tides.md", kDocTides);
    my_agent::tool::docs::DocsIndex index;   // 共享：锁的用武之地
    FailIfCalledBackend backend;
    const DocsConfig cfg = cfg_for_root(dir.path());

    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&index, &cfg, &backend, t]() {
            const auto r = my_agent::tool::docs::execute_search_docs(
                nlohmann::json{{"query", t == 0 ? "fluxonium" : "harbor"}},
                cfg, backend.fn(), index);
            EXPECT_TRUE(r.has_value());
        });
    }
    for (auto& th : threads) th.join();
}

// ── registry 接线（胶水面）────────────────────────────────────────────────

TEST(SearchDocsRegistration, SchemaAndEffectsDeclared) {
    const my_agent::tool::ToolDef* def = my_agent::tool::find("search_docs");
    ASSERT_NE(def, nullptr);
    EXPECT_FALSE(def->description.empty());

    EXPECT_TRUE(def->input_schema.contains("properties"));
    EXPECT_TRUE(def->input_schema["properties"].contains("query"));
    EXPECT_TRUE(def->input_schema["properties"].contains("k"));
    ASSERT_TRUE(def->input_schema.contains("required"));
    ASSERT_TRUE(def->input_schema["required"].is_array());
    ASSERT_EQ(def->input_schema["required"].size(), 1u);
    EXPECT_EQ(def->input_schema["required"][0], "query");

    // 诚实全声明（#43 拍板）：读文档 + 写缓存 + localhost embed
    EXPECT_TRUE(def->effects.has(my_agent::tool::Effect::ReadFs));
    EXPECT_TRUE(def->effects.has(my_agent::tool::Effect::WriteFs));
    EXPECT_TRUE(def->effects.has(my_agent::tool::Effect::Net));
}

TEST(SearchDocsRegistration, ZeroConfigThroughRealRegistry) {
    // 真注册表走一遍零配置路径（不崩 + 可操作错误）。
    // 测试进程 cwd 有 ./docs 或设了 MY_AGENT_DOCS_DIR 时无零配置可言，跳过。
    if (std::getenv("MY_AGENT_DOCS_DIR") != nullptr ||
        fs::is_directory(fs::path{"docs"})) {
        GTEST_SKIP() << "docs 已配置，零配置路径不可测";
    }
    const auto result = my_agent::tool::execute(
        "search_docs", nlohmann::json{{"query", "anything"}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("MY_AGENT_DOCS_DIR"),
              std::string::npos);
}

} // namespace
