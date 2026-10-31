#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/effects.hpp"
#include "../src/tool/memory_tools.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace memory = my_agent::tool::memory;

// 每个测试一个独立临时目录，互不干扰。
class MemoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path()
            / ("my_agent_memory_test_" + std::to_string(::getpid()) + "_"
               + ::testing::UnitTest::GetInstance()
                     ->current_test_info()
                     ->name());
        fs::remove_all(root_);
        fs::create_directories(root_ / "home");
        fs::create_directories(root_ / "project");
    }

    void TearDown() override { fs::remove_all(root_); }

    [[nodiscard]] memory::Roots roots() const
    {
        return memory::Roots{
            .user = root_ / "home",
            .project = root_ / "project",
        };
    }

    fs::path root_;
};

}  // namespace

// 场景：往一个 scope 写一条，再读回来。
// 领域语义：append 是 remember 工具的唯一落盘路径，load_all 是系统提示每轮的
// 唯一读取路径。两者必须闭合 —— 写进去的事实下一轮就能被模型看到。
// Red 原因：仓库尚无 memory_store.cpp，MemoryStore 的成员函数均无定义。
TEST_F(MemoryStoreTest, LoadsBackARecordThatWasAppended)
{
    memory::MemoryStore store{roots()};

    const memory::AppendResult result =
        store.append(memory::Scope::Project, "prefers fish shell");

    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(std::size_t{8}, result.id.size());

    const std::vector<memory::Record> records =
        store.load_all(memory::Scope::Project);

    ASSERT_EQ(std::size_t{1}, records.size());
    EXPECT_EQ(result.id, records.front().id);
    EXPECT_EQ("prefers fish shell", records.front().text);
    EXPECT_EQ(memory::Scope::Project, records.front().scope);
    EXPECT_GT(records.front().ts, std::int64_t{0});
}

// 场景：两个 scope 各写一条。
// 领域语义：user scope 跨工作区共享，project scope 按项目隔离。两者落在不同文件，
// 一个 scope 的读取绝不能看到另一个的记录 —— 否则「按项目隔离」这个承诺就是假的。
TEST_F(MemoryStoreTest, KeepsUserAndProjectScopesInSeparateFiles)
{
    memory::MemoryStore store{roots()};

    ASSERT_TRUE(store.append(memory::Scope::User, "likes concise answers").error.empty());
    ASSERT_TRUE(store.append(memory::Scope::Project, "builds with cmake").error.empty());

    const std::vector<memory::Record> user =
        store.load_all(memory::Scope::User);
    const std::vector<memory::Record> project =
        store.load_all(memory::Scope::Project);

    ASSERT_EQ(std::size_t{1}, user.size());
    ASSERT_EQ(std::size_t{1}, project.size());
    EXPECT_EQ("likes concise answers", user.front().text);
    EXPECT_EQ("builds with cmake", project.front().text);

    EXPECT_NE(
        store.path_for(memory::Scope::User),
        store.path_for(memory::Scope::Project)
    );
}

// 场景：写三条，按 id 精确移除中间那条。
// 领域语义：forget 是用户纠正过时事实的唯一出口，必须精确 —— 只动命中的那条，
// 其余原样存活。JSONL 的按行可寻址就是为了让这件事简单。
TEST_F(MemoryStoreTest, RemovesExactlyTheRecordMatchingAnId)
{
    memory::MemoryStore store{roots()};

    const std::string first =
        store.append(memory::Scope::Project, "fact one").id;
    const std::string second =
        store.append(memory::Scope::Project, "fact two").id;
    const std::string third =
        store.append(memory::Scope::Project, "fact three").id;
    ASSERT_FALSE(second.empty());

    EXPECT_EQ(std::size_t{1}, store.forget_by_id(second));

    const std::vector<memory::Record> survivors =
        store.load_all(memory::Scope::Project);
    ASSERT_EQ(std::size_t{2}, survivors.size());
    EXPECT_EQ(first, survivors.at(0).id);
    EXPECT_EQ(third, survivors.at(1).id);
}

