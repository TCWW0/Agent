#include "my_agent/ui/status_bar.hpp"
#include "my_agent/runtime/model.hpp"

#include <gtest/gtest.h>

TEST(StatusBarTest, MissingFinalUsageRemainsUnavailable)
{
    my_agent::Model model;
    model.session.usage.turn = my_agent::TurnUsageState{
        .turn_id = my_agent::TurnId{1},
    };

    const auto status = my_agent::ui::project_model_usage(model, {
        .context_limit = 8192,
    });

    EXPECT_EQ(my_agent::UsageQuality::Unavailable, status.turn_quality);
    EXPECT_EQ(my_agent::UsageQuality::Unavailable, status.context_quality);
    EXPECT_FALSE(status.observed_context_tokens.has_value());
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

    const auto status = my_agent::ui::project_model_usage(model, {
        .context_limit = 8192,
        .observed_context_tokens = 999,
    });

    EXPECT_EQ(15U, status.observed_context_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Exact, status.context_quality);
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

    const auto status = my_agent::ui::project_model_usage(model, {});

    EXPECT_EQ(100U, status.request_input_tokens);
    EXPECT_EQ(20U, status.request_output_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Exact, status.request_quality);
    EXPECT_EQ(150U, status.turn_input_tokens);
    EXPECT_EQ(30U, status.turn_output_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Unavailable, status.turn_quality);
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

    const auto last = my_agent::ui::project_model_usage(model, {});
    EXPECT_EQ(164U, last.observed_context_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Last, last.context_quality);

    model.session.usage.observed_context->quality = my_agent::UsageQuality::Stale;
    const auto stale = my_agent::ui::project_model_usage(model, {});
    EXPECT_EQ(164U, stale.observed_context_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Stale, stale.context_quality);
}
