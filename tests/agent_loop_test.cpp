#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/context_admission.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "request_preparation_test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <variant>

namespace {

my_agent::Message message(
    my_agent::Role role,
    std::uint64_t message_id,
    std::uint64_t turn_id,
    std::string text)
{
    return my_agent::Message{
        .role = role,
        .text = std::move(text),
        .id = my_agent::MessageId{message_id},
        .turn_id = my_agent::TurnId{turn_id},
    };
}

my_agent::ContextProjectionReceipt receipt_for_test(
    const my_agent::ContextProjection& projection)
{
    return my_agent::ContextProjectionReceipt{
        .provider = projection.provider,
        .model = projection.model,
        .fingerprint = projection.fingerprint,
        .estimator_version = projection.estimator_version,
        .policy_version = projection.policy_version,
        .segments = projection.segments,
        .realistic_input_tokens = projection.realistic_input_tokens,
    };
}

struct QueuedRequestFixture {
    my_agent::Model model;
    my_agent::Request request;
    my_agent::ContextProjection projection;
    my_agent::ContextBudgetAssessment budget;
};

QueuedRequestFixture queued_request_fixture()
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 1024;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, "test-provider", "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(400, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(400, 'a')),
    };
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{2},
        .message_id = my_agent::MessageId{3},
        .text = std::string(400, 'q'),
    });

    my_agent::Request request{
        .messages = model.domain.thread.messages,
        .max_output_tokens = config.normal_output_budget(),
    };
    request.messages.push_back(message(
        my_agent::Role::User, 3, 2, std::string(400, 'q')));
    const my_agent::ContextProjection projection = my_agent::project_context(
        model.domain.request_policy.provider_name(),
        model.domain.request_policy.provider_model(),
        request,
        model.session.projection_ledger,
        config.estimator_version,
        config.projection_policy_version,
        config.tool_replay_policy,
        config.effective_limit());
    const my_agent::ContextBudgetAssessment budget =
        my_agent::assess_context_admission(
            projection,
            config,
            request.max_output_tokens);
    model.phase = my_agent::PreparingRequest{.preparation_id = my_agent::PreparationId{1}, .intent = my_agent::QueuedTurnRequest{my_agent::TurnId{2}, my_agent::MessageId{3}}};
    return QueuedRequestFixture{
        .model = std::move(model),
        .request = std::move(request),
        .projection = projection,
        .budget = budget,
    };
}

my_agent::Msg prepared_event(
    const QueuedRequestFixture& fixture,
    my_agent::PreparationId preparation_id)
{
    return my_agent::Msg{my_agent::RequestPrepared{
        .preparation_id = preparation_id,
        .snapshot = my_agent::PreparedRequestSnapshot{
            .request = fixture.request,
            .budget = fixture.budget,
            .receipt = receipt_for_test(fixture.projection),
        },
    }};
}

my_agent::PreparedRequestSnapshot prepared_snapshot(
    const my_agent::PrepareRequest& command)
{
    return my_agent::prepare_request_snapshot(
        command.request,
        {},
        std::nullopt,
                command.policy,
        command.ledger);
}

my_agent::Model manual_compaction_model()
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 4096;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, "test-provider", "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(400, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(400, 'a')),
    };
    return model;
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

}  // namespace

TEST(AgentLoopTest, SubmitFromIdleStartsOneQueuedStream)
{
    const my_agent::Step step = my_agent_test::prepare_request(
    my_agent::update(
    my_agent::Model{},
    my_agent::Msg{my_agent::Submit{.text = "hello"}}));

    const auto* command = std::get_if<my_agent::StartStream>(&step.cmd);
    ASSERT_NE(nullptr, command);
    EXPECT_EQ(my_agent::TurnId{1}, command->ref.turn_id);
    EXPECT_EQ(my_agent::StreamId{1}, command->ref.stream_id);
    ASSERT_TRUE(std::holds_alternative<my_agent::Streaming>(step.model.phase));
    EXPECT_EQ(2U, step.model.domain.thread.messages.size());
    EXPECT_EQ("hello", step.model.domain.thread.messages.front().text);
}

