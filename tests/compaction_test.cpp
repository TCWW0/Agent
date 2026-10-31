#include "my_agent/runtime/compaction.hpp"

#include "my_agent/runtime/context_projection.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {

using my_agent::AcceptedCompaction;
using my_agent::CompactionFailureKind;
using my_agent::CompactionId;
using my_agent::CompactionPlan;
using my_agent::CompactionPlanFailureKind;
using my_agent::ContextManagementConfig;
using my_agent::Message;
using my_agent::MessageId;
using my_agent::Request;
using my_agent::Role;
using my_agent::Thread;
using my_agent::TurnId;

ContextManagementConfig config()
{
    ContextManagementConfig result;
    result.configured_limit = 8192;
    result.safety_margin = 64;
    return result;
}

Message user(
    std::uint64_t message_id,
    std::uint64_t turn_id,
    std::string text)
{
    return Message{
        .role = Role::User,
        .text = std::move(text),
        .id = MessageId{message_id},
        .turn_id = TurnId{turn_id},
    };
}

Message assistant(
    std::uint64_t message_id,
    std::uint64_t turn_id,
    std::string text)
{
    return Message{
        .role = Role::Assistant,
        .text = std::move(text),
        .id = MessageId{message_id},
        .turn_id = TurnId{turn_id},
    };
}

Thread two_turns()
{
    return Thread{
        .messages = {
            user(1, 1, "first question"),
            assistant(2, 1, "first answer"),
            user(3, 2, "second question"),
            assistant(4, 2, "second answer"),
        },
    };
}

Thread validation_history()
{
    return Thread{
        .messages = {
            user(1, 1, std::string(500, 'u')),
            assistant(2, 1, std::string(500, 'a')),
            user(3, 2, std::string(500, 'v')),
            assistant(4, 2, std::string(500, 'b')),
        },
    };
}

Request next_request(const Thread& thread)
{
    return Request{.messages = thread.messages};
}

std::string valid_summary()
{
    return R"({
        "goal": "finish the task",
        "constraints": ["preserve identity"],
        "facts": ["the source was compacted"],
        "decisions": ["use one accepted summary"],
        "state": ["waiting for the next turn"],
        "next_steps": ["continue the conversation"]
    })";
}

CompactionPlan make_plan(
    const Thread& thread,
    const ContextManagementConfig& management = config())
{
    Request request = next_request(thread);
    request.max_output_tokens = management.normal_output_budget();
    auto planned = my_agent::make_compaction_plan(
        CompactionId{1},
        thread,
        request,
        my_agent::RequestPolicy{management, {}, "test-model"});
    EXPECT_TRUE(planned.has_value()) << planned.error().message;
    return *planned;
}

AcceptedCompaction accept(
    const CompactionPlan& plan,
    std::string_view summary = valid_summary(),
    const ContextManagementConfig& management = config())
{
    const auto validation = my_agent::validate_compaction(
        plan,
        summary);
    if (!validation.has_value()) {
        ADD_FAILURE() << validation.error().rejection_reason;
        return {};
    }
    return *validation;
}

TEST(CompactionCoreTest, FirstSourceContainsEveryCompletedTurn)
{
    const Thread thread = two_turns();
    const Request source = my_agent::effective_completed_history(thread);

    ASSERT_EQ(4U, source.messages.size());
    EXPECT_EQ("first question", source.messages[0].text);
    EXPECT_EQ("second answer", source.messages[3].text);
    for (std::size_t index = 0; index < thread.messages.size(); ++index) {
        EXPECT_EQ(thread.messages[index].id, source.messages[index].id);
        EXPECT_EQ(thread.messages[index].text, source.messages[index].text);
    }
}

TEST(CompactionCoreTest, QueuedUserMessageIsNeverInTheSource)
{
    Thread thread = two_turns();
    thread.messages.push_back(user(5, 3, "queued question"));

    const Request source = my_agent::effective_completed_history(thread);

    ASSERT_EQ(4U, source.messages.size());
    EXPECT_EQ("second answer", source.messages.back().text);
    EXPECT_EQ(
        std::string::npos,
        source.messages.back().text.find("queued question"));
}

