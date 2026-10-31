// TS-003 Red：会话服务（SessionFacts → SessionServices）。
// 契约见 docs/tool-system/TS-003-CONTRACT.md §6。
//
// 这一片修的不是一个可观测 bug。今天 registry.cpp 与 repl/main.cpp 各自
// discover 一遍环境，恰好指向同一批路径，所以行为上看不出差别；差别只在
// **环境在会话中途变化**时显现。因此本文件每个用例都注入临时根，并在构造之后
// 主动破坏环境（chdir / setenv）—— 一旦实现退回「用的时候现发现」，断言就会
// 指向真实环境而红。
//
// 对抗性夹具的刻意安排：
//   workspace = <base>/<case>/ws
//   skill 根  = <base>/<case>/skills        刻意在工作区之外，
//                                           read 的 allowed_roots 才有可测的边界
//   memory 根 = <base>/<case>/memory-{user,project}
//   docs 根   = <base>/<case>/docs
// 全部落在测试工作目录下的独立临时夹具：/tmp 是 read 的默认允许根，
// 因此会话隔离测试必须把 skill 根放在 /tmp 之外。
//
// 预期失败形态是**编译失败**：session_services.hpp 尚不存在。
#include "my_agent/tool/session_services.hpp"

#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace docs = my_agent::tool::docs;
namespace memory = my_agent::tool::memory;
namespace rag = my_agent::rag;

using my_agent::tool::ErrorKind;
using my_agent::tool::ExecResult;
using my_agent::tool::RegisteredTool;
using my_agent::tool::SessionFacts;
using my_agent::tool::SessionServices;
using my_agent::tool::ToolError;
using my_agent::tool::ToolExecutionContext;

class SessionServicesTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        original_cwd_ = fs::current_path();

        // 环境必须是已知状态：本文件只允许会话自己决定文档根在哪。
        save_env("MY_AGENT_DOCS_DIR");
        save_env("MY_AGENT_EMBED_MODEL");
        ::unsetenv("MY_AGENT_DOCS_DIR");
        ::unsetenv("MY_AGENT_EMBED_MODEL");

        base_ = original_cwd_
            / ("my_agent_session_test_" + std::to_string(::getpid()) + "_"
               + ::testing::UnitTest::GetInstance()
                     ->current_test_info()
                     ->name());
        fs::remove_all(base_);
        fs::create_directories(base_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::current_path(original_cwd_, ec);
        fs::remove_all(base_, ec);
        restore_env();
    }

    // 一个独立会话的全部环境事实。skill 根刻意放在工作区之外 —— 否则
    // read 的 allowed_roots 与 workspace 边界重合，就测不出是哪条在放行。
    [[nodiscard]] SessionFacts facts(std::string_view label) const
    {
        const fs::path base = base_ / std::string{label};
        fs::create_directories(base / "ws");
        fs::create_directories(base / "skills");
        fs::create_directories(base / "docs");
        return SessionFacts{
            .workspace_root = base / "ws",
            .memory_roots =
                memory::Roots{
                    .user = base / "memory-user",
                    .project = base / "memory-project",
                },
            .skill_roots = {base / "skills"},
            .docs =
                docs::DocsConfig{
                    .root = base / "docs",
                    .root_missing = false,
                },
        };
    }

    static void write_file(const fs::path& path, std::string_view contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary};
        out << contents;
    }

    static void write_skill(
        const fs::path& root,
        std::string_view slug,
        std::string_view contents
    )
    {
        write_file(root / std::string{slug} / "SKILL.md", contents);
    }

    // 按名字从会话的注册里取工具来跑。越界即失败 —— 会话没这个工具本身就是
    // 契约违背，不该被静默跳过。
    static ExecResult run(
        const SessionServices& services,
        std::string_view name,
        const nlohmann::json& args
    )
    {
        for (const RegisteredTool& registration : services.registrations()) {
            if (registration.descriptor.name.value == name) {
                return registration.handler(args, ToolExecutionContext{});
            }
        }
        ADD_FAILURE() << "会话里没有工具 " << name;
        return std::unexpected(
            ToolError{.kind = ErrorKind::NotFound, .message = "no such tool"}
        );
    }

    // 数据成员必须 protected：TEST_F 的函数体属于派生类，碰不到 private。
    fs::path original_cwd_;
    fs::path base_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_env_;

