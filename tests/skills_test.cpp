#include "my_agent/tool/skills.hpp"
#include "../src/tool/read.hpp"
#include "../src/tool/skill_tools.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace skills = my_agent::tool::skills;

// 每个测试一个独立目录，放在 /tmp 默认读权限之外；两个 root 模拟 project / user。
class SkillEngineTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = fs::current_path()
            / ("my_agent_skills_test_" + std::to_string(::getpid()) + "_"
               + ::testing::UnitTest::GetInstance()
                     ->current_test_info()
                     ->name());
        fs::remove_all(root_);
        fs::create_directories(project_root());
        fs::create_directories(user_root());
    }

    void TearDown() override { fs::remove_all(root_); }

    [[nodiscard]] fs::path project_root() const { return root_ / "project"; }
    [[nodiscard]] fs::path user_root() const { return root_ / "user"; }

    // roots 按优先级从高到低：project 在前，同名 skill 覆盖 user 的那份。
    [[nodiscard]] std::vector<fs::path> roots() const
    {
        return {project_root(), user_root()};
    }

    static void write_skill(
        const fs::path& root,
        std::string_view slug,
        std::string_view contents
    )
    {
        const fs::path dir = root / slug;
        fs::create_directories(dir);
        std::ofstream out{dir / "SKILL.md", std::ios::binary};
        out << contents;
    }

    static void write_file(
        const fs::path& root,
        std::string_view relative,
        std::string_view contents
    )
    {
        const fs::path path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary};
        out << contents;
    }

    fs::path root_;
};

// 场景：一份最简的 SKILL.md —— frontmatter 给了 name 和 description，后面是正文。
// 领域语义：frontmatter 供 Tier1 目录（便宜，每轮都在提示里），正文供 Tier2
// （模型 activate 才加载）。这条边界必须干净：frontmatter 泄进正文会让 Tier2
// 重复 Tier1 已有的内容，白花 token。
// Red 原因：仓库尚无 skills.cpp，parse_skill 无定义。
TEST(SkillParseTest, SplitsFrontmatterFieldsFromTheBody)
{
    const std::string contents =
        "---\n"
        "name: pdf-extract\n"
        "description: Extract text from PDF files\n"
        "---\n"
        "Use pdftotext for the first pass.\n";

    const skills::Skill skill =
        skills::parse_skill(contents, "whatever-dir", "/tmp/skills/pdf");

    EXPECT_EQ("pdf-extract", skill.name);
    EXPECT_EQ("Extract text from PDF files", skill.description);
    EXPECT_EQ("Use pdftotext for the first pass.\n", skill.body);
    EXPECT_EQ(std::filesystem::path{"/tmp/skills/pdf"}, skill.dir);
}

// 场景：description 里带一个未加引号的冒号。
// 领域语义：`description: Use when: handling PDFs` 是真实 skill 里的常见写法，
// 别的客户端也接受。所以按**第一个**冒号切，而不是拒绝整行 —— 严格 YAML 会在
// 这里失败，而我们要能读别人装好的 skill。
TEST(SkillParseTest, SplitsOnTheFirstColonSoDescriptionsCanContainColons)
{
    const std::string contents =
        "---\n"
        "name: pdf\n"
        "description: Use when: the user mentions a PDF\n"
        "---\n"
        "body\n";

    const skills::Skill skill = skills::parse_skill(contents, "pdf", "/tmp/pdf");

    EXPECT_EQ("Use when: the user mentions a PDF", skill.description);
}

// 场景：frontmatter 没给 name。
// 领域语义：目录名就是 skill 的天然标识（activate 用它寻址）。缺 name 不该让整份
// skill 消失 —— 装 skill 的人已经用目录名表达了意图。
TEST(SkillParseTest, FallsBackToTheDirectorySlugWhenNameIsMissing)
{
    const std::string contents =
        "---\n"
        "description: Something useful\n"
        "---\n"
        "body\n";

    const skills::Skill skill =
        skills::parse_skill(contents, "my-slug", "/tmp/my-slug");

    EXPECT_EQ("my-slug", skill.name);
}

