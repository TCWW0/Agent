#include "my_agent/runtime/request_preparation.hpp"
#include "my_agent/runtime/context_admission.hpp"

#include "my_agent/runtime/context_projection.hpp"

#include <gtest/gtest.h>

TEST(ContextMaintenanceTest, RefreshUsesTheUnifiedRequestBudget)
{
    my_agent::ContextManagementConfig management;
    management.configured_limit = 4096;
    management.safety_margin = 64;
    const my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::User,
                .text = "completed history",
                .id = my_agent::MessageId{1},
                .turn_id = my_agent::TurnId{1},
            },
        },
    };
    const auto prepared = my_agent::prepare_request_snapshot(request, {}, {},
        my_agent::RequestPolicy{management, {}, "test-model"}, {});

    EXPECT_EQ(1024U, prepared.request.max_output_tokens);
    EXPECT_EQ(3008U, prepared.budget.hard_input_limit);
    EXPECT_TRUE(prepared.budget.admitted);
}