private:
    void save_env(const char* name)
    {
        if (const char* value = std::getenv(name)) {
            saved_env_.emplace_back(name, std::string{value});
        } else {
            saved_env_.emplace_back(name, std::nullopt);
        }
    }

    void restore_env()
    {
        for (const auto& [name, value] : saved_env_) {
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
        saved_env_.clear();
    }
};

// ── 测试接缝 + AC 2：两个会话的事实互不可见 ──────────────────────────────

TEST_F(SessionServicesTest, TwoSessionsKeepSeparateMemory)
{
    const SessionFacts a_facts = facts("a");
    const SessionFacts b_facts = facts("b");
    const SessionServices a{a_facts};
    const SessionServices b{b_facts};

    const ExecResult stored = run(
        a, "remember", nlohmann::json{{"text", "session A only"}, {"scope", "user"}}
    );
    ASSERT_TRUE(stored.has_value()) << stored.error().render();

    // 走工具写下的那条，必须立刻出现在同一会话的提示事实里：load_all()
    // 每次读盘，所以「实例稳定」不该以「答案陈旧」为代价。
    const std::vector<std::string> lines_a = a.memory_lines();
    ASSERT_EQ(1u, lines_a.size()) << "A 应该只看到自己写的那一条";
    EXPECT_NE(std::string::npos, lines_a.front().find("session A only"))
        << lines_a.front();

    EXPECT_TRUE(b.memory_lines().empty())
        << "B 的 memory 根是另一份，不该看见 A 写下的事实";

    // 落点断言：文件必须落在注入的临时根里。实现一旦退回 discover_roots()，
    // 它会去写真实 $HOME —— 这两条就是那件事的探针。
    EXPECT_TRUE(fs::exists(a_facts.memory_roots.user))
        << "remember 没有写进会话自己的 memory 根：" << a_facts.memory_roots.user;
    EXPECT_FALSE(fs::exists(b_facts.memory_roots.user))
        << "B 什么都没写，它的根不该被创建";
}

TEST_F(SessionServicesTest, RegistersAllNativeToolsInStableOrder)
{
    const SessionFacts session_facts = facts("ordered-registrations");
    const fs::path extra_read_root =
        session_facts.workspace_root.parent_path() / "extra-read-root";
    write_file(extra_read_root / "visible.txt", "visible through shared roots\n");

    SessionServices services{session_facts};
    const std::vector<RegisteredTool> registrations = services.registrations();
    constexpr std::array<std::string_view, 7> expected_names{
        "calculator", "read", "list_dir", "remember", "forget", "skill", "search_docs",
    };

    ASSERT_EQ(expected_names.size(), registrations.size());
    for (std::size_t i = 0; i < registrations.size(); ++i) {
        const auto& descriptor = registrations[i].descriptor;
        EXPECT_EQ(expected_names[i], descriptor.name.value);
        EXPECT_TRUE(static_cast<bool>(registrations[i].handler));
        EXPECT_EQ(my_agent::ToolReplayMode::HeadTail, descriptor.replay.mode);
        EXPECT_EQ(std::uint32_t{1}, descriptor.replay.weight);
    }

    const auto& calculator = registrations[0].descriptor;
    EXPECT_EQ("Multiply two numbers.", calculator.description);
    EXPECT_EQ(my_agent::tool::EffectSet{}, calculator.effects);
    EXPECT_EQ("multiply", calculator.input_schema["properties"]["operation"]["enum"][0]);

    const auto& read = registrations[1].descriptor;
    EXPECT_TRUE(read.effects.has(my_agent::tool::Effect::ReadFs));
    ASSERT_TRUE(read.input_schema["required"].is_array());
    ASSERT_EQ(1u, read.input_schema["required"].size());
    EXPECT_EQ("path", read.input_schema["required"][0]);

    const auto& list_dir = registrations[2];
    EXPECT_TRUE(list_dir.descriptor.effects.has(my_agent::tool::Effect::ReadFs));
    ASSERT_TRUE(services.allow_read_root(extra_read_root).has_value());
    const ExecResult listed = list_dir.handler(
        nlohmann::json{{"path", extra_read_root.string()}},
        ToolExecutionContext{});
    ASSERT_TRUE(listed.has_value()) << listed.error().render();
    EXPECT_EQ("\"visible.txt\"  29 B\n", listed->text);

    EXPECT_TRUE(registrations[3].descriptor.effects.has(my_agent::tool::Effect::WriteFs));
    EXPECT_TRUE(registrations[4].descriptor.effects.has(my_agent::tool::Effect::WriteFs));
    EXPECT_TRUE(registrations[5].descriptor.effects.has(my_agent::tool::Effect::ReadFs));

    const auto& search_docs = registrations[6].descriptor;
    EXPECT_TRUE(search_docs.input_schema["properties"].contains("query"));
    EXPECT_TRUE(search_docs.effects.has(my_agent::tool::Effect::ReadFs));
    EXPECT_TRUE(search_docs.effects.has(my_agent::tool::Effect::WriteFs));
    EXPECT_TRUE(search_docs.effects.has(my_agent::tool::Effect::Net));
}

