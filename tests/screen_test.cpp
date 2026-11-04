#include "my_agent/ui/screen.hpp"
#include "my_agent/ui/maya_animation.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/core/motion.hpp>
#include <maya/render/frame.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

my_agent::Model model_with(std::vector<my_agent::Message> messages = {})
{
    my_agent::Model model;
    model.domain.thread.messages = std::move(messages);
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

TEST(ScreenTest, GivesUserAndAssistantTurnsDistinctConversationIdentities)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "hello"},
        {.role = my_agent::Role::Assistant, .text = "hello back"},
    });
    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";

    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    ASSERT_EQ(2u, screen.transcript.turns.size());
    EXPECT_EQ("❯", screen.transcript.turns[0].glyph);
    EXPECT_EQ("You", screen.transcript.turns[0].label);
    EXPECT_EQ("✦", screen.transcript.turns[1].glyph);
    EXPECT_EQ("qwen3.5:latest", screen.transcript.turns[1].label);
}

TEST(ScreenTest, UsesTheRequestModelWhenNoUiModelNameIsAvailable)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::Assistant, .text = "hello"},
    });
    model.domain.request_policy =
        my_agent::RequestPolicy{{}, "ollama", "qwen2.5:7b"};

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});

    ASSERT_EQ(1u, screen.transcript.turns.size());
    EXPECT_EQ("qwen2.5:7b", screen.transcript.turns.front().label);
}

TEST(ScreenTest, GivesUserAndAssistantTextRoleSpecificBodySemantics)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "# literal prompt"},
        {.role = my_agent::Role::Assistant,
         .text = "## Answer",
         .tool_calls = {{.id = "call-1", .name = "read"}}},
    });

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});

    ASSERT_EQ(2u, screen.transcript.turns.size());
    ASSERT_EQ(1u, screen.transcript.turns[0].body.size());
    EXPECT_TRUE(std::holds_alternative<maya::Turn::PlainText>(
        screen.transcript.turns[0].body[0]));
    ASSERT_EQ(2u, screen.transcript.turns[1].body.size());
    EXPECT_TRUE(std::holds_alternative<maya::Turn::MarkdownText>(
        screen.transcript.turns[1].body[0]));
    EXPECT_TRUE(std::holds_alternative<maya::Turn::PlainText>(
        screen.transcript.turns[1].body[1]));
}

TEST(ScreenTest, PermissionCardUsesThePendingCallBinding)
{
    my_agent::Model model;
    model.domain.thread.messages.push_back({
        .role = my_agent::Role::Assistant,
        .text = "",
        .tool_calls = {{
            .id = "custom-call",
            .name = "custom_writer",
            .args = {{"target", "report.txt"}},
            .status = my_agent::ToolCall::Pending{
                .binding = my_agent::tool::ToolBinding{
                    .ref = my_agent::tool::ToolRef{7},
                    .effects = {my_agent::tool::Effect::WriteFs,
                                my_agent::tool::Effect::Net},
                }},
            .tool_exec_id = my_agent::ToolExecId{9},
        }},
        .id = my_agent::MessageId{11},
        .turn_id = my_agent::TurnId{3},
    });
    model.phase = my_agent::AwaitingPermission{
        .call = my_agent::ToolCallRef{
            .assistant_message_id = my_agent::MessageId{11},
            .tool_exec_id = my_agent::ToolExecId{9},
        },
    };

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    ASSERT_TRUE(screen.dock.permission.has_value());
    EXPECT_EQ("custom_writer", screen.dock.permission->tool_name);
    EXPECT_NE(std::string::npos,
              screen.dock.permission->description.find("write_fs,net"));
    EXPECT_NE(std::string::npos,
              screen.dock.permission->description.find("report.txt"));

    std::get<my_agent::AwaitingPermission>(model.phase).call.tool_exec_id =
        my_agent::ToolExecId{10};
    const my_agent::ui::ScreenConfig stale = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_FALSE(stale.dock.permission.has_value());
    EXPECT_NE(std::string::npos,
              stale.dock.status.phase.verb.find("permission state invalid"));
}