// 场景：frontmatter 没给 description。
// 领域语义：Tier1 目录的每一行都要有描述，否则模型看到一个光名字无从判断何时用它。
// 正文首个非空行通常就是一句概述，拿它兜底比留空好。
TEST(SkillParseTest, FallsBackToTheFirstNonBlankBodyLineWhenDescriptionIsMissing)
{
    const std::string contents =
        "---\n"
        "name: pdf\n"
        "---\n"
        "\n"
        "   \n"
        "Extracts text from PDFs.\n"
        "More detail here.\n";

    const skills::Skill skill = skills::parse_skill(contents, "pdf", "/tmp/pdf");

    EXPECT_EQ("Extracts text from PDFs.", skill.description);
}

// 场景：frontmatter 里有我们不认识的键。
// 领域语义：SKILL.md 是别的客户端也在写的格式，会出现 allowed-tools、license 等
// 我们不处理的键。宽容跳过，不要因为一个陌生键就丢掉整份 skill。
TEST(SkillParseTest, IgnoresUnknownFrontmatterKeysInsteadOfFailing)
{
    const std::string contents =
        "---\n"
        "name: pdf\n"
        "allowed-tools: read bash\n"
        "license: MIT\n"
        "description: Extract text\n"
        "---\n"
        "body\n";

    const skills::Skill skill = skills::parse_skill(contents, "pdf", "/tmp/pdf");

    EXPECT_EQ("pdf", skill.name);
    EXPECT_EQ("Extract text", skill.description);
    EXPECT_EQ("body\n", skill.body);
}

// 场景：整份文件没有 frontmatter，从第一行就是正文。
// 领域语义：手写的 skill 常常省掉 frontmatter。这时整份内容都是正文，name 取目录名，
// description 取首行 —— 仍然是一份可用的 skill，不该被当成损坏。
TEST(SkillParseTest, TreatsAFileWithNoFrontmatterAsAllBody)
{
    const std::string contents = "Just instructions, no frontmatter.\n";

    const skills::Skill skill =
        skills::parse_skill(contents, "plain", "/tmp/plain");

    EXPECT_EQ("plain", skill.name);
    EXPECT_EQ("Just instructions, no frontmatter.", skill.description);
    EXPECT_EQ(contents, skill.body);
}

// 场景：两个根各有一份 skill，名字不同。
// 领域语义：发现是 Tier1 的输入。跨根收集，按名字排序 —— 顺序稳定，系统提示才
// 不会因为文件系统的遍历顺序而每轮抖动（提示内容变化会打掉 provider 的缓存）。
// Red 原因：SkillEngine::all 无定义。
TEST_F(SkillEngineTest, FindsSkillsAcrossEveryRoot)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");
    write_skill(user_root(), "excel", "---\nname: excel\ndescription: Sheets\n---\nbody\n");

    const std::vector<skills::Skill> found =
        skills::SkillEngine{roots()}.all();

    ASSERT_EQ(std::size_t{2}, found.size());
    EXPECT_EQ("excel", found.at(0).name);
    EXPECT_EQ("pdf", found.at(1).name);
}

// 场景：两个根有同名 skill。
// 领域语义：project 覆盖 user 是整个优先级链条存在的理由 —— 用户在全局装了一份
// 通用 skill，某个项目需要一份特化版本，项目里那份必须胜出。合并两份会产生
// 自相矛盾的指令，所以是「首个命中胜出」而不是合并。
TEST_F(SkillEngineTest, LetsTheProjectRootOverrideASameNamedUserSkill)
{
    write_skill(
        project_root(),
        "pdf",
        "---\nname: pdf\ndescription: Project version\n---\nproject body\n"
    );
    write_skill(
        user_root(),
        "pdf",
        "---\nname: pdf\ndescription: User version\n---\nuser body\n"
    );

    const std::vector<skills::Skill> found =
        skills::SkillEngine{roots()}.all();

    ASSERT_EQ(std::size_t{1}, found.size());
    EXPECT_EQ("Project version", found.front().description);
    EXPECT_EQ(project_root() / "pdf", found.front().dir);
}