// ── AC 3：skill 目录块与 skill 工具都认本会话的 engine ────────────────────

TEST_F(SessionServicesTest, TwoSessionsKeepSeparateSkillCatalogs)
{
    const SessionFacts a_facts = facts("a");
    const SessionFacts b_facts = facts("b");
    write_skill(
        a_facts.skill_roots.front(),
        "flux-capacitor",
        "---\nname: flux-capacitor\ndescription: Stops time paradoxes\n---\nthe body\n"
    );

    const SessionServices a{a_facts};
    const SessionServices b{b_facts};

    EXPECT_NE(std::string::npos, a.skill_catalog().find("flux-capacitor"));
    EXPECT_EQ(std::string::npos, b.skill_catalog().find("flux-capacitor"))
        << "B 的 skill 根里没有这份 skill，目录块不该出现它：\n"
        << b.skill_catalog();

    // 目录块只列名字，正文要 activate 才拿得到 —— 工具也必须认这同一份 engine。
    const ExecResult activated =
        run(a, "skill", nlohmann::json{{"name", "flux-capacitor"}});
    ASSERT_TRUE(activated.has_value()) << activated.error().render();
    EXPECT_NE(std::string::npos, activated->text.find("the body"))
        << activated->text;

    const ExecResult missing =
        run(b, "skill", nlohmann::json{{"name", "flux-capacitor"}});
    EXPECT_FALSE(missing.has_value())
        << "同名 skill 不在 B 的根里，B 不该能激活它";
}

// ── AC 3：read 的 allowed_roots 来自本会话的 engine（Tier3 边界）──────────

TEST_F(SessionServicesTest, ReadAllowedRootsComeFromTheSessionsOwnEngine)
{
    const SessionFacts a_facts = facts("a");
    write_skill(
        a_facts.skill_roots.front(),
        "pdf",
        "---\nname: pdf\ndescription: PDFs\n---\nbody\n"
    );
    const fs::path resource = a_facts.skill_roots.front() / "pdf" / "notes.txt";
    write_file(resource, "resource in a skill directory\n");

    const SessionServices a{a_facts};
    const SessionServices b{facts("b")};

    // skill 目录刻意在工作区之外：只有 allowed_roots 放行才读得到。
    const ExecResult ok = run(a, "read", nlohmann::json{{"path", resource.string()}});
    ASSERT_TRUE(ok.has_value()) << ok.error().render();
    EXPECT_NE(std::string::npos, ok->text.find("resource in a skill directory"));

    const ExecResult denied =
        run(b, "read", nlohmann::json{{"path", resource.string()}});
    ASSERT_FALSE(denied.has_value())
        << "B 的 engine 没有这份 skill，这条路径应被工作区边界拒掉";
    EXPECT_EQ(ErrorKind::OutOfWorkspace, denied.error().kind)
        << denied.error().render();
}