// 场景：用一个不存在的 id 调 forget。
// 领域语义：找不到不是错误，是「零条被移除」。模型引用一个过期 id 不该炸掉。
TEST_F(MemoryStoreTest, ReportsZeroRemovalsForAnUnknownId)
{
    memory::MemoryStore store{roots()};
    ASSERT_TRUE(store.append(memory::Scope::Project, "fact one").error.empty());

    EXPECT_EQ(std::size_t{0}, store.forget_by_id("deadbeef"));
    EXPECT_EQ(std::size_t{1}, store.load_all(memory::Scope::Project).size());
}

// 场景：按子串移除，命中跨越两个 scope。
// 领域语义：forget 语义上是「忘掉这件事」，用户不该被要求先说清它存在哪个 scope。
// 所以子串匹配两个 scope 都扫。
TEST_F(MemoryStoreTest, RemovesMatchingRecordsAcrossBothScopes)
{
    memory::MemoryStore store{roots()};

    ASSERT_TRUE(store.append(memory::Scope::User, "deploy via kubectl").error.empty());
    ASSERT_TRUE(store.append(memory::Scope::Project, "deploy via kubectl too").error.empty());
    ASSERT_TRUE(store.append(memory::Scope::Project, "unrelated fact").error.empty());

    EXPECT_EQ(std::size_t{2}, store.forget_by_substring("kubectl"));

    EXPECT_TRUE(store.load_all(memory::Scope::User).empty());
    const std::vector<memory::Record> project =
        store.load_all(memory::Scope::Project);
    ASSERT_EQ(std::size_t{1}, project.size());
    EXPECT_EQ("unrelated fact", project.front().text);
}

// 场景：用空串（或纯空白）调子串 forget。
// 领域语义：空 pattern 会匹配每一条记录。一次手滑的 forget{""} 不能清空整个存储，
// 所以必须显式拒绝而不是「匹配全部」。
TEST_F(MemoryStoreTest, RefusesAnEmptySubstringPatternInsteadOfMatchingEverything)
{
    memory::MemoryStore store{roots()};
    ASSERT_TRUE(store.append(memory::Scope::Project, "fact one").error.empty());
    ASSERT_TRUE(store.append(memory::Scope::Project, "fact two").error.empty());

    EXPECT_EQ(std::size_t{0}, store.forget_by_substring(""));
    EXPECT_EQ(std::size_t{0}, store.forget_by_substring("   "));

    EXPECT_EQ(std::size_t{2}, store.load_all(memory::Scope::Project).size());
}

// 场景：文件里混着坏行 —— 截断的 JSON、非对象、scope 非法、id 或 text 缺失。
// 领域语义：存储是纯文本，人会手工编辑它（这正是选 JSONL 的理由之一）。一条坏行
// 只能影响它自己，加载必须跳过并继续读后面的行，否则一次手滑就让全部记忆不可用。
TEST_F(MemoryStoreTest, SkipsMalformedLinesAndKeepsReadingTheRest)
{
    memory::MemoryStore store{roots()};

    const fs::path path = store.path_for(memory::Scope::Project);
    fs::create_directories(path.parent_path());

    std::ofstream out{path, std::ios::binary};
    out << R"({"id":"aaaaaaaa","ts":1,"scope":"project","text":"good one"})" << '\n';
    out << R"({"id":"bbbbbbbb","ts":2,"scope":"project","text":"trunca)" << '\n';
    out << "not json at all" << '\n';
    out << R"(["an","array","not","an","object"])" << '\n';
    out << R"({"id":"cccccccc","ts":3,"scope":"nonsense","text":"bad scope"})" << '\n';
    out << R"({"id":"","ts":4,"scope":"project","text":"missing id"})" << '\n';
    out << R"({"id":"dddddddd","ts":5,"scope":"project","text":""})" << '\n';
    out << '\n';
    out << R"({"id":"eeeeeeee","ts":6,"scope":"project","text":"good two"})" << '\n';
    out.close();

    const std::vector<memory::Record> records =
        store.load_all(memory::Scope::Project);

    ASSERT_EQ(std::size_t{2}, records.size());
    EXPECT_EQ("good one", records.at(0).text);
    EXPECT_EQ("good two", records.at(1).text);
}

