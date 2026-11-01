#include "my_agent/ui/screen.hpp"
#include "my_agent/runtime/model.hpp"
#include "virtual_terminal.hpp"

#include <maya/render/frame.hpp>
#include <gtest/gtest.h>

namespace {

std::string status_text(
    const my_agent::Model& model,
    const my_agent::ui::UiState& ui = {})
{
    const auto screen = my_agent::ui::project_screen(
        model, ui, maya::theme::dark);
    maya::FrameBuffer framebuffer{220, 3};
    my_agent::test::VirtualTerminal terminal{220, 3};
    terminal.feed(framebuffer.render(
        my_agent::ui::status_bar_element(screen.dock.status),
        maya::theme::dark));
    std::string text;
    for (const auto& line : terminal.screen()) {
        text += line + '\n';
    }
    return text;
}

}  // namespace

TEST(StatusBarTest, MissingFinalUsageRemainsUnavailable)
{
    my_agent::Model model;
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
    };
    my_agent::ui::UiState ui;
    ui.status.context_limit = 8192;

    const auto text = status_text(model, ui);
    EXPECT_NE(std::string::npos, text.find("turn unavailable"));
    EXPECT_NE(std::string::npos, text.find("ctx unavailable"));
    EXPECT_EQ(0, my_agent::ui::project_screen(
        model, ui, maya::Theme{}).dock.status.context.used);
}

TEST(StatusBarTest, BlockedEstimateDoesNotReplaceObservedContext)
{
    my_agent::Model model;
    model.phase = my_agent::Blocked{
        .reason = my_agent::BlockReason::ContextLimit,
        .budget = my_agent::ContextBudgetAssessment{
            .projected_input_tokens = 8120,
            .hard_input_limit = 7040,
            .estimated = true,
        },
    };
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 12,
            .output_tokens = 3,
        },
        .quality = my_agent::UsageQuality::Exact,
    };
    my_agent::ui::UiState ui;
    ui.status.context_limit = 8192;

    const auto screen = my_agent::ui::project_screen(
        model, ui, maya::Theme{});
    EXPECT_EQ(15, screen.dock.status.context.used);
    EXPECT_NE(std::string::npos, status_text(model, ui).find("ctx exact"));
}

TEST(StatusBarTest, RequestAndTurnUsageKeepTheirOwnQualities)
{
    my_agent::Model model;
    model.session.usage.latest_request = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 100,
            .output_tokens = 20,
        },
        .quality = my_agent::UsageQuality::Exact,
    };
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
        .totals = my_agent::UsageTotals{
            .input_tokens = 150,
            .output_tokens = 30,
        },
    };

    const auto unavailable = status_text(model);
    EXPECT_NE(std::string::npos,
              unavailable.find("req in 100 out 20"));
    EXPECT_NE(std::string::npos,
              unavailable.find("turn unavailable"));

    model.session.usage.latest_request->operation =
        my_agent::StreamRef{my_agent::TurnId{1}, my_agent::StreamId{1}};
    EXPECT_NE(std::string::npos,
              status_text(model).find("turn in 150 out 30"));
}

TEST(StatusBarTest, LastAndStaleContextRemainDistinguishable)
{
    my_agent::Model model;
    model.session.usage.observed_context = my_agent::UsageObservation{
        .totals = my_agent::UsageTotals{
            .input_tokens = 128,
            .output_tokens = 36,
        },
        .quality = my_agent::UsageQuality::Last,
    };
    const my_agent::ui::UiState ui;

    EXPECT_EQ(164, my_agent::ui::project_screen(
        model, ui, maya::Theme{}).dock.status.context.used);
    EXPECT_NE(std::string::npos, status_text(model, ui).find("ctx last"));
    model.session.usage.observed_context->quality =
        my_agent::UsageQuality::Stale;
    EXPECT_EQ(164, my_agent::ui::project_screen(
        model, ui, maya::Theme{}).dock.status.context.used);
    EXPECT_NE(std::string::npos, status_text(model, ui).find("ctx stale"));
}

TEST(StatusBarTest, CurrentTurnQualityIsDerivedFromTheLatestStreamOwner)
{
    my_agent::Model model;
    model.session.usage.latest_request = my_agent::UsageObservation{
        .operation = my_agent::StreamRef{
            my_agent::TurnId{1}, my_agent::StreamId{3}},
        .quality = my_agent::UsageQuality::Exact,
    };
    model.session.usage.begin_turn(my_agent::TurnId{2});
    EXPECT_NE(std::string::npos,
              status_text(model).find("turn unavailable"));
    model.session.usage.begin_turn(my_agent::TurnId{1});
    const auto current = status_text(model);
    EXPECT_NE(std::string::npos, current.find("turn in 0 out 0"));
    EXPECT_EQ(std::string::npos, current.find("turn unavailable"));
    model.session.usage.latest_request->operation = my_agent::CompactionId{1};
    EXPECT_NE(std::string::npos,
              status_text(model).find("turn unavailable"));
}