TEST(AgentLoopTest, PolicyUpdateReissuesPreparationAndRejectsBothStaleResults)
{
    const my_agent::Step initial = my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "hello"}});
    const auto* first_prepare = std::get_if<my_agent::PrepareRequest>(
        &initial.cmd);
    ASSERT_NE(nullptr, first_prepare);
    EXPECT_EQ(my_agent::PreparationId{1}, first_prepare->preparation_id);
    const my_agent::PreparedRequestSnapshot stale_snapshot =
        prepared_snapshot(*first_prepare);

    my_agent::ContextManagementConfig next_policy;
    next_policy.configured_limit = 4096;
    next_policy.safety_margin = 64;
    const my_agent::Step changed = my_agent::update(
        initial.model,
        my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{next_policy, "next-provider", "next-model"}}});
    const auto* second_prepare = std::get_if<my_agent::PrepareRequest>(
        &changed.cmd);
    ASSERT_NE(nullptr, second_prepare);
    EXPECT_EQ(my_agent::PreparationId{2}, second_prepare->preparation_id);
    EXPECT_NE(first_prepare->preparation_id, second_prepare->preparation_id);
    EXPECT_EQ("next-provider", second_prepare->policy.provider_name());
    EXPECT_EQ("next-model", second_prepare->policy.provider_model());
    EXPECT_EQ(4096U, second_prepare->policy.config().effective_limit());
    ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
        changed.model.phase));
    EXPECT_EQ(second_prepare->preparation_id,
        std::get<my_agent::PreparingRequest>(changed.model.phase)
            .preparation_id);

    const my_agent::Step stale_success = my_agent::update(
        changed.model,
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = first_prepare->preparation_id,
            .snapshot = stale_snapshot,
        }});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale_success.cmd));
    ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
        stale_success.model.phase));
    EXPECT_EQ(second_prepare->preparation_id,
        std::get<my_agent::PreparingRequest>(stale_success.model.phase)
            .preparation_id);

    const my_agent::Step stale_failure = my_agent::update(
        stale_success.model,
        my_agent::Msg{my_agent::RequestPreparationFailed{
            .preparation_id = first_prepare->preparation_id,
            .message = "stale preparation failed",
        }});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale_failure.cmd));
    ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
        stale_failure.model.phase));
    EXPECT_EQ(second_prepare->preparation_id,
        std::get<my_agent::PreparingRequest>(stale_failure.model.phase)
            .preparation_id);
    ASSERT_EQ(1U, stale_failure.model.session.queued_messages.size());

    const my_agent::PreparedRequestSnapshot current_snapshot =
        prepared_snapshot(*second_prepare);
    const my_agent::Step accepted = my_agent::update(
        stale_failure.model,
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = second_prepare->preparation_id,
            .snapshot = current_snapshot,
        }});
    const auto* stream = std::get_if<my_agent::StartStream>(&accepted.cmd);
    ASSERT_NE(nullptr, stream);
    EXPECT_EQ(next_policy.normal_output_budget(),
        stream->request.max_output_tokens);
    ASSERT_TRUE(std::holds_alternative<my_agent::Streaming>(
        accepted.model.phase));
    const my_agent::Streaming active =
        std::get<my_agent::Streaming>(accepted.model.phase);

    my_agent::ContextManagementConfig streaming_policy;
    streaming_policy.configured_limit = 16384;
    const my_agent::Step changed_while_streaming = my_agent::update(
        accepted.model,
        my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{streaming_policy, "third-provider", "third-model"}}});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        changed_while_streaming.cmd));
    ASSERT_TRUE(std::holds_alternative<my_agent::Streaming>(
        changed_while_streaming.model.phase));
    const my_agent::Streaming& still_active =
        std::get<my_agent::Streaming>(changed_while_streaming.model.phase);
    EXPECT_EQ(active.ref, still_active.ref);
    EXPECT_EQ(active.projection_receipt.fingerprint, still_active.projection_receipt.fingerprint);
    EXPECT_EQ("third-model",
        changed_while_streaming.model.domain.request_policy.provider_model());
}

TEST(AgentLoopTest, QueuedPreparationFailureKeepsTheHeadForExplicitResume)
{
    const my_agent::Step started = my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "keep me queued"}});
    const auto* prepare = std::get_if<my_agent::PrepareRequest>(&started.cmd);
    ASSERT_NE(nullptr, prepare);

    const my_agent::Step failed = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::RequestPreparationFailed{
            .preparation_id = prepare->preparation_id,
            .message = "system prompt unavailable",
        }});

    ASSERT_TRUE(std::holds_alternative<my_agent::Blocked>(failed.model.phase));
    EXPECT_EQ(my_agent::BlockReason::ExplicitResumeRequired,
        std::get<my_agent::Blocked>(failed.model.phase).reason);
    ASSERT_EQ(1U, failed.model.session.queued_messages.size());
    EXPECT_EQ("keep me queued", failed.model.session.queued_messages.front().text);
    EXPECT_TRUE(failed.model.domain.thread.messages.empty());
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));
}