TEST(CompactionCoreTest, PreviousSummaryIsSyntheticUserAndRawCoverageIsSkipped)
{
    const Thread thread = validation_history();
    const CompactionPlan first = make_plan(thread);
    const AcceptedCompaction previous = accept(first);

    Thread with_new_turn = thread;
    with_new_turn.messages.push_back(user(5, 3, "new question"));
    with_new_turn.messages.push_back(assistant(6, 3, "new answer"));
    with_new_turn.accepted_compaction = previous;
    const Request source = my_agent::effective_completed_history(with_new_turn);

    ASSERT_EQ(3U, source.messages.size());
    EXPECT_EQ(Role::User, source.messages.front().role);
    EXPECT_NE(std::string::npos, source.messages.front().text.find("goal"));
    EXPECT_EQ("new question", source.messages[1].text);
    EXPECT_EQ("new answer", source.messages[2].text);
    EXPECT_EQ(6U, with_new_turn.messages.size());
}

TEST(CompactionCoreTest, NoCompleteHistoryReturnsTypedFailure)
{
    Thread thread{
        .messages = {user(1, 1, "only queued input")},
    };
    const Request request = next_request(thread);

    const auto planned = my_agent::make_compaction_plan(
        CompactionId{1},
        thread,
        request,
        my_agent::RequestPolicy{config(), {}, "test-model"});

    ASSERT_FALSE(planned.has_value());
    EXPECT_EQ(
        CompactionPlanFailureKind::NoCompleteHistory,
        planned.error().kind);
}

TEST(CompactionCoreTest, CompactionSourceMustFitItsOwnAdmissionBudget)
{
    ContextManagementConfig management = config();
    management.configured_limit = 400;
    management.safety_margin = 16;
    Thread thread{
        .messages = {
            user(1, 1, std::string(4000, 'u')),
            assistant(2, 1, std::string(4000, 'a')),
        },
    };

    Request request = next_request(thread);
    request.max_output_tokens = management.normal_output_budget();
    const auto planned = my_agent::make_compaction_plan(
        CompactionId{1},
        thread,
        request,
        my_agent::RequestPolicy{management, {}, "test-model"});

    ASSERT_FALSE(planned.has_value());
    EXPECT_EQ(
        CompactionPlanFailureKind::SourceTooLarge,
        planned.error().kind);
}

TEST(CompactionCoreTest, RepeatedCompactionUsesS1AndOnlyNewCompletedTurns)
{
    const CompactionPlan first_plan = make_plan(validation_history());
    const AcceptedCompaction s1 = accept(first_plan);

    Thread second_thread = validation_history();
    second_thread.messages.push_back(user(5, 3, "new question"));
    second_thread.messages.push_back(assistant(6, 3, "new answer"));
    second_thread.accepted_compaction = s1;
    const CompactionPlan second_plan = make_plan(second_thread);
    const AcceptedCompaction s2 = accept(second_plan);

    EXPECT_EQ(Role::User, second_plan.source_request.messages.front().role);
    EXPECT_EQ("new question", second_plan.source_request.messages[1].text);
    EXPECT_EQ("new answer", second_plan.source_request.messages[2].text);
    EXPECT_EQ(second_plan.covered_through, s2.covered_through);
    EXPECT_EQ(6U, second_thread.messages.size());
    for (const Message& message : second_thread.messages) {
        EXPECT_EQ(std::string::npos, message.text.find("Earlier conversation"));
    }
}

TEST(CompactionValidationTest, AcceptsExactlyTheSixFieldJsonProtocol)
{
    const CompactionPlan plan = make_plan(validation_history());

    const auto validation = my_agent::validate_compaction(
        plan,
        valid_summary());

    if (!validation) {
        FAIL() << validation.error().rejection_reason;
    }
    EXPECT_EQ(6U, (*validation).summary.value.size());
}