// 场景：一个根不存在，另一个根里有个没有 SKILL.md 的子目录。
// 领域语义：四个发现根里通常只有一两个真的存在（大多数人没有 ~/.claude/skills）。
// 不存在的根、以及碰巧在 skills/ 下的无关目录，都只能被跳过 —— 不能让缺一个
// 可选目录就打断整轮发现。
TEST_F(SkillEngineTest, SkipsMissingRootsAndDirectoriesWithoutASkillFile)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");
    fs::create_directories(project_root() / "not-a-skill");

    const skills::SkillEngine engine{
        {project_root(), root_ / "does-not-exist", user_root()}
    };

    const std::vector<skills::Skill> found = engine.all();

    ASSERT_EQ(std::size_t{1}, found.size());
    EXPECT_EQ("pdf", found.front().name);
}

// 场景：两份 skill，渲染 Tier1 目录块。
// 领域语义：这是渐进披露最便宜的一层，每轮都在系统提示里。只放名字和一句描述，
// 正文等 activate —— 全部正文都塞进提示，token 会随 skill 数量线性增长，而任一
// 回合真正用得上的通常只有一份。块里还必须明确告诉模型「用 skill 工具加载」，
// 否则它会照着一句描述猜正文内容。
// Red 原因：SkillEngine::catalog_block 无定义。
TEST_F(SkillEngineTest, RendersACatalogOfNamesAndDescriptionsForTheSystemPrompt)
{
    write_skill(
        project_root(),
        "pdf",
        "---\nname: pdf\ndescription: Extract text from PDFs\n---\nlong body\n"
    );
    write_skill(
        user_root(),
        "excel",
        "---\nname: excel\ndescription: Read spreadsheets\n---\nlong body\n"
    );

    const std::string block = skills::SkillEngine{roots()}.catalog_block();

    EXPECT_NE(std::string::npos, block.find("<skills>"));
    EXPECT_NE(std::string::npos, block.find("</skills>"));
    EXPECT_NE(std::string::npos, block.find("- excel — Read spreadsheets"));
    EXPECT_NE(std::string::npos, block.find("- pdf — Extract text from PDFs"));

    // Tier1 只给描述，正文不能泄进来 —— 否则渐进披露就没有意义了。
    EXPECT_EQ(std::string::npos, block.find("long body"));

    // 必须指明加载途径，否则模型会照着描述猜正文。
    EXPECT_NE(std::string::npos, block.find("skill"));
}

// 场景：一个 skill 都没有。
// 领域语义：与 <memory> 同一个约定 —— 空块不输出。给模型一个空的
// <skills></skills> 只是噪声，还可能被读成「确实没有可用 skill」的强信号。
TEST_F(SkillEngineTest, OmitsTheCatalogBlockEntirelyWhenThereAreNoSkills)
{
    EXPECT_TRUE(skills::SkillEngine{roots()}.catalog_block().empty());
}