// 场景：project 根不可用（空路径），模型仍然请求 project scope。
// 领域语义：这是本模块最重要的一条决定 —— **拒绝，而不是静默回退到 user**。
// 一条 project 事实被提升到 user scope，会加载进每一个其他工作区的系统提示，
// 造成跨工作区记忆串味：在项目 B 工作时看到关于项目 A 的事实。报错并告诉调用方
// 怎么显式声明全局事实，比悄悄写错地方好。
TEST_F(MemoryStoreTest, RefusesProjectAppendInsteadOfSilentlyFallingBackToUserScope)
{
    memory::MemoryStore store{memory::Roots{
        .user = root_ / "home",
        .project = {},  // project 不可用
    }};

    const memory::AppendResult result =
        store.append(memory::Scope::Project, "builds with cmake");

    EXPECT_TRUE(result.id.empty());
    ASSERT_FALSE(result.error.empty());
    EXPECT_NE(std::string::npos, result.error.find("project scope"));

    // 关键断言：事实没有被偷偷写进 user scope。
    EXPECT_TRUE(store.load_all(memory::Scope::User).empty());
}

// 场景：text 是空串或纯空白。
// 领域语义：空记忆没有内容可注入系统提示，只会占一行噪声。在入口拒绝。
TEST_F(MemoryStoreTest, RefusesTextThatIsEmptyAfterTrimming)
{
    memory::MemoryStore store{roots()};

    EXPECT_FALSE(store.append(memory::Scope::Project, "").error.empty());
    EXPECT_FALSE(store.append(memory::Scope::Project, "   \t\n ").error.empty());
    EXPECT_TRUE(store.load_all(memory::Scope::Project).empty());
}

// 场景：load_all 在文件还不存在时被调用。
// 领域语义：系统提示每轮都读 memory，全新工作区里文件根本不存在。这必须是
// 「零条记录」而不是错误，否则第一次对话就会带上一条噪声。
TEST_F(MemoryStoreTest, ReturnsNoRecordsWhenTheScopeFileDoesNotExistYet)
{
    memory::MemoryStore store{roots()};
    EXPECT_TRUE(store.load_all(memory::Scope::Project).empty());
    EXPECT_TRUE(store.load_all(memory::Scope::User).empty());
}

namespace {

my_agent::tool::RegisteredTool find_tool(
    const std::vector<my_agent::tool::RegisteredTool>& tools,
    std::string_view name
)
{
    const auto found = std::ranges::find_if(
        tools,
        [name](const my_agent::tool::RegisteredTool& tool) {
            return tool.descriptor.name.value == name;
        }
    );
    if (found == tools.end()) {
        throw std::runtime_error{"tool not found: " + std::string{name}};
    }
    return *found;
}

}  // namespace

// 场景：模型调用 remember 工具存一条事实。
// 领域语义：这是 memory 接入 Agent 的那一层 —— 存储层本身不知道工具的存在，工具
// 层负责参数校验、scope 解析、把结果渲染成模型能读懂的文本。工具执行完，事实必须
// 已经落盘，因为系统提示下一轮就要读它。
// Red 原因：仓库尚无 memory_tools.cpp，make_memory_tools 无定义。
TEST_F(MemoryStoreTest, RememberToolWritesAFactThatIsImmediatelyLoadable)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    const std::vector<my_agent::tool::RegisteredTool> tools =
        my_agent::tool::detail::make_memory_tools(store);

    const my_agent::tool::RegisteredTool remember = find_tool(tools, "remember");

    const my_agent::tool::ExecResult result = remember.handler(nlohmann::json{
        {"text", "prefers concise answers"},
        {"scope", "project"},
    }, my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    // 输出里要带上 id，模型后续才能用它精确 forget。
    EXPECT_NE(std::string::npos, result->text.find("remembered"));

    const std::vector<memory::Record> records =
        store->load_all(memory::Scope::Project);
    ASSERT_EQ(std::size_t{1}, records.size());
    EXPECT_EQ("prefers concise answers", records.front().text);
    EXPECT_NE(std::string::npos, result->text.find(records.front().id));
}