TEST(AgentLoopTest, ContinuationAndRetryPreparationFailuresAnnotateThenDriveQueue)
{
    for (const bool retry : {false, true}) {
        SCOPED_TRACE(retry);
        my_agent::Model model;
        my_agent::Message assistant =
            message(my_agent::Role::Assistant, 2, 1, "partial response");
        if (!retry) {
            assistant.text.clear();
            assistant.tool_calls.push_back(my_agent::ToolCall{
                .id = "calculator-call",
                .name = "calculator",
                .args = nlohmann::json{{"expression", "6 * 7"}},
                .status = my_agent::ToolCall::Done{.output = "42"},
                .tool_exec_id = my_agent::ToolExecId{1},
            });
        }
        model.domain.thread.messages = {
            message(my_agent::Role::User, 1, 1, "current turn"),
            std::move(assistant),
        };
        model.phase = my_agent::PreparingRequest{.preparation_id = my_agent::PreparationId{1}, .intent = retry ? my_agent::RequestIntent{my_agent::RetryRequest{my_agent::TurnId{1}, my_agent::MessageId{2}}} : my_agent::RequestIntent{my_agent::ToolContinuationRequest{my_agent::TurnId{1}, my_agent::MessageId{2}}}};
        model.session.ids.next_preparation = 2;
        model.session.queued_messages.push_back(my_agent::QueuedMessage{
            .turn_id = my_agent::TurnId{2},
            .message_id = my_agent::MessageId{3},
            .text = "next queued turn",
        });

        const my_agent::Step failed = my_agent::update(
            std::move(model),
            my_agent::Msg{my_agent::RequestPreparationFailed{
                .preparation_id = my_agent::PreparationId{1},
                .message = "request preparation failed",
            }});

        const auto* next_prepare = std::get_if<my_agent::PrepareRequest>(
            &failed.cmd);
        ASSERT_NE(nullptr, next_prepare);
        EXPECT_EQ(my_agent::PreparationId{2}, next_prepare->preparation_id);
        ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
            failed.model.phase));
        const auto& next = std::get<my_agent::PreparingRequest>(
            failed.model.phase);
        EXPECT_TRUE(std::holds_alternative<my_agent::QueuedTurnRequest>(next.intent));
        EXPECT_EQ(my_agent::MessageId{3}, std::get<my_agent::QueuedTurnRequest>(next.intent).queued_message_id);
        ASSERT_EQ(2U, failed.model.domain.thread.messages.size());
        ASSERT_TRUE(failed.model.domain.thread.messages.back().error.has_value());
        EXPECT_EQ("request preparation failed",
            *failed.model.domain.thread.messages.back().error);
        ASSERT_EQ(1U, failed.model.session.queued_messages.size());
        EXPECT_EQ(my_agent::MessageId{3},
            failed.model.session.queued_messages.front().message_id);
    }
}

TEST(AgentLoopTest, IdleCompactionRequestStartsOneManualCompaction)
{
    const my_agent::Step started = my_agent::update(
        manual_compaction_model(),
        my_agent::Msg{my_agent::RequestCompaction{}});

    const auto* command = std::get_if<my_agent::StartCompaction>(
        &started.cmd);
    ASSERT_NE(nullptr, command);
    ASSERT_TRUE(std::holds_alternative<my_agent::Compacting>(
        started.model.phase));
    EXPECT_TRUE(started.model.session.queued_messages.empty());
    EXPECT_EQ(2U, started.model.domain.thread.messages.size());
}

TEST(AgentLoopTest, ActiveCompactionKeepsItsCapturedPolicyAfterPolicyUpdate)
{
    my_agent::ContextManagementConfig original_policy;
    original_policy.configured_limit = 4096;
    original_policy.safety_margin = 16;
    my_agent::Model model;
    model.domain.request_policy = my_agent::RequestPolicy{original_policy, "original-provider", "original-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(500, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(500, 'a')),
    };

    const my_agent::Step started = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::RequestCompaction{}});
    const auto* compaction = std::get_if<my_agent::StartCompaction>(&started.cmd);
    ASSERT_NE(nullptr, compaction);
    ASSERT_TRUE(std::holds_alternative<my_agent::Compacting>(
        started.model.phase));
    const my_agent::CompactionPlan captured =
        std::get<my_agent::Compacting>(started.model.phase).plan;
    EXPECT_EQ(original_policy.configured_limit, captured.policy.config().configured_limit);
    EXPECT_EQ("original-model", captured.policy.provider_model());

    const my_agent::Step queued = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::Submit{.text = "prepare after compaction"}});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(queued.cmd));
    ASSERT_EQ(1U, queued.model.session.queued_messages.size());

    my_agent::ContextManagementConfig next_policy = original_policy;
    next_policy.configured_limit = 8192;
    next_policy.safety_margin = 64;
    const my_agent::Step changed = my_agent::update(
        queued.model,
        my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{next_policy, "next-provider", "next-model"}}});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(changed.cmd));
    ASSERT_TRUE(std::holds_alternative<my_agent::Compacting>(
        changed.model.phase));
    const auto& active_plan =
        std::get<my_agent::Compacting>(changed.model.phase).plan;
    EXPECT_EQ(captured.compaction_id, active_plan.compaction_id);
    EXPECT_EQ(original_policy.configured_limit,
        active_plan.policy.config().configured_limit);
    EXPECT_EQ("original-model", active_plan.policy.provider_model());

    const my_agent::Step with_summary = my_agent::update(
        changed.model,
        my_agent::Msg{my_agent::CompactionDelta{
            .compaction_id = compaction->compaction_id,
            .text = valid_summary(),
        }});
    const my_agent::Step finished = my_agent::update(
        with_summary.model,
        my_agent::Msg{my_agent::CompactionFinished{
            .compaction_id = compaction->compaction_id,
        }});

    const auto* prepare = std::get_if<my_agent::PrepareRequest>(&finished.cmd);
    ASSERT_NE(nullptr, prepare);
    EXPECT_EQ("next-provider", prepare->policy.provider_name());
    EXPECT_EQ("next-model", prepare->policy.provider_model());
    EXPECT_EQ(next_policy.configured_limit,
        prepare->policy.config().configured_limit);
    EXPECT_TRUE(finished.model.domain.thread.accepted_compaction.has_value());
    ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
        finished.model.phase));
    EXPECT_EQ(my_agent::PreparationId{1}, prepare->preparation_id);
}