TEST(ScreenTest, ProjectsThinkingOnlyForAnEmptyAssistantTurn)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "hello"},
        {.role = my_agent::Role::Assistant, .text = ""},
    });
    model.phase = my_agent::Streaming{};

    const maya::Theme theme;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, my_agent::ui::UiState{}, theme);

    ASSERT_EQ(1u, screen.transcript.turns.size());
    ASSERT_TRUE(screen.transcript.in_flight.has_value());
    ASSERT_EQ(1u, screen.transcript.in_flight->words.size());
    EXPECT_EQ("thinking", screen.transcript.in_flight->words.front());
    EXPECT_EQ(theme.info.kind(), screen.transcript.in_flight->edge_color.kind());
    EXPECT_EQ(theme.info.r(), screen.transcript.in_flight->edge_color.r());
    EXPECT_EQ(theme.info.g(), screen.transcript.in_flight->edge_color.g());
    EXPECT_EQ(theme.info.b(), screen.transcript.in_flight->edge_color.b());
    EXPECT_EQ(0u, screen.transcript.in_flight->stream_bytes);
    EXPECT_FLOAT_EQ(0.0f, screen.transcript.in_flight->stream_rate);
}

TEST(ScreenTest, RemovesThinkingAfterTextToolCallErrorOrPhaseChange)
{
    const maya::Theme theme;
    const auto project = [&theme](my_agent::Model model) {
        return my_agent::ui::project_screen(
            model, my_agent::ui::UiState{}, theme);
    };

    my_agent::Model text = model_with({
        {.role = my_agent::Role::Assistant, .text = "first token"},
    });
    text.phase = my_agent::Streaming{};
    EXPECT_FALSE(project(text).transcript.in_flight.has_value());

    my_agent::Model tool = model_with({
        {.role = my_agent::Role::Assistant,
         .text = "",
         .tool_calls = {{.id = "call-1", .name = "read"}}},
    });
    tool.phase = my_agent::Streaming{};
    EXPECT_FALSE(project(tool).transcript.in_flight.has_value());

    my_agent::Model error = model_with({
        {.role = my_agent::Role::Assistant,
         .text = "",
         .error = std::string{"provider failed"}},
    });
    error.phase = my_agent::Streaming{};
    EXPECT_FALSE(project(error).transcript.in_flight.has_value());

    my_agent::Model idle = model_with({
        {.role = my_agent::Role::Assistant, .text = ""},
    });
    idle.phase = my_agent::Idle{};
    EXPECT_FALSE(project(idle).transcript.in_flight.has_value());

    my_agent::Model user = model_with({
        {.role = my_agent::Role::User, .text = "hello"},
    });
    user.phase = my_agent::Streaming{};
    EXPECT_FALSE(project(user).transcript.in_flight.has_value());
}

void count_animation_request(void* context) noexcept
{
    ++*static_cast<int*>(context);
}

void count_delayed_animation_request(void* context, std::int64_t) noexcept
{
    ++*static_cast<int*>(context);
}

