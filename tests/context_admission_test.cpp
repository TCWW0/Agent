#include "context_projection_test_support.hpp"
#include "my_agent/runtime/context_admission.hpp"

#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/request_preparation.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace {

my_agent::ContextManagementConfig config()
{
    my_agent::ContextManagementConfig result;
    result.configured_limit = 8192;
    result.safety_margin = 128;
    return result;
}

TEST(RequestPreparationTest, OptionalSystemPromptPreservesOrReplacesTheDraft)
{
    const auto management = config();
    my_agent::Request draft = request();
    draft.system_prompt = "base prompt";

    const auto without_provider = my_agent::prepare_request_snapshot(
        draft,
        {},
        std::nullopt,
        my_agent::RequestPolicy{management, "test-provider", "test-model"},
        {});
    EXPECT_EQ("base prompt", without_provider.request.system_prompt);

    const auto empty_provider = my_agent::prepare_request_snapshot(
        draft,
        {},
        std::string{},
        my_agent::RequestPolicy{management, "test-provider", "test-model"},
        {});
    EXPECT_TRUE(empty_provider.request.system_prompt.empty());
    EXPECT_NE(without_provider.receipt.fingerprint,
        empty_provider.receipt.fingerprint);
}

my_agent::Request request()
{
    return my_agent::Request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::User,
                .text = "hello",
                .id = my_agent::MessageId{1},
                .turn_id = my_agent::TurnId{1},
            },
        },
    };
}

}  // namespace

TEST(ContextManagementConfigTest, DerivesTheBatchOneOutputBudgets)
{
    const auto management = config();

    EXPECT_EQ(2048U, management.normal_output_budget());
    EXPECT_EQ(1024U, management.compaction_output_budget());
}

TEST(ContextManagementConfigTest, ProviderLimitTightensBothBudgets)
{
    auto management = config();
    management.provider_limit = 4096;

    EXPECT_EQ(4096U, management.effective_limit());
    EXPECT_EQ(1024U, management.normal_output_budget());
    EXPECT_EQ(512U, management.compaction_output_budget());
}

TEST(ContextBudgetTest, AdmissionUsesTheRequestOutputBudget)
{
    const auto management = config();
    const auto projection = my_agent_test::project_context(
        "test-model",
        request());

    const auto budget = my_agent::assess_context_admission(
        projection,
        management,
        1000);

    EXPECT_EQ(7064U, budget.hard_input_limit);
    EXPECT_TRUE(budget.admitted);
}

TEST(ContextBudgetTest, RequestOutputBudgetChangesTheHardLimit)
{
    const auto management = config();
    const auto projection = my_agent_test::project_context(
        "test-model",
        request());

    const auto normal = my_agent::assess_context_admission(
        projection,
        management,
        management.normal_output_budget());
    const auto constrained = my_agent::assess_context_admission(
        projection,
        management,
        4000);

    EXPECT_EQ(6016U, normal.hard_input_limit);
    EXPECT_EQ(4064U, constrained.hard_input_limit);
}

TEST(ContextManagementConfigTest, InvalidSafetyMarginIsRejected)
{
    auto management = config();
    management.safety_margin = management.configured_limit;

    EXPECT_THROW(management.validate(), std::invalid_argument);
}

TEST(ContextProjectionTest, FingerprintIncludesTheModelAndRequest)
{
    const auto first = my_agent_test::project_context("model-a", request());
    const auto second = my_agent_test::project_context("model-b", request());

    EXPECT_NE(first.fingerprint, second.fingerprint);
    EXPECT_EQ(first.realistic_input_tokens, first.projected_input_tokens);
}