TEST(AgentLoopTest, CompactionRequestIsIgnoredWhileBusyOrQueued)
{
    my_agent::Model busy = manual_compaction_model();
    busy.phase = my_agent::Streaming{};
    const my_agent::Step busy_step = my_agent::update(
        busy, my_agent::Msg{my_agent::RequestCompaction{}});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(busy_step.cmd));
    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        busy_step.model.phase));

    my_agent::Model queued = manual_compaction_model();
    queued.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{2},
        .message_id = my_agent::MessageId{3},
        .text = "waiting",
    });
    const my_agent::Step queued_step = my_agent::update(
        queued, my_agent::Msg{my_agent::RequestCompaction{}});
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        queued_step.cmd));
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(
        queued_step.model.phase));
    EXPECT_EQ(1U, queued_step.model.session.queued_messages.size());
}

TEST(AgentLoopTest, ManualCompactionSuccessKeepsRawTranscriptAndAcceptsSummary)
{
    const my_agent::Step started = my_agent::update(
        manual_compaction_model(),
        my_agent::Msg{my_agent::RequestCompaction{}});
    const auto* command = std::get_if<my_agent::StartCompaction>(
        &started.cmd);
    ASSERT_NE(nullptr, command);

    const my_agent::Step with_summary = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionDelta{
            .compaction_id = command->compaction_id,
            .text = valid_summary(),
        }});
    const my_agent::Step finished = my_agent::update(
        with_summary.model,
        my_agent::Msg{my_agent::CompactionFinished{
            .compaction_id = command->compaction_id,
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(
        finished.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(finished.cmd));
    ASSERT_TRUE(finished.model.domain.thread.accepted_compaction.has_value());
    EXPECT_EQ(2U, finished.model.domain.thread.messages.size());
    EXPECT_EQ(std::string(400, 'u'), finished.model.domain.thread.messages[0].text);
    EXPECT_EQ(std::string(400, 'a'), finished.model.domain.thread.messages[1].text);
}

TEST(AgentLoopTest, ManualCompactionFailureAndCancelDoNotRetry)
{
    const auto start_manual = [] {
        return my_agent::update(
            manual_compaction_model(),
            my_agent::Msg{my_agent::RequestCompaction{}});
    };

    const my_agent::Step started = start_manual();
    const auto* command = std::get_if<my_agent::StartCompaction>(
        &started.cmd);
    ASSERT_NE(nullptr, command);
    const my_agent::Step failed = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionError{
            .compaction_id = command->compaction_id,
            .message = "provider failed",
        }});
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(failed.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));
    ASSERT_TRUE(failed.model.session.last_failed_compaction.has_value());

    const my_agent::Step cancelled_start = start_manual();
    const auto* cancelled_command = std::get_if<my_agent::StartCompaction>(
        &cancelled_start.cmd);
    ASSERT_NE(nullptr, cancelled_command);
    const my_agent::Step cancelled = my_agent::update(
        cancelled_start.model,
        my_agent::Msg{my_agent::CancelActive{}});
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(
        cancelled.model.phase));
    const auto* cancel = std::get_if<my_agent::CancelOperation>(
        &cancelled.cmd);
    ASSERT_NE(nullptr, cancel);
    EXPECT_EQ(cancelled_command->compaction_id, std::get<my_agent::CompactionId>(cancel->operation));
}

TEST(AgentLoopTest, StaleManualCompactionEventsDoNotChangeTheActiveOperation)
{
    const my_agent::Step started = my_agent::update(
        manual_compaction_model(),
        my_agent::Msg{my_agent::RequestCompaction{}});
    const auto* command = std::get_if<my_agent::StartCompaction>(
        &started.cmd);
    ASSERT_NE(nullptr, command);

    const my_agent::Step stale = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionFinished{
            .compaction_id = my_agent::CompactionId{
                command->compaction_id.value + 1},
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::Compacting>(
        stale.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale.cmd));
    EXPECT_FALSE(std::get<my_agent::Compacting>(stale.model.phase)
                     .pending_summary.size());
    EXPECT_FALSE(stale.model.domain.thread.accepted_compaction.has_value());
}

TEST(AgentLoopTest, CompactionUsageWithMismatchedIdentityIsIgnored)
{
    const my_agent::Step started = my_agent::update(
        manual_compaction_model(),
        my_agent::Msg{my_agent::RequestCompaction{}});
    const auto* command = std::get_if<my_agent::StartCompaction>(
        &started.cmd);
    ASSERT_NE(nullptr, command);

    const std::array<my_agent::OperationRef, 4> mismatched_operations{
        my_agent::CompactionId{},
        my_agent::CompactionId{command->compaction_id.value + 1},
        my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{command->compaction_id.value}},
        my_agent::ToolCallRef{.assistant_message_id = my_agent::MessageId{1}, .tool_exec_id = my_agent::ToolExecId{command->compaction_id.value}},
    };
    for (const my_agent::OperationRef& operation :
         mismatched_operations) {
        const my_agent::Step ignored = my_agent::update(
            started.model,
            my_agent::Msg{my_agent::RequestUsage{
                .operation = operation,
                .request_fingerprint = command->request_fingerprint,
                .input_tokens = 123,
                .output_tokens = 7,
            }});

        const auto* active = std::get_if<my_agent::Compacting>(
            &ignored.model.phase);
        ASSERT_NE(nullptr, active);
        EXPECT_EQ(command->compaction_id, active->plan.compaction_id);
        EXPECT_FALSE(active->usage_accepted);
        EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(ignored.cmd));
        EXPECT_EQ(0U, ignored.model.session.usage.request_count);
        EXPECT_EQ(0U, ignored.model.session.usage.compaction_session.input_tokens);
        EXPECT_EQ(0U, ignored.model.session.usage.compaction_session.output_tokens);
    }

}

