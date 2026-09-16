#include "my_agent/ui/screen.hpp"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

my_agent::Model model_with(std::vector<my_agent::Message> messages = {})
{
    my_agent::Model model;
    model.thread.messages = std::move(messages);
    return model;
}

// 把一个 turn 的全部文本槽内容拼起来（PlainText / MarkdownText）。
// 断言「transcript 里有什么」用：不关心渲染，只看语义内容落在哪一侧。
std::string turn_text(const maya::Turn::Config& turn)
{
    std::string out;
    for (const maya::Turn::BodySlot& slot : turn.body) {
        std::visit(
            [&out](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, maya::Turn::PlainText>) {
                    out += s.content;
                } else if constexpr (std::is_same_v<T, maya::Turn::MarkdownText>) {
                    out += s.content;
                }
            },
            slot);
        out += '\n';
    }
    return out;
}

// 场景：一条用户消息、流式回复进行中、composer 打了半截字。
// 领域语义：屏幕的语义结构必须把「已结算的对话内容」（transcript）与
// 「活动中的 chrome」（dock）分成两个字段 —— 半截输入只属于 dock 的
// composer，对话历史只属于 transcript。这是 #26 第一条验收的公开行为：
// 投影结果以独立字段区分两侧，而不是靠行序在扁平帧里猜。
// Red 原因：project_screen 是桩（返回默认构造），transcript 空、composer 空。
TEST(ScreenTest, SeparatesTranscriptContentFromDockChrome)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "hello"},
    });
    model.phase = my_agent::Streaming{};

    my_agent::ui::UiState ui;
    ui.input = "draft";

    const maya::Theme theme;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, theme);

    // transcript 侧：对话历史在场，且不含 dock 的半截输入。
    ASSERT_EQ(1u, screen.transcript.turns.size());
    const std::string transcript_text =
        turn_text(screen.transcript.turns.front());
    EXPECT_NE(std::string::npos, transcript_text.find("hello"));
    EXPECT_EQ(std::string::npos, transcript_text.find("draft"));

    // dock 侧：composer 半截输入、无权限请求时 slot 为空、相位活动在场。
    EXPECT_EQ("draft", screen.dock.composer.text);
    EXPECT_FALSE(screen.dock.permission.has_value());
    EXPECT_FALSE(screen.dock.status.phase.verb.empty());

    // 分离的反向：对话历史不漏进 composer。
    EXPECT_EQ(std::string::npos, screen.dock.composer.text.find("hello"));
}

}  // namespace