// 场景：remember 不带 scope。
// 领域语义：scope 缺省为 project。默认按项目隔离比默认全局安全 —— 前者写错了只
// 影响当前工作区，后者会污染每一个其他工作区的系统提示。
TEST_F(MemoryStoreTest, RememberToolDefaultsToProjectScope)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    const std::vector<my_agent::tool::RegisteredTool> tools =
        my_agent::tool::detail::make_memory_tools(store);

    const my_agent::tool::ExecResult result =
        find_tool(tools, "remember").handler(nlohmann::json{
            {"text", "builds with cmake"},
        }, my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(std::size_t{1}, store->load_all(memory::Scope::Project).size());
    EXPECT_TRUE(store->load_all(memory::Scope::User).empty());
}

// 场景：remember 缺 text，或 scope 是无法识别的字符串。
// 领域语义：模型会编造参数。工具层必须在到存储层之前把它挡住，并给出一条模型能
// 据此自我纠正的错误信息。
TEST_F(MemoryStoreTest, RememberToolRejectsMissingTextAndUnknownScope)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    const my_agent::tool::RegisteredTool remember = find_tool(
        my_agent::tool::detail::make_memory_tools(store), "remember"
    );

    const my_agent::tool::ExecResult no_text =
        remember.handler(nlohmann::json::object(),
                         my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(no_text.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, no_text.error().kind);

    const my_agent::tool::ExecResult bad_scope = remember.handler(nlohmann::json{
        {"text", "a fact"},
        {"scope", "global"},
    }, my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(bad_scope.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, bad_scope.error().kind);

    EXPECT_TRUE(store->load_all(memory::Scope::Project).empty());
}

// 场景：模型用 id 调 forget，再用子串调一次。
// 领域语义：两种寻址方式对应两种真实情况 —— 系统提示里带了 id 所以能精确删；
// 或者用户只说「别再记着 kubectl 那事」，只有子串可用。
TEST_F(MemoryStoreTest, ForgetToolRemovesByIdAndBySubstring)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    const std::vector<my_agent::tool::RegisteredTool> tools =
        my_agent::tool::detail::make_memory_tools(store);
    const my_agent::tool::RegisteredTool forget = find_tool(tools, "forget");

    const std::string id =
        store->append(memory::Scope::Project, "deploy via kubectl").id;
    ASSERT_TRUE(store->append(memory::Scope::Project, "uses zsh").error.empty());

    const my_agent::tool::ExecResult by_id =
        forget.handler(nlohmann::json{{"id", id}},
                       my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(by_id.has_value()) << by_id.error().message;
    EXPECT_NE(std::string::npos, by_id->text.find('1'));
    ASSERT_EQ(std::size_t{1}, store->load_all(memory::Scope::Project).size());

    const my_agent::tool::ExecResult by_text =
        forget.handler(nlohmann::json{{"text", "zsh"}},
                       my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(by_text.has_value()) << by_text.error().message;
    EXPECT_TRUE(store->load_all(memory::Scope::Project).empty());
}

// 场景：forget 一个不存在的 id。
// 领域语义：零命中不是失败 —— 模型引用了一个已经被删掉的 id 是常见情况。返回
// 成功但说明移除了 0 条，让模型自己判断，而不是抛错打断回合。
TEST_F(MemoryStoreTest, ForgetToolSucceedsWithZeroRemovalsForAnUnknownId)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    ASSERT_TRUE(store->append(memory::Scope::Project, "a fact").error.empty());

    const my_agent::tool::ExecResult result = find_tool(
        my_agent::tool::detail::make_memory_tools(store), "forget"
    ).handler(nlohmann::json{{"id", "deadbeef"}},
              my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_NE(std::string::npos, result->text.find('0'));
    EXPECT_EQ(std::size_t{1}, store->load_all(memory::Scope::Project).size());
}

// 场景：模型给 forget 传了一个描述性的短语，而不是记录里真实存在的子串。
// 领域语义：端到端验证抓到的真实缺陷 —— 提示里有 `[55f091af] User prefers fish
// shell`，用户说「忘掉我的 shell 偏好」，模型传了 text:"shell preference"。严格
// 子串匹配零命中，但工具回报"成功"，模型于是宣称删掉了，磁盘上记录还在。
// 零命中虽然不是错误（见上一个测试：过期 id 是常见情况），但输出必须让模型看出
// 什么都没被删，并指向 id 这条可靠路径，否则它会把失败当成功报给用户。
TEST_F(MemoryStoreTest, ForgetToolSaysNothingMatchedSoTheModelDoesNotClaimSuccess)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    ASSERT_TRUE(
        store->append(memory::Scope::Project, "User prefers fish shell")
            .error.empty()
    );

    const my_agent::tool::ExecResult result = find_tool(
        my_agent::tool::detail::make_memory_tools(store), "forget"
    ).handler(nlohmann::json{{"text", "shell preference"}},
              my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(std::size_t{1}, store->load_all(memory::Scope::Project).size());

    // "no memory record matched" 比 "forgot 0 record(s)" 更难被误读成成功。
    EXPECT_NE(std::string::npos, result->text.find("no memory record matched"));
    // 并且要指回 id —— 系统提示里每条记录都带 id，那才是可靠的寻址方式。
    EXPECT_NE(std::string::npos, result->text.find("id"));
}

// 场景：forget 既没给 id 也没给 text。
// 领域语义：无参 forget 无从判断该删什么。若当成「空子串」就会清空整个存储，
// 所以必须在工具层拒绝。
TEST_F(MemoryStoreTest, ForgetToolRequiresEitherAnIdOrAText)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    ASSERT_TRUE(store->append(memory::Scope::Project, "a fact").error.empty());

    const my_agent::tool::ExecResult result = find_tool(
        my_agent::tool::detail::make_memory_tools(store), "forget"
    ).handler(nlohmann::json::object(),
              my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, result.error().kind);
    EXPECT_EQ(std::size_t{1}, store->load_all(memory::Scope::Project).size());
}

// 场景：把一条记录渲染成系统提示里的一行。
// 领域语义：id 必须出现在渲染结果里。这是 remember 与 forget 之间的闭环 ——
// 模型只有在提示中看得到 id，才能在用户说「忘掉那条」时精确调 forget，
// 而不是靠子串猜。
TEST_F(MemoryStoreTest, RendersRecordsWithTheirIdSoTheModelCanForgetThemPrecisely)
{
    const memory::Record record{
        .id = "a1b2c3d4",
        .ts = 1731860000,
        .scope = memory::Scope::Project,
        .text = "builds with cmake",
    };

    const std::string line = memory::render_for_prompt(record);

    EXPECT_NE(std::string::npos, line.find("a1b2c3d4"));
    EXPECT_NE(std::string::npos, line.find("builds with cmake"));
}

// 场景：检查两个工具声明的 effects。
// 领域语义：effects 是权限系统的输入，必须**如实**描述工具做了什么。两者都写文件，
// 所以都带 WriteFs —— 把写文件的工具标成纯的会让整个 policy 层失去可信度。
// 默认 write profile 下 policy 一律 Allow，演示路径不受影响；ask/minimal 下用户
// 才能真正管住 agent 往磁盘上记了什么。
TEST_F(MemoryStoreTest, MemoryToolsDeclareWriteFsSoThePolicyLayerCanGateThem)
{
    auto store = std::make_shared<memory::MemoryStore>(roots());
    const std::vector<my_agent::tool::RegisteredTool> tools =
        my_agent::tool::detail::make_memory_tools(store);

    for (const char* name : {"remember", "forget"}) {
        const my_agent::tool::RegisteredTool tool = find_tool(tools, name);
        EXPECT_TRUE(tool.descriptor.effects.has(my_agent::tool::Effect::WriteFs)) << name;
    }
}