TEST(AgentLoopTest, ManualCompactionWithoutCompleteHistoryReportsTypedFailure)
{
    my_agent::Model model = manual_compaction_model();
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, "incomplete turn"),
    };

    const my_agent::Step step = my_agent::update(
        model, my_agent::Msg{my_agent::RequestCompaction{}});

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(step.model.phase));
    EXPECT_EQ(
        my_agent::ContextBlockDetail::NoCompleteHistory,
        step.model.session.last_failed_compaction->detail);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(step.cmd));
}

TEST(AgentLoopTest, ManualCompactionWithAnOversizedSourceReportsTypedFailure)
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 400;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, {}, "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(4000, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(4000, 'a')),
    };

    const my_agent::Step step = my_agent::update(
        model, my_agent::Msg{my_agent::RequestCompaction{}});

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(step.model.phase));
    EXPECT_EQ(
        my_agent::ContextBlockDetail::CompactionSourceTooLarge,
        step.model.session.last_failed_compaction->detail);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(step.cmd));
}

TEST(AgentLoopTest, StaleStreamDeltaDoesNotChangeTheActiveModel)
{
    my_agent::Step started = my_agent_test::prepare_request(
        my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "hello"}}));
    const auto* command = std::get_if<my_agent::StartStream>(&started.cmd);
    ASSERT_NE(nullptr, command);

    const my_agent::Step stale = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::StreamTextDelta{
            .ref = my_agent::StreamRef{.turn_id = command->ref.turn_id, .stream_id = my_agent::StreamId{command->ref.stream_id.value + 1}},
            .text = "ignored",
        }});

    EXPECT_EQ(stale.model.domain.thread.messages.back().text, "");
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale.cmd));
}

TEST(AgentLoopTest, FinishedStreamReturnsToIdleAndKeepsTranscript)
{
    my_agent::Step started = my_agent_test::prepare_request(
    my_agent::update(my_agent::Model{},my_agent::Msg{my_agent::Submit{.text = "hello"}}));
    const auto* command = std::get_if<my_agent::StartStream>(&started.cmd);
    ASSERT_NE(nullptr, command);

    my_agent::Step with_text = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::StreamTextDelta{
            .ref = my_agent::StreamRef{.turn_id = command->ref.turn_id, .stream_id = command->ref.stream_id},
            .text = "world",
        }});
    my_agent::Step finished = my_agent::update(
        with_text.model,
        my_agent::Msg{my_agent::StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = command->ref.turn_id, .stream_id = command->ref.stream_id},
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(finished.model.phase));
    EXPECT_EQ("world", finished.model.domain.thread.messages.back().text);
}

TEST(AgentLoopTest, FinishedIdleTurnDoesNotStartProjectionRefresh)
{
    my_agent::Model model;
    model.domain.request_policy = my_agent::RequestPolicy{};
    model.domain.thread.messages = {
        my_agent::Message{
            .role = my_agent::Role::User,
            .text = "hello",
            .id = my_agent::MessageId{1},
            .turn_id = my_agent::TurnId{1},
        },
        my_agent::Message{
            .role = my_agent::Role::Assistant,
            .text = "world",
            .id = my_agent::MessageId{2},
            .turn_id = my_agent::TurnId{1},
        },
    };
    model.phase = my_agent::Streaming{.ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{1}}, .target_message_id = my_agent::MessageId{2}};

    const my_agent::Step finished = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{1}},
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(finished.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(finished.cmd));
}

TEST(AgentLoopTest, QueuedPreparedRequestStartsOneCompaction)
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 1024;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, "test-provider", "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(400, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(400, 'a')),
    };
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{2},
        .message_id = my_agent::MessageId{3},
        .text = std::string(400, 'q'),
    });

    my_agent::Request request{
        .messages = model.domain.thread.messages,
        .max_output_tokens = config.normal_output_budget(),
    };
    request.messages.push_back(message(
        my_agent::Role::User, 3, 2, std::string(400, 'q')));
    const my_agent::ContextProjection projection = my_agent::project_context(
        model.domain.request_policy.provider_name(),
        model.domain.request_policy.provider_model(),
        request,
        model.session.projection_ledger,
        config.estimator_version,
        config.projection_policy_version,
        config.tool_replay_policy,
        config.effective_limit());
    const my_agent::ContextBudgetAssessment budget =
        my_agent::assess_context_admission(
            projection,
            config,
            request.max_output_tokens);
    ASSERT_TRUE(budget.triggered);
    ASSERT_TRUE(budget.admitted);

    model.phase = my_agent::PreparingRequest{.preparation_id = my_agent::PreparationId{1}, .intent = my_agent::QueuedTurnRequest{my_agent::TurnId{2}, my_agent::MessageId{3}}};
    const my_agent::Step prepared = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = my_agent::PreparationId{1},
            .snapshot = my_agent::PreparedRequestSnapshot{
                .request = request,
                .budget = budget,
                .receipt = receipt_for_test(projection),
            },
        }});

    const auto* command = std::get_if<my_agent::StartCompaction>(
        &prepared.cmd);
    ASSERT_NE(nullptr, command);
    EXPECT_EQ(config.compaction_output_budget(),
              command->request.max_output_tokens);
    EXPECT_TRUE(std::holds_alternative<my_agent::Compacting>(
        prepared.model.phase));
}