TEST(ScreenTest, ActivePhasesRequestBreathingButIdleDoesNot)
{
    int requests = 0;
    my_agent::ui::MayaAnimationHostScope host{
        &requests,
        &count_animation_request,
        &count_delayed_animation_request,
    };

    maya::testing::freeze_anim_clock(1'000);

    my_agent::Model active;
    active.phase = my_agent::Streaming{};
    const auto first = my_agent::ui::project_screen(
        active, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_GT(requests, 0);
    maya::testing::advance_anim_clock_ms(100);
    const auto second = my_agent::ui::project_screen(
        active, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_NE(first.dock.status.phase.frame, second.dock.status.phase.frame);

    requests = 0;
    my_agent::Model idle;
    idle.phase = my_agent::Idle{};
    static_cast<void>(my_agent::ui::project_screen(
        idle, my_agent::ui::UiState{}, maya::Theme{}));
    EXPECT_EQ(0, requests);
    maya::testing::unfreeze_anim_clock();
}

TEST(ScreenTest, ProjectsBoundedTelemetryHistoryIntoTheTokenSparkline)
{
    my_agent::Model model;
    model.phase = my_agent::Streaming{};
    my_agent::ui::UiState ui;
    ui.status.tokens_per_second = 24.0;
    ui.status.tokens_generated = 42;
    ui.token_rate_history.observe(12.0, 1);
    ui.token_rate_history.observe(24.0, 2);

    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    EXPECT_TRUE(screen.dock.status.token_stream.live);
    EXPECT_EQ(42, screen.dock.status.token_stream.total);
    ASSERT_EQ(2u, screen.dock.status.token_stream.history.size());
    EXPECT_FLOAT_EQ(12.0f, screen.dock.status.token_stream.history.front());
    EXPECT_FLOAT_EQ(24.0f, screen.dock.status.token_stream.history.back());
}

TEST(ScreenTest, ProjectsCoreUsageFactsIntoTheRenderedStatusBar)
{
    my_agent::Model model;
    model.session.usage.latest_request = my_agent::UsageObservation{
        .operation = my_agent::StreamRef{my_agent::TurnId{1}, my_agent::StreamId{1}},
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
        .totals = my_agent::UsageTotals{
            .input_tokens = 180,
            .output_tokens = 42,
        },
    };
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };

    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";
    ui.status.context_limit = 8192;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    maya::FrameBuffer framebuffer{160, 3};
    const std::string& bytes = framebuffer.render(
        my_agent::ui::status_bar_element(screen.dock.status),
        maya::theme::dark);

    EXPECT_NE(std::string::npos, bytes.find("req in 128 out 36")) << bytes;
    EXPECT_NE(std::string::npos, bytes.find("turn in 180 out 42")) << bytes;
}

TEST(ScreenTest, HidesRequestAndTurnUsageBeforeCoreMetadataOnNarrowWidth)
{
    my_agent::Model model;
    model.session.usage.latest_request = my_agent::UsageObservation{
        .operation = my_agent::StreamRef{my_agent::TurnId{1}, my_agent::StreamId{1}},
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
        .totals = my_agent::UsageTotals{
            .input_tokens = 180,
            .output_tokens = 42,
        },
    };
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };

    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";
    ui.status.context_limit = 8192;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    maya::FrameBuffer framebuffer{90, 3};
    const std::string& bytes = framebuffer.render(
        my_agent::ui::status_bar_element(screen.dock.status),
        maya::theme::dark);

    EXPECT_EQ(std::string::npos, bytes.find("req in")) << bytes;
    EXPECT_EQ(std::string::npos, bytes.find("turn in")) << bytes;
    EXPECT_NE(std::string::npos, bytes.find("qwen3.5:latest")) << bytes;
    EXPECT_NE(std::string::npos, bytes.find("ctx")) << bytes;
}

TEST(ScreenTest, HidesTurnUsageBeforeRequestUsageAtMediumWidth)
{
    my_agent::Model model;
    model.session.usage.latest_request = my_agent::UsageObservation{
        .operation = my_agent::StreamRef{my_agent::TurnId{1}, my_agent::StreamId{1}},
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
        .totals = my_agent::UsageTotals{
            .input_tokens = 180,
            .output_tokens = 42,
        },
    };
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Exact,
    };

    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";
    ui.status.context_limit = 8192;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    maya::FrameBuffer framebuffer{125, 3};
    const std::string& bytes = framebuffer.render(
        my_agent::ui::status_bar_element(screen.dock.status),
        maya::theme::dark);

    EXPECT_NE(std::string::npos, bytes.find("req in")) << bytes;
    EXPECT_EQ(std::string::npos, bytes.find("turn in")) << bytes;
}

TEST(ScreenTest, ProjectsTranscriptGuidanceAlternatives)
{
    const my_agent::Model model;
    const maya::Theme theme;
    my_agent::ui::UiState ui;

    auto project = [&] {
        return my_agent::ui::project_screen(model, ui, theme)
            .dock.transcript_guidance;
    };

    EXPECT_TRUE(project().full.empty());

    ui.transcript_hint = my_agent::ui::TranscriptHintKind::Latest;
    auto latest = project();
    ASSERT_EQ(1u, latest.full.size());
    EXPECT_EQ("Ctrl+End · latest", latest.full.front().text);
    ASSERT_EQ(1u, latest.compact.size());
    EXPECT_EQ("↓ Ctrl+End", latest.compact.front().text);

    ui.transcript_hint = my_agent::ui::TranscriptHintKind::NewOutput;
    auto output = project();
    ASSERT_EQ(2u, output.full.size());
    EXPECT_EQ("↓ new output", output.full[0].text);
    EXPECT_EQ(" · Ctrl+End", output.full[1].text);
    ASSERT_TRUE(output.full[0].style.fg.has_value());
    EXPECT_EQ(theme.info, *output.full[0].style.fg);
    ASSERT_TRUE(output.full[1].style.fg.has_value());
    EXPECT_EQ(theme.muted, *output.full[1].style.fg);
    ASSERT_EQ(1u, output.compact.size());
    EXPECT_EQ("↓ Ctrl+End", output.compact.front().text);
}