TEST_F(SessionServicesTest, SkillDiscoveredAfterRuntimeCreationCanReadItsResource)
{
    const SessionFacts session_facts = facts("late-skill");
    const SessionServices services{session_facts};
    const my_agent::tool::ToolRuntime runtime{services.registrations()};
    EXPECT_TRUE(services.skill_catalog().empty());

    write_skill(session_facts.skill_roots.front(), "new-skill",
                "---\nname: new-skill\ndescription: New instructions\n---\nRead notes.txt\n");
    const fs::path resource = session_facts.skill_roots.front()
        / "new-skill" / "notes.txt";
    write_file(resource, "fresh resource\n");

    EXPECT_NE(std::string::npos,
              services.skill_catalog().find("new-skill"));
    const auto skill = runtime.resolve("skill");
    ASSERT_TRUE(skill.has_value());
    const ExecResult activated = runtime.execute(
        skill->ref, nlohmann::json{{"name", "new-skill"}});
    ASSERT_TRUE(activated.has_value()) << activated.error().render();
    EXPECT_NE(std::string::npos, activated->text.find("Read notes.txt"));

    const auto read = runtime.resolve("read");
    ASSERT_TRUE(read.has_value());
    const ExecResult fetched = runtime.execute(
        read->ref, nlohmann::json{{"path", resource.string()}});
    ASSERT_TRUE(fetched.has_value()) << fetched.error().render();
    EXPECT_EQ("fresh resource\n", fetched->text);

    fs::remove(session_facts.skill_roots.front() / "new-skill" / "SKILL.md");
    EXPECT_TRUE(services.skill_catalog().empty());
    const ExecResult still_allowed = runtime.execute(
        read->ref, nlohmann::json{{"path", resource.string()}});
    ASSERT_TRUE(still_allowed.has_value()) << still_allowed.error().render();
    EXPECT_EQ("fresh resource\n", still_allowed->text);
}

// ── AC 5：环境变了，会话的 workspace 不变 ─────────────────────────────────

TEST_F(SessionServicesTest, ChangingCwdAfterConstructionDoesNotMoveTheWorkspace)
{
    const SessionFacts a_facts = facts("a");
    const SessionServices a{a_facts};

    const fs::path elsewhere = base_ / "elsewhere";
    fs::create_directories(elsewhere);
    fs::current_path(elsewhere);

    // 直接钉访问器。
    EXPECT_EQ(a_facts.workspace_root, a.workspace_root());

    // 端到端：相对路径仍解析到构造时的 workspace，不是新的 cwd。
    write_file(a_facts.workspace_root / "note.md", "in the session workspace\n");
    const ExecResult hit = run(a, "read", nlohmann::json{{"path", "note.md"}});
    ASSERT_TRUE(hit.has_value()) << hit.error().render();
    EXPECT_NE(std::string::npos, hit->text.find("in the session workspace"));
}

// ── AC 7：环境变了，会话的文档配置不变（跑到真检索）──────────────────────

TEST_F(SessionServicesTest, DocsConfigStaysWithTheSessionWhenTheEnvironmentChanges)
{
    const SessionFacts a_facts = facts("a");
    write_file(
        a_facts.docs.root / "note.md",
        "# Flux capacitor\n\nThe flux capacitor needs 1.21 gigawatts.\n"
    );

    const SessionServices a{a_facts};

    // 构造之后把环境整个换掉：cwd 到别处，且显式把配置指向一份诱饵语料。
    const fs::path elsewhere = base_ / "elsewhere";
    const fs::path decoy_docs = elsewhere / "docs";
    fs::create_directories(decoy_docs);
    write_file(decoy_docs / "decoy.md", "# Decoy\n\nnothing about gigawatts\n");
    ::setenv("MY_AGENT_DOCS_DIR", decoy_docs.string().c_str(), 1);
    fs::current_path(elsewhere);

    // 检索走注入的语料。embed.model 为空 = BM25-only，零网络。
    const ExecResult hit =
        run(a, "search_docs", nlohmann::json{{"query", "gigawatts"}});
    ASSERT_TRUE(hit.has_value()) << hit.error().render();

    EXPECT_NE(std::string::npos, hit->text.find("flux capacitor")) << hit->text;
    EXPECT_EQ(std::string::npos, hit->text.find("decoy"))
        << "检索落到了环境里那份诱饵语料上：\n"
        << hit->text;
}