TEST(AgentLoopTest, ContextLimitOnEmptyQueuedTurnBlocksWithoutRecovery)
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 1024;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, "test-provider", "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(400, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(400, 'a')),
        message(my_agent::Role::User, 3, 2, std::string(400, 'q')),
        message(my_agent::Role::Assistant, 4, 2, {}),
    };
    my_agent::Request request{
        .messages = {
            model.domain.thread.messages[0],
            model.domain.thread.messages[1],
            model.domain.thread.messages[2],
        },
        .max_output_tokens = config.normal_output_budget(),
    };
    const my_agent::ContextProjection projection = my_agent::project_context(
        model.domain.request_policy.provider_name(),
        model.domain.request_policy.provider_model(),
        request,
        model.session.projection_ledger,
        config.estimator_version,
        config.projection_policy_version,
        config.tool_replay_policy,
        config.effective_limit());
    model.phase = my_agent::Streaming{
        .ref = my_agent::StreamRef{
            .turn_id = my_agent::TurnId{2},
            .stream_id = my_agent::StreamId{1},
        },
        .target_message_id = my_agent::MessageId{4},
        .projection_receipt = receipt_for_test(projection),
        .rollback_user_message = my_agent::MessageId{3},
    };

    const my_agent::Step failed = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::StreamError{
            .ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{2}, .stream_id = my_agent::StreamId{1}},
            .message = "context window exceeded",
            .provider_error = my_agent::ProviderError{
                .kind = my_agent::ProviderErrorKind::ContextLimit,
            },
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::Blocked>(
        failed.model.phase));
    const auto& blocked = std::get<my_agent::Blocked>(failed.model.phase);
    EXPECT_EQ(my_agent::BlockReason::ContextLimit, blocked.reason);
    EXPECT_EQ(
        my_agent::ContextBlockDetail::RequestTooLarge,
        blocked.context_detail);
    EXPECT_EQ(2U, failed.model.domain.thread.messages.size());
    ASSERT_EQ(1U, failed.model.session.queued_messages.size());
    EXPECT_EQ(my_agent::MessageId{3},
              failed.model.session.queued_messages.front().message_id);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));
}

TEST(AgentLoopTest, ContextLimitOnToolContinuationPreservesSideEffects)
{
    my_agent::Model model;
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, "calculate"),
        my_agent::Message{
            .role = my_agent::Role::Assistant,
            .tool_calls = {
                my_agent::ToolCall{
                    .id = "calculator-call",
                    .name = "calculator",
                    .args = nlohmann::json{{"expression", "6 * 7"}},
                    .status = my_agent::ToolCall::Done{.output = "42"},
                    .tool_exec_id = my_agent::ToolExecId{1},
                },
            },
            .id = my_agent::MessageId{2},
            .turn_id = my_agent::TurnId{1},
        },
        message(my_agent::Role::Assistant, 3, 1, {}),
    };
    const my_agent::Request request{
        .messages = {
            model.domain.thread.messages[0],
            model.domain.thread.messages[1],
        },
    };
    model.phase = my_agent::Streaming{.ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{1}}, .target_message_id = my_agent::MessageId{3}, .projection_receipt = my_agent::ContextProjectionReceipt{
            .fingerprint = my_agent::fingerprint_request(request),
            .segments = {},
        }};

    const my_agent::Step failed = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::StreamError{
            .ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{1}},
            .message = "context window exceeded",
            .provider_error = my_agent::ProviderError{
                .kind = my_agent::ProviderErrorKind::ContextLimit,
            },
        }});

    ASSERT_TRUE(std::holds_alternative<my_agent::Blocked>(
        failed.model.phase));
    EXPECT_EQ(
        my_agent::ContextBlockDetail::RequestTooLarge,
        std::get<my_agent::Blocked>(failed.model.phase).context_detail);
    ASSERT_EQ(3U, failed.model.domain.thread.messages.size());
    ASSERT_EQ(1U, failed.model.domain.thread.messages[1].tool_calls.size());
    EXPECT_EQ("42", failed.model.domain.thread.messages[1].tool_calls[0].output());
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));
}