TEST(ScreenTest, ProjectsQueueHeadAsACompactDockPreview)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "active"},
        {.role = my_agent::Role::Assistant, .text = "reply"},
    });
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{7},
        .message_id = my_agent::MessageId{8},
        .text = "first queued prompt",
    });
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{9},
        .message_id = my_agent::MessageId{10},
        .text = "second queued prompt",
    });

    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(
            model, my_agent::ui::UiState{}, maya::Theme{});

    ASSERT_EQ(std::size_t{2}, screen.transcript.turns.size());
    EXPECT_EQ("reply\n", turn_text(screen.transcript.turns.back()));
    ASSERT_TRUE(screen.dock.queue_preview.has_value());
    EXPECT_EQ(
        "queued 1 / 2 · Ctrl-X clear",
        screen.dock.queue_preview->label);
    EXPECT_EQ("first queued prompt", screen.dock.queue_preview->text);
    EXPECT_EQ(std::size_t{2}, screen.dock.composer.queued);
    EXPECT_EQ(
        "Ctrl-X clear queued prompts",
        screen.dock.composer.queued_placeholder);
}

TEST(ScreenTest, UsesSolidCircleActivityGlyphs)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::Assistant, .text = "reply"},
    });
    const maya::Theme theme;

    model.phase = my_agent::Streaming{};
    EXPECT_EQ("●", my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, theme).dock.status.phase.glyph);

    model.phase = my_agent::ExecutingTool{};
    EXPECT_EQ("●", my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, theme).dock.status.phase.glyph);

    model.phase = my_agent::Idle{};
    EXPECT_EQ("○", my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, theme).dock.status.phase.glyph);
}

TEST(ScreenTest, ProjectsBlockedQueueCountAndReason)
{
    my_agent::Model model;
    model.phase = my_agent::Blocked{
        .reason = my_agent::BlockReason::Authentication,
        .message = "token expired",
    };
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{1},
        .message_id = my_agent::MessageId{1},
        .text = "waiting prompt",
    });

    const auto screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_EQ(std::size_t{1}, screen.dock.composer.queued);
    EXPECT_EQ("!", screen.dock.status.phase.glyph);
    EXPECT_EQ("Blocked", screen.dock.status.phase.verb);
    ASSERT_TRUE(screen.dock.queue_preview.has_value());
    EXPECT_NE(std::string::npos,
        screen.dock.queue_preview->label.find("Ctrl-X clear"));
}

TEST(ScreenTest, ExplainsContextLimitBlockAndHowToRecover)
{
    my_agent::Model model;
    model.phase = my_agent::Blocked{
        .reason = my_agent::BlockReason::ContextLimit,
        .budget = my_agent::ContextBudgetAssessment{
            .projected_input_tokens = 8120,
            .hard_input_limit = 7040,
        },
        .message = "request exceeds the available context budget",
    };
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{1},
        .message_id = my_agent::MessageId{1},
        .text = "oversized pasted prompt",
    });

    const auto screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_EQ("!", screen.dock.status.phase.glyph);
    EXPECT_EQ("Ctx limit", screen.dock.status.phase.verb);
    ASSERT_TRUE(screen.dock.queue_preview.has_value());
    EXPECT_NE(std::string::npos,
        screen.dock.queue_preview->label.find("ctx ~8120 > 7040"));
    EXPECT_NE(std::string::npos,
        screen.dock.queue_preview->label.find("estimate"));
    EXPECT_NE(std::string::npos,
        screen.dock.queue_preview->label.find("Ctrl-X clear"));
}

TEST(ScreenTest, ProjectsCompactingPhaseAndQueuedCount)
{
    my_agent::Model model;
    model.phase = my_agent::Compacting{};
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{1},
        .message_id = my_agent::MessageId{1},
        .text = "queued",
    });

    const auto screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_EQ(std::size_t{1}, screen.dock.composer.queued);
    EXPECT_NE(std::string::npos,
        screen.dock.status.phase.verb.find("Compacting context"));
    EXPECT_NE(std::string::npos,
        screen.dock.status.phase.verb.find("1 queued"));
}