// 场景：activate 一份带资源文件的 skill。
// 领域语义：Tier2 与 Tier3 的分界就在这个返回值里。正文整份给出（模型要照着做），
// 资源**只列不读** —— 正文往往引用了它们，但引用不等于每次都要，而且一份 skill
// 可能带上几十 KB 的参考文档。同时必须给出 skill 目录的绝对路径：正文里的相对
// 路径是相对 skill 目录的，而工具的 cwd 是工作区，不给绝对路径模型就拼不出来。
// Red 原因：SkillEngine::activate 无定义。
TEST_F(SkillEngineTest, ActivationReturnsTheBodyAndListsResourcesWithoutReadingThem)
{
    write_skill(
        project_root(),
        "pdf",
        "---\nname: pdf\ndescription: Extract text\n---\n"
        "Run the bundled script, then check the reference.\n"
    );
    write_file(
        project_root(),
        "pdf/scripts/extract.sh",
        "#!/bin/sh\necho SENTINEL_SCRIPT_CONTENT\n"
    );
    write_file(project_root(), "pdf/references/notes.md", "SENTINEL_REFERENCE_BODY\n");

    const std::optional<std::string> payload =
        skills::SkillEngine{roots()}.activate("pdf");

    ASSERT_TRUE(payload.has_value());

    // 正文整份给出。
    EXPECT_NE(
        std::string::npos,
        payload->find("Run the bundled script, then check the reference.")
    );

    // 资源被列出 —— 模型得知道有什么可读。
    EXPECT_NE(std::string::npos, payload->find("scripts/extract.sh"));
    EXPECT_NE(std::string::npos, payload->find("references/notes.md"));

    // 但内容绝不能被读进来：这正是 Tier3 存在的意义。
    EXPECT_EQ(std::string::npos, payload->find("SENTINEL_SCRIPT_CONTENT"));
    EXPECT_EQ(std::string::npos, payload->find("SENTINEL_REFERENCE_BODY"));

    // 绝对路径必须在：正文里的相对路径要靠它才能解析成 read 能用的路径。
    EXPECT_NE(std::string::npos, payload->find((project_root() / "pdf").string()));
}

// 场景：activate 一个不存在的名字。
// 领域语义：模型会写错名字。返回 nullopt 让工具层渲染成可自我纠正的错误，
// 而不是返回一份空正文让模型以为 skill 是空的。
TEST_F(SkillEngineTest, ActivationReportsNothingForAnUnknownSkillName)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");

    EXPECT_FALSE(skills::SkillEngine{roots()}.activate("nope").has_value());
}

// 场景：取 skill 目录清单。
// 领域语义：Tier3 的前提。skill 常装在 $HOME 下，而 read 工具有 workspace 硬边界
// —— 不把这些目录交给 read 的允许列表，正文引用的脚本一读就被拒，Tier3 直接断掉。
TEST_F(SkillEngineTest, ExposesSkillDirectoriesSoTheReadToolCanAllowThem)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");
    write_skill(user_root(), "excel", "---\nname: excel\ndescription: Sheets\n---\nbody\n");

    const std::vector<fs::path> dirs = skills::SkillEngine{roots()}.directories();

    ASSERT_EQ(std::size_t{2}, dirs.size());
    EXPECT_NE(
        std::ranges::find(dirs, project_root() / "pdf"),
        dirs.end()
    );
    EXPECT_NE(
        std::ranges::find(dirs, user_root() / "excel"),
        dirs.end()
    );
}