TEST(AgentLoopTest, ContextLimitAfterVisibleOutputKeepsTheCurrentTurn)
{
    my_agent::Step started = my_agent_test::prepare_request(
    my_agent::update(
    my_agent::Model{},
    my_agent::Msg{my_agent::Submit{.text = "hello"}}));
    const auto* command = std::get_if<my_agent::StartStream>(&started.cmd);
    ASSERT_NE(nullptr, command);

    my_agent::Step with_text = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::StreamTextDelta{
            .ref = my_agent::StreamRef{.turn_id = command->ref.turn_id, .stream_id = command->ref.stream_id},
            .text = "partial answer",
        }});
    const my_agent::Step failed = my_agent::update(
        std::move(with_text.model),
        my_agent::Msg{my_agent::StreamError{
            .ref = my_agent::StreamRef{.turn_id = command->ref.turn_id, .stream_id = command->ref.stream_id},
            .message = "context window exceeded",
            .provider_error = my_agent::ProviderError{
                .kind = my_agent::ProviderErrorKind::ContextLimit,
            },
        }});

    ASSERT_TRUE(std::holds_alternative<my_agent::Blocked>(
        failed.model.phase));
    EXPECT_EQ(
        my_agent::ContextBlockDetail::RequestTooLarge,
        std::get<my_agent::Blocked>(failed.model.phase).context_detail);
    ASSERT_EQ(2U, failed.model.domain.thread.messages.size());
    EXPECT_EQ("partial answer", failed.model.domain.thread.messages.back().text);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));
}

TEST(AgentLoopTest, SourceTooLargeBlocksTheQueuedRequestWithTypedDetail)
{
    my_agent::ContextManagementConfig config;
    config.configured_limit = 400;
    config.safety_margin = 16;

    my_agent::Model model;
    model.domain.request_policy =
        my_agent::RequestPolicy{config, {}, "test-model"};
    model.domain.thread.messages = {
        message(my_agent::Role::User, 1, 1, std::string(4000, 'u')),
        message(my_agent::Role::Assistant, 2, 1, std::string(4000, 'a')),
    };
    model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{2},
        .message_id = my_agent::MessageId{3},
        .text = "next",
    });
    my_agent::Request request{
        .messages = model.domain.thread.messages,
        .max_output_tokens = config.normal_output_budget(),
    };
    request.messages.push_back(message(
        my_agent::Role::User, 3, 2, "next"));
    const my_agent::ContextProjection projection = my_agent::project_context(
        {},
        model.domain.request_policy.provider_model(),
        request,
        model.session.projection_ledger,
        config.estimator_version,
        config.projection_policy_version,
        config.tool_replay_policy,
        config.effective_limit());
    const my_agent::ContextBudgetAssessment budget =
        my_agent::assess_context_admission(
            projection,
            config,
            request.max_output_tokens);
    ASSERT_FALSE(budget.admitted);

    model.phase = my_agent::PreparingRequest{.preparation_id = my_agent::PreparationId{1}, .intent = my_agent::QueuedTurnRequest{my_agent::TurnId{2}, my_agent::MessageId{3}}};
    const my_agent::Step blocked = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = my_agent::PreparationId{1},
            .snapshot = my_agent::PreparedRequestSnapshot{
                .request = request,
                .budget = budget,
                .receipt = receipt_for_test(projection),
            },
        }});

    ASSERT_TRUE(std::holds_alternative<my_agent::Blocked>(
        blocked.model.phase));
    EXPECT_EQ(
        my_agent::ContextBlockDetail::CompactionSourceTooLarge,
        std::get<my_agent::Blocked>(blocked.model.phase).context_detail);
    ASSERT_EQ(1U, blocked.model.session.queued_messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(blocked.cmd));
}

TEST(AgentLoopTest, SuccessfulCompactionRepreparesTheSameQueueHead)
{
    QueuedRequestFixture fixture = queued_request_fixture();
    const my_agent::Step started = my_agent::update(
        std::move(fixture.model),
        prepared_event(fixture, my_agent::PreparationId{1}));
    const auto* start = std::get_if<my_agent::StartCompaction>(&started.cmd);
    ASSERT_NE(nullptr, start);

    const my_agent::Step with_summary = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionDelta{
            .compaction_id = start->compaction_id,
            .text = valid_summary(),
        }});
    const my_agent::Step finished = my_agent::update(
        with_summary.model,
        my_agent::Msg{my_agent::CompactionFinished{
            .compaction_id = start->compaction_id,
        }});

    const auto* prepare = std::get_if<my_agent::PrepareRequest>(
        &finished.cmd);
    ASSERT_NE(nullptr, prepare);
    const auto& preparing = std::get<my_agent::PreparingRequest>(finished.model.phase);
    EXPECT_EQ(my_agent::TurnId{2},
        std::get<my_agent::QueuedTurnRequest>(preparing.intent).turn_id);
    EXPECT_EQ(1U, finished.model.session.queued_messages.size());
    ASSERT_TRUE(finished.model.domain.thread.accepted_compaction.has_value());
    EXPECT_EQ(2U, finished.model.domain.thread.messages.size());
    for (const my_agent::Message& item : finished.model.domain.thread.messages) {
        EXPECT_EQ(std::string::npos, item.text.find("Earlier conversation"));
    }
}

