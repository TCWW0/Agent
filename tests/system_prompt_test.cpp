#include "my_agent/prompt/system_prompt.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

// 场景：只有环境事实，没有 memory 也没有 skill。
// 领域语义：系统提示必须携带 agent 赖以定位自己的环境事实 —— 工作目录决定了
// read 工具的边界，模型需要知道它在哪。
// Red 原因：仓库尚不存在 prompt::build（编译错误）。
TEST(SystemPromptTest, IncludesEnvironmentFacts)
{
    const my_agent::prompt::Context context{
        .working_directory = "/home/dev/project",
        .operating_system = "Linux",
    };

    const std::string prompt = my_agent::prompt::build(context);

    EXPECT_NE(std::string::npos, prompt.find("/home/dev/project"));
    EXPECT_NE(std::string::npos, prompt.find("Linux"));
}

// 场景：没有 memory 时。
// 领域语义：空的分节不应该出现。给模型一个空的 <memory></memory> 只是噪声，
// 还可能被当成"我确实没有任何记忆"的强信号。分节按需出现。
TEST(SystemPromptTest, OmitsMemoryBlockWhenThereAreNoMemories)
{
    const my_agent::prompt::Context context{
        .working_directory = "/tmp",
        .operating_system = "Linux",
    };

    const std::string prompt = my_agent::prompt::build(context);

    EXPECT_EQ(std::string::npos, prompt.find("<memory>"));
}

// 场景：有若干条 memory。
// 领域语义：M7 的注入点。memory 逐条成行地放进 <memory> 块，模型每轮都能看到。
TEST(SystemPromptTest, RendersMemoriesInsideAMemoryBlock)
{
    const my_agent::prompt::Context context{
        .working_directory = "/tmp",
        .operating_system = "Linux",
        .memories = {"User prefers concise answers.", "Project uses C++23."},
    };

    const std::string prompt = my_agent::prompt::build(context);

    const std::size_t open = prompt.find("<memory>");
    const std::size_t close = prompt.find("</memory>");
    ASSERT_NE(std::string::npos, open);
    ASSERT_NE(std::string::npos, close);
    EXPECT_LT(open, close);

    const std::size_t first = prompt.find("User prefers concise answers.");
    const std::size_t second = prompt.find("Project uses C++23.");
    ASSERT_NE(std::string::npos, first);
    ASSERT_NE(std::string::npos, second);
    // 两条都必须落在块内，且保持给定顺序。
    EXPECT_GT(first, open);
    EXPECT_LT(second, close);
    EXPECT_LT(first, second);
}

// 场景：有 skill 目录。
// Red/领域语义：Tier1 渐进披露 —— 系统提示里只放 skill 目录（名字 + 一句描述），
// 正文要等模型主动 activate 才加载。目录整块由 SkillEngine 生成，这里只负责放
// 进去，保证切片 8 有一个不用改提示构建器就能接上的位置。
TEST(SystemPromptTest, EmbedsSkillsCatalogWhenPresent)
{
    const my_agent::prompt::Context context{
        .working_directory = "/tmp",
        .operating_system = "Linux",
        .memories = {},
        .skills_catalog = "<skills>\n- pdf — Extract text from PDFs\n</skills>",
    };

    const std::string prompt = my_agent::prompt::build(context);

    EXPECT_NE(std::string::npos, prompt.find("<skills>"));
    EXPECT_NE(std::string::npos, prompt.find("- pdf — Extract text from PDFs"));
}

// 场景：检查静态基底里关于 remember 的指引。
// 领域语义：这条指引是端到端手动验证逼出来的，不是设计时想到的。qwen3.5 在流式
// 模式下遇到「记住我偏好 fish」只会在散文里答应一句然后什么也不做 —— 回合结束，
// 事实丢失。已有的那条「工具能更可靠地回答问题时就用工具」不覆盖 remember，因为
// remember 不回答任何问题。加上这条后流式下才会真的调 remember。
// 锁住它，否则日后重排提示会静默退化成"假装记住"。
TEST(SystemPromptTest, InstructsTheModelToCallRememberRatherThanAcknowledgeInProse)
{
    const std::string prompt = my_agent::prompt::build(my_agent::prompt::Context{
        .working_directory = "/tmp",
        .operating_system = "Linux",
    });

    EXPECT_NE(std::string::npos, prompt.find("remember tool"));
}

// 场景：采集进程级操作系统事实。
// working_directory 属于会话边界，由 SessionServices 提供；prompt 层只负责
// 不随会话变化的宿主操作系统。
TEST(SystemPromptTest, HostOperatingSystemIsAvailable)
{
    EXPECT_FALSE(my_agent::prompt::host_operating_system().empty());
}

}  // namespace