TEST(CompactionValidationTest, RejectsWrongTypesAndExtraNestedFence)
{
    const CompactionPlan plan = make_plan(validation_history());
    const std::string wrong_type =
        R"({"goal":[],"constraints":[],"facts":[],"decisions":[],"state":[],"next_steps":[]})";
    const std::string nested_fence = "```json\n"
        + valid_summary() + "\n```\n```";

    const auto wrong_type_validation = my_agent::validate_compaction(
        plan,
        wrong_type);
    ASSERT_FALSE(wrong_type_validation.has_value());
    EXPECT_EQ(
        CompactionFailureKind::Schema,
        wrong_type_validation.error().kind);

    const auto nested_fence_validation = my_agent::validate_compaction(
        plan,
        nested_fence);
    ASSERT_FALSE(nested_fence_validation.has_value());
    EXPECT_EQ(
        CompactionFailureKind::Schema,
        nested_fence_validation.error().kind);
}

TEST(CompactionValidationTest, AllowsOneDeterministicOuterJsonFence)
{
    const CompactionPlan plan = make_plan(validation_history());
    const std::string fenced = "```json\n" + valid_summary() + "\n```";

    EXPECT_TRUE(my_agent::validate_compaction(
                    plan,
                    fenced)
                    .has_value());
}

TEST(CompactionValidationTest, RejectsOutputOverBudget)
{
    ContextManagementConfig management = config();
    management.compaction_output_ceiling = 1;
    const CompactionPlan plan = make_plan(validation_history(), management);

    const auto validation = my_agent::validate_compaction(
        plan,
        valid_summary());

    ASSERT_FALSE(validation.has_value());
    EXPECT_EQ(CompactionFailureKind::Size, validation.error().kind);
}

TEST(CompactionValidationTest, RejectsIdentityDigestAndNoNetSavings)
{
    const CompactionPlan plan = make_plan(validation_history());
    CompactionPlan changed_source = plan;
    changed_source.source_request.messages.front().text = "tampered";
    const auto changed_source_validation = my_agent::validate_compaction(
        changed_source,
        valid_summary());
    ASSERT_FALSE(changed_source_validation.has_value());
    EXPECT_EQ(
        CompactionFailureKind::Coverage,
        changed_source_validation.error().kind);

    CompactionPlan no_gain = plan;
    no_gain.before_tokens = 0;
    const auto no_gain_validation = my_agent::validate_compaction(
        no_gain,
        valid_summary());
    ASSERT_FALSE(no_gain_validation.has_value());
    EXPECT_EQ(
        CompactionFailureKind::NoGain,
        no_gain_validation.error().kind);

    CompactionPlan changed_request = plan;
    changed_request.compaction_request.system_prompt = "tampered";
    const auto changed_request_validation = my_agent::validate_compaction(
        changed_request,
        valid_summary());
    ASSERT_FALSE(changed_request_validation.has_value());
    EXPECT_EQ(
        CompactionFailureKind::Identity,
        changed_request_validation.error().kind);
}

TEST(CompactionValidationTest, RejectsProjectedRequestOverHardInputLimit)
{
    ContextManagementConfig management = config();
    management.configured_limit = 500;
    management.safety_margin = 16;
    Thread thread = Thread{
        .messages = {
            user(1, 1, "short"),
            assistant(2, 1, "short answer"),
        },
    };
    CompactionPlan plan = make_plan(thread, management);
    plan.next_request.messages.push_back(
        user(3, 2, std::string(4000, 'x')));

    const auto validation = my_agent::validate_compaction(
        plan,
        R"({"goal":"","constraints":[],"facts":[],"decisions":[],"state":[],"next_steps":[]})");
    ASSERT_FALSE(validation.has_value());
    EXPECT_EQ(CompactionFailureKind::Projection, validation.error().kind);
}

}  // namespace