TEST(AgentLoopTest, FailedCompactionRepreparesThenStreamsWithoutRetry)
{
    QueuedRequestFixture fixture = queued_request_fixture();
    const my_agent::Step started = my_agent::update(
        std::move(fixture.model),
        prepared_event(fixture, my_agent::PreparationId{1}));
    const auto* start = std::get_if<my_agent::StartCompaction>(&started.cmd);
    ASSERT_NE(nullptr, start);

    const my_agent::Step failed = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionError{
            .compaction_id = start->compaction_id,
            .message = "provider failed",
        }});
    const auto* prepare = std::get_if<my_agent::PrepareRequest>(&failed.cmd);
    ASSERT_NE(nullptr, prepare);
    ASSERT_TRUE(failed.model.session.last_failed_compaction.has_value());

    const QueuedRequestFixture retry_fixture{
        .model = failed.model,
        .request = fixture.request,
        .projection = fixture.projection,
        .budget = fixture.budget,
    };
    const my_agent::Step reprepared = my_agent::update(
        failed.model,
        prepared_event(retry_fixture, prepare->preparation_id));
    EXPECT_TRUE(std::holds_alternative<my_agent::StartStream>(
        reprepared.cmd));
    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        reprepared.model.phase));
}

TEST(AgentLoopTest, CancelledCompactionRepreparesThenStreamsWithoutRetry)
{
    QueuedRequestFixture fixture = queued_request_fixture();
    const my_agent::Step started = my_agent::update(
        std::move(fixture.model),
        prepared_event(fixture, my_agent::PreparationId{1}));
    const auto* start = std::get_if<my_agent::StartCompaction>(&started.cmd);
    ASSERT_NE(nullptr, start);

    const my_agent::Step cancelled = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CancelActive{}});
    const auto* prepare = std::get_if<my_agent::PrepareRequest>(
        &cancelled.cmd);
    ASSERT_NE(nullptr, prepare);
    ASSERT_TRUE(cancelled.model.session.last_failed_compaction.has_value());

    const QueuedRequestFixture retry_fixture{
        .model = cancelled.model,
        .request = fixture.request,
        .projection = fixture.projection,
        .budget = fixture.budget,
    };
    const my_agent::Step reprepared = my_agent::update(
        cancelled.model,
        prepared_event(retry_fixture, prepare->preparation_id));
    EXPECT_TRUE(std::holds_alternative<my_agent::StartStream>(
        reprepared.cmd));
    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        reprepared.model.phase));
}

TEST(AgentLoopTest, NewCompletedTurnAllowsCompactionAfterAFailedSource)
{
    QueuedRequestFixture fixture = queued_request_fixture();
    const my_agent::Step started = my_agent::update(
        std::move(fixture.model),
        prepared_event(fixture, my_agent::PreparationId{1}));
    const auto* start = std::get_if<my_agent::StartCompaction>(&started.cmd);
    ASSERT_NE(nullptr, start);

    my_agent::Step failed = my_agent::update(
        started.model,
        my_agent::Msg{my_agent::CompactionError{
            .compaction_id = start->compaction_id,
            .message = "provider failed",
        }});
    ASSERT_TRUE(std::holds_alternative<my_agent::PreparingRequest>(
        failed.model.phase));
    const auto* preparing = std::get_if<my_agent::PreparingRequest>(
        &failed.model.phase);
    ASSERT_NE(nullptr, preparing);
    const my_agent::TurnId turn_id = std::get<my_agent::QueuedTurnRequest>(preparing->intent).turn_id;
    const my_agent::PreparationId preparation_id = preparing->preparation_id;

    failed.model.domain.thread.messages.push_back(
        message(my_agent::Role::User, 5, 3, "new completed question"));
    failed.model.domain.thread.messages.push_back(
        message(my_agent::Role::Assistant, 6, 3, "new completed answer"));
    const my_agent::ContextManagementConfig& config =
        failed.model.domain.request_policy.config();
    my_agent::Request request{
        .messages = failed.model.domain.thread.messages,
        .max_output_tokens = config.normal_output_budget(),
    };
    request.messages.push_back(message(
        my_agent::Role::User,
        failed.model.session.queued_messages.front().message_id.value,
        failed.model.session.queued_messages.front().turn_id.value,
        failed.model.session.queued_messages.front().text));
    const my_agent::ContextProjection projection = my_agent::project_context(
        failed.model.domain.request_policy.provider_name(),
        failed.model.domain.request_policy.provider_model(),
        request,
        failed.model.session.projection_ledger,
        config.estimator_version,
        config.projection_policy_version,
        config.tool_replay_policy,
        config.effective_limit());
    const my_agent::ContextBudgetAssessment budget =
        my_agent::assess_context_admission(
            projection,
            config,
            request.max_output_tokens);
    ASSERT_TRUE(budget.triggered);

    const my_agent::Step reprepared = my_agent::update(
        std::move(failed.model),
        my_agent::Msg{my_agent::RequestPrepared{
            .preparation_id = preparation_id,
            .snapshot = my_agent::PreparedRequestSnapshot{
                .request = request,
                .budget = budget,
                .receipt = receipt_for_test(projection),
            },
        }});

    EXPECT_TRUE(std::holds_alternative<my_agent::StartCompaction>(
        reprepared.cmd));
    EXPECT_TRUE(std::holds_alternative<my_agent::Compacting>(
        reprepared.model.phase));
}