TEST_F(SessionServicesTest, ConfiguredTeiRerankerReordersNativeSearchDocs)
{
    SessionFacts session_facts = facts("tei-rerank");
    write_file(session_facts.docs.root / "a.md", "gamma alpha\n");
    write_file(session_facts.docs.root / "b.md", "gamma beta\n");
    session_facts.docs.rerank = rag::TeiRerankConfig{
        .host = "tei.test",
        .port = 9090,
        .timeout_ms = 1200,
    };

    bool embed_called = false;
    const rag::EmbedBackend embed_backend =
        [&embed_called](const rag::EmbedConfig&,
                        const std::vector<std::string>&,
                        rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
        embed_called = true;
        return std::unexpected(std::string{"embed should not be called"});
    };

    int transport_calls = 0;
    const rag::TeiRerankTransport transport =
        [&transport_calls](const rag::TeiRerankConfig& config,
                           std::string_view body)
            -> std::expected<std::string, std::string> {
        ++transport_calls;
        EXPECT_EQ(config.host, "tei.test");
        EXPECT_EQ(config.port, 9090);
        EXPECT_EQ(config.timeout_ms, 1200);

        const auto request = nlohmann::json::parse(body);
        const auto& texts = request.at("texts");
        nlohmann::json response = nlohmann::json::array();
        for (std::size_t i = 0; i < texts.size(); ++i) {
            const std::string text = texts[i].get<std::string>();
            response.push_back({
                {"index", i},
                {"score", text.find("beta") != std::string::npos ? 10.0 : 0.0},
            });
        }
        return response.dump();
    };

    const SessionServices services{
        std::move(session_facts), embed_backend, transport};
    const ExecResult result = run(
        services, "search_docs",
        nlohmann::json{{"query", "gamma"}, {"k", 2}});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ(transport_calls, 1);
    EXPECT_FALSE(embed_called);
    const std::size_t beta = result->text.find("b.md:");
    const std::size_t alpha = result->text.find("a.md:");
    ASSERT_NE(beta, std::string::npos);
    ASSERT_NE(alpha, std::string::npos);
    EXPECT_LT(beta, alpha);
}

TEST_F(SessionServicesTest, ConfiguredTeiFailureKeepsLocalResults)
{
    SessionFacts session_facts = facts("tei-fallback");
    write_file(session_facts.docs.root / "guide.md", "gamma deployment guide\n");
    session_facts.docs.rerank = rag::TeiRerankConfig{};

    const rag::TeiRerankTransport transport =
        [](const rag::TeiRerankConfig&, std::string_view)
            -> std::expected<std::string, std::string> {
        return std::unexpected(std::string{"tei unavailable"});
    };
    const SessionServices services{
        std::move(session_facts), rag::ollama_embed, transport};

    const ExecResult result = run(
        services, "search_docs", nlohmann::json{{"query", "gamma"}});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_NE(result->text.find("guide.md:"), std::string::npos);
    EXPECT_NE(result->text.find("rerank: model-fallback — tei unavailable"),
              std::string::npos);
}