TEST(ScreenTest, ProjectsCompactingActivityInTranscriptOnlyWhileCompacting)
{
    my_agent::Model model = model_with({
        {.role = my_agent::Role::User, .text = "old user"},
        {.role = my_agent::Role::Assistant, .text = "old answer"},
    });
    model.phase = my_agent::Compacting{};

    const auto compacting = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});

    ASSERT_TRUE(compacting.transcript.in_flight.has_value());
    ASSERT_EQ(1u, compacting.transcript.in_flight->words.size());
    EXPECT_EQ("Compacting context…",
              compacting.transcript.in_flight->words.front());
    ASSERT_EQ(2u, compacting.transcript.turns.size());

    model.phase = my_agent::Idle{};
    const auto idle = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_FALSE(idle.transcript.in_flight.has_value());
}

TEST(ScreenTest, LabelsCompactionRetryAndFinalBlock)
{
    my_agent::Model model;
    model.phase = my_agent::Compacting{
    };
    auto screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_NE(std::string::npos,
              screen.dock.status.phase.verb.find("Compacting context"));

    model.phase = my_agent::PreparingRequest{.preparation_id = {}, .intent = my_agent::RetryRequest{{}, {}}};
    screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_NE(std::string::npos,
              screen.dock.status.phase.verb.find("Retrying context"));

    model.phase = my_agent::Blocked{
        .reason = my_agent::BlockReason::ContextLimit,
        .message = "context limit",
    };
    screen = my_agent::ui::project_screen(
        model, my_agent::ui::UiState{}, maya::Theme{});
    EXPECT_EQ("Ctx limit", screen.dock.status.phase.verb);
}

TEST(ScreenTest, ProjectsObservedContextIntoTheMayaGauge)
{
    my_agent::Model model;
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 7000,
            .output_tokens = 120,
        },
        .quality = my_agent::UsageQuality::Exact,
    };

    my_agent::ui::UiState ui;
    ui.status.context_limit = 8192;
    const auto screen = my_agent::ui::project_screen(
        model, ui, maya::Theme{});

    EXPECT_EQ(7120, screen.dock.status.context.used);
    EXPECT_EQ(8192, screen.dock.status.context.max);
}

TEST(ScreenTest, RendersObservedContextQualityAlongsideTheMayaGauge)
{
    my_agent::Model model;
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Last,
    };

    my_agent::ui::UiState ui;
    ui.status.context_limit = 8192;
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, maya::Theme{});

    maya::FrameBuffer framebuffer{160, 3};
    const std::string& bytes = framebuffer.render(
        my_agent::ui::status_bar_element(screen.dock.status),
        maya::theme::dark);

    EXPECT_NE(std::string::npos, bytes.find("ctx last")) << bytes;
}

TEST(ScreenTest, RendersPasteAsACompactComposerChip)
{
    my_agent::ui::UiState ui;
    ui.pastes.push_back(my_agent::ui::PendingPaste{
        .body = "line one\nline two\nline three",
        .line_count = 3,
        .byte_count = 28,
    });
    ui.input = my_agent::ui::make_paste_placeholder(0);
    ui.cursor = ui.input.size();

    const auto screen = my_agent::ui::project_screen(
        my_agent::Model{}, ui, maya::Theme{});

    EXPECT_EQ("[Pasted text · 3 lines · 28 B]", screen.dock.composer.text);
    EXPECT_EQ(static_cast<int>(screen.dock.composer.text.size()),
        screen.dock.composer.cursor);
}

}  // namespace

TEST(ScreenTest, ReadsLatestUsageFromTheCurrentModelWithTheSameUiInput)
{
    my_agent::Model model;
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{.input_tokens = 12, .output_tokens = 3},
        .quality = my_agent::UsageQuality::Exact,
    };
    const my_agent::ui::UiState ui;
    EXPECT_EQ(15, my_agent::ui::project_screen(model, ui, maya::Theme{}).dock.status.context.used);

    model.session.usage.observed_context->totals =
        my_agent::UsageTotals{.input_tokens = 100, .output_tokens = 20};
    EXPECT_EQ(120, my_agent::ui::project_screen(model, ui, maya::Theme{}).dock.status.context.used);
    model.session.usage.observed_context.reset();
    EXPECT_EQ(0, my_agent::ui::project_screen(model, ui, maya::Theme{}).dock.status.context.used);
}