// 场景：read 工具被告知一个 skill 目录，去读那个目录里的资源文件。
// 领域语义：Tier3 落地的那一步。skill 常装在 $HOME 下，完全在工作区之外，而 read
// 有 workspace 硬边界。允许列表把边界从「workspace」放宽到「workspace ∪ skill
// 目录」—— **只放宽读**。这样正文引用的脚本读得到，同时工作区之外的任意文件
// 仍然读不到。
// 直接工厂的显式允许根与会话中的动态允许根都只影响 read。
TEST_F(SkillEngineTest, ReadToolAllowsSkillDirectoriesWhileStillRefusingElsewhere)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");
    write_file(project_root(), "pdf/scripts/extract.sh", "SCRIPT BODY\n");
    write_file(root_, "outside/secret.txt", "SECRET\n");

    // workspace 故意设成一个与 skill 根无关的空目录：唯一让读通过的途径就是
    // 允许列表，测试因此不会被「碰巧在工作区内」蒙混过去。
    const fs::path workspace = root_ / "workspace";
    fs::create_directories(workspace);

    const my_agent::tool::RegisteredTool read = my_agent::tool::detail::make_read_tool(
        workspace,
        skills::SkillEngine{roots()}.directories(),
        /*allow_temporary_root=*/false
    );

    const my_agent::tool::ExecResult allowed = read.handler(nlohmann::json{
        {"path", (project_root() / "pdf" / "scripts" / "extract.sh").string()},
    }, my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(allowed.has_value()) << allowed.error().render();
    EXPECT_EQ("SCRIPT BODY\n", allowed->text);

    // 边界仍然在：不在允许列表里、也不在工作区里的文件照旧被拒。
    const my_agent::tool::ExecResult refused = read.handler(nlohmann::json{
        {"path", (root_ / "outside" / "secret.txt").string()},
    }, my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(
        my_agent::tool::ErrorKind::OutOfWorkspace,
        refused.error().kind
    );
}

// 场景：模型调 skill 工具加载一份 skill。
// 领域语义：这是 SkillEngine 接进 Agent 的那一层 —— 引擎本身不知道工具的存在，
// 工具层负责参数校验和把结果渲染成模型能直接照做的文本。
TEST_F(SkillEngineTest, SkillToolReturnsTheActivatedBody)
{
    write_skill(
        project_root(),
        "pdf",
        "---\nname: pdf\ndescription: Extract text\n---\nRun pdftotext first.\n"
    );

    auto engine = std::make_shared<skills::SkillEngine>(roots());
    const my_agent::tool::RegisteredTool skill =
        my_agent::tool::detail::make_skill_tool(engine);

    const my_agent::tool::ExecResult result =
        skill.handler(nlohmann::json{{"name", "pdf"}},
                      my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_NE(std::string::npos, result->text.find("Run pdftotext first."));
    EXPECT_NE(std::string::npos, result->text.find("<skill_content"));
}

// 场景：模型给了一个不存在的 skill 名字。
// 领域语义：模型会写错名字或凭印象编一个。错误信息里必须带上真实可用的名字，
// 它才能一次纠正过来，而不是反复猜。
TEST_F(SkillEngineTest, SkillToolListsAvailableNamesWhenTheRequestedOneIsUnknown)
{
    write_skill(project_root(), "pdf", "---\nname: pdf\ndescription: PDFs\n---\nbody\n");
    write_skill(user_root(), "excel", "---\nname: excel\ndescription: Sheets\n---\nbody\n");

    auto engine = std::make_shared<skills::SkillEngine>(roots());
    const my_agent::tool::ExecResult result =
        my_agent::tool::detail::make_skill_tool(engine).handler(
            nlohmann::json{{"name", "nope"}}, my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::NotFound, result.error().kind);
    // 带上真实名字，模型才能一次纠正。
    EXPECT_NE(std::string::npos, result.error().message.find("pdf"));
    EXPECT_NE(std::string::npos, result.error().message.find("excel"));
}

// 场景：调 skill 不带 name。
// 领域语义：无参 activate 无从判断该加载哪份。在工具层挡住，给出可自我纠正的错误。
TEST_F(SkillEngineTest, SkillToolRequiresAName)
{
    auto engine = std::make_shared<skills::SkillEngine>(roots());
    const my_agent::tool::ExecResult result =
        my_agent::tool::detail::make_skill_tool(engine).handler(
            nlohmann::json::object(), my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, result.error().kind);
}

// 场景：检查 skill 工具声明的 effects。
// 领域语义：effects 必须如实描述工具做了什么。activate 只读 SKILL.md，所以带
// ReadFs 而不是 WriteFs —— 与 remember/forget 带 WriteFs 是同一条纪律：
// 谎报 effects 会让整个 policy 层失去可信度。
TEST_F(SkillEngineTest, SkillToolDeclaresReadFsBecauseActivationOnlyReadsFiles)
{
    auto engine = std::make_shared<skills::SkillEngine>(roots());
    const my_agent::tool::RegisteredTool skill =
        my_agent::tool::detail::make_skill_tool(engine);

    EXPECT_TRUE(skill.descriptor.effects.has(my_agent::tool::Effect::ReadFs));
    EXPECT_FALSE(skill.descriptor.effects.has(my_agent::tool::Effect::WriteFs));
}

}  // namespace