TEST_F(SessionServicesTest, IndependentDocsSessionsBuildTheirOwnIndexes)
{
    SessionFacts a_facts = facts("docs-a");
    SessionFacts b_facts = facts("docs-b");
    const fs::path shared_docs = base_ / "shared-docs";
    write_file(shared_docs / "note.md", "# Fluxonium\n\nFluxonium coherence matters.\n");
    a_facts.docs.root = shared_docs;
    b_facts.docs.root = shared_docs;
    a_facts.docs.embed.model = "fake-embed";
    b_facts.docs.embed.model = "fake-embed";

    std::size_t a_doc_calls = 0;
    std::size_t b_doc_calls = 0;
    auto backend = [](std::size_t& doc_calls) -> my_agent::rag::EmbedBackend {
        return [&doc_calls](const my_agent::rag::EmbedConfig&,
                            const std::vector<std::string>& texts,
                            my_agent::rag::EmbedRole role)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            if (role == my_agent::rag::EmbedRole::Doc) {
                ++doc_calls;
            }
            return std::vector<std::vector<float>>(texts.size(), {1.0f, 0.0f});
        };
    };
    const SessionServices a{a_facts, backend(a_doc_calls)};
    const my_agent::tool::ToolRuntime a_runtime{a.registrations()};
    EXPECT_FALSE(fs::exists(shared_docs / ".my_agent_rag_cache.bin"));

    const auto a_search = a_runtime.resolve("search_docs");
    ASSERT_TRUE(a_search.has_value());
    const ExecResult a_hit = a_runtime.execute(
        a_search->ref, nlohmann::json{{"query", "fluxonium"}});
    ASSERT_TRUE(a_hit.has_value()) << a_hit.error().render();
    EXPECT_NE(std::string::npos, a_hit->text.find("mode: hybrid"));
    EXPECT_GT(a_doc_calls, 0u);
    ASSERT_TRUE(fs::exists(shared_docs / ".my_agent_rag_cache.bin"));

    const std::size_t built_calls = a_doc_calls;
    fs::remove(shared_docs / ".my_agent_rag_cache.bin");
    const my_agent::tool::ToolRuntime a_again{a.registrations()};
    const auto a_again_search = a_again.resolve("search_docs");
    ASSERT_TRUE(a_again_search.has_value());
    ASSERT_TRUE(a_again.execute(
        a_again_search->ref, nlohmann::json{{"query", "fluxonium"}})
                    .has_value());
    EXPECT_EQ(built_calls, a_doc_calls);
    EXPECT_FALSE(fs::exists(shared_docs / ".my_agent_rag_cache.bin"));

    // Remove the disk cache: a second session must still build its own index.
    const SessionServices b{b_facts, backend(b_doc_calls)};
    const my_agent::tool::ToolRuntime b_runtime{b.registrations()};
    const auto b_search = b_runtime.resolve("search_docs");
    ASSERT_TRUE(b_search.has_value());
    const ExecResult b_hit = b_runtime.execute(
        b_search->ref, nlohmann::json{{"query", "fluxonium"}});
    ASSERT_TRUE(b_hit.has_value()) << b_hit.error().render();
    EXPECT_NE(std::string::npos, b_hit->text.find("mode: hybrid"));
    EXPECT_GT(b_doc_calls, 0u);
}

TEST_F(SessionServicesTest, DocsRuntimesKeepRootsSeparateAndRefreshChangedDocuments)
{
    SessionFacts a_facts = facts("docs-root-a");
    SessionFacts b_facts = facts("docs-root-b");
    write_file(a_facts.docs.root / "flux.md", "# Fluxonium\n\nfluxonium circuits\n");
    write_file(b_facts.docs.root / "tides.md", "# Harbor\n\nharbor tides\n");
    const SessionServices a{a_facts};
    const SessionServices b{b_facts};
    const my_agent::tool::ToolRuntime a_runtime{a.registrations()};
    const my_agent::tool::ToolRuntime b_runtime{b.registrations()};
    const auto a_search = a_runtime.resolve("search_docs");
    const auto b_search = b_runtime.resolve("search_docs");
    ASSERT_TRUE(a_search.has_value());
    ASSERT_TRUE(b_search.has_value());

    const ExecResult a_hit = a_runtime.execute(
        a_search->ref, nlohmann::json{{"query", "fluxonium"}});
    const ExecResult b_hit = b_runtime.execute(
        b_search->ref, nlohmann::json{{"query", "harbor"}});
    ASSERT_TRUE(a_hit.has_value()) << a_hit.error().render();
    ASSERT_TRUE(b_hit.has_value()) << b_hit.error().render();
    EXPECT_NE(std::string::npos, a_hit->text.find("flux.md:"));
    EXPECT_EQ(std::string::npos, a_hit->text.find("tides.md:"));
    EXPECT_NE(std::string::npos, b_hit->text.find("tides.md:"));
    EXPECT_EQ(std::string::npos, b_hit->text.find("flux.md:"));
    EXPECT_NE(std::string::npos, a_hit->text.find("mode: BM25-only"));
    EXPECT_NE(std::string::npos, b_hit->text.find("mode: BM25-only"));

    write_file(a_facts.docs.root / "flux.md",
               "# Fluxonium\n\nfluxonium circuits use zephyrium\n");
    const ExecResult refreshed = a_runtime.execute(
        a_search->ref, nlohmann::json{{"query", "zephyrium"}});
    ASSERT_TRUE(refreshed.has_value()) << refreshed.error().render();
    EXPECT_NE(std::string::npos, refreshed->text.find("zephyrium"));
    const ExecResult b_miss = b_runtime.execute(
        b_search->ref, nlohmann::json{{"query", "zephyrium"}});
    ASSERT_TRUE(b_miss.has_value()) << b_miss.error().render();
    EXPECT_NE(std::string::npos, b_miss->text.find("no matching passages"));
}

}  // namespace
