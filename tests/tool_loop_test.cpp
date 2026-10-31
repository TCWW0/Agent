#include "my_agent/runtime/agent.hpp"
#include "my_agent/tool/session_services.hpp"
#include "request_preparation_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

my_agent::Msg resolved_core_tool_call(
    std::string id,
    std::string name,
    nlohmann::json args)
{
    const bool is_read = name == "read";
    return my_agent::Msg{my_agent::ResolvedStreamToolCall{
        std::move(id),
        std::move(name),
        std::move(args),
        my_agent::tool::ToolBinding{
            .ref = my_agent::tool::ToolRef{.value = is_read ? 2U : 1U},
            .effects = is_read
                ? my_agent::tool::EffectSet{my_agent::tool::Effect::ReadFs}
                : my_agent::tool::EffectSet{},
        },
    }};
}

my_agent::ToolExecOutput execution_output(
    const my_agent::RunTool& command,
    my_agent::tool::ExecResult result)
{
    return my_agent::ToolExecOutput{
        .turn_id = command.ref.turn_id,
        .tool_exec_id = command.tool_exec_id,
        .id = command.id,
        .ref = command.ref,
        .result = std::move(result),
    };
}

my_agent::PermissionApprove approve_pending(const my_agent::Model& model)
{
    const auto& pending = model.pending_permission.value();
    return my_agent::PermissionApprove{
        .turn_id = pending.turn_id,
        .tool_exec_id = pending.tool_exec_id,
        .id = pending.id,
    };
}

my_agent::PermissionReject reject_pending(
    const my_agent::Model& model,
    std::optional<std::string> feedback = {})
{
    const auto& pending = model.pending_permission.value();
    return my_agent::PermissionReject{
        .turn_id = pending.turn_id,
        .tool_exec_id = pending.tool_exec_id,
        .id = pending.id,
        .feedback = std::move(feedback),
    };
}

}  // namespace

TEST(ToolLoopTest, ExecutesCalculatorToolAndContinuesToFinalAnswer)
{
    std::vector<my_agent::Request> received_requests;
    std::size_t provider_turn = 0;

    my_agent::StreamEffect fake_stream =
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            received_requests.push_back(std::move(request));

            if (provider_turn++ == 0) {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamToolCall{
                        .id = "calculator-call-1",
                        .name = "calculator",
                        .args = nlohmann::json{
                            {"operation", "multiply"},
                            {"left", 6},
                            {"right", 7},
                        },
                    },
                });
            } else {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamTextDelta{
                        .text = "The result is 42.",
                    },
                });
            }

            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        };

    my_agent::HeadlessRunner runner{
        std::move(fake_stream), headless_session_runtime()};

    const my_agent::Model& model = runner.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "Calculate 6 * 7"},
    });

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(model.phase));

    ASSERT_EQ(std::size_t{2}, received_requests.size());

    const my_agent::Request& initial_request = received_requests[0];
    constexpr std::array<std::string_view, 6> expected_names{
        "calculator", "read", "remember", "forget", "skill", "search_docs"};
    ASSERT_EQ(expected_names.size(), initial_request.tools.size());
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        EXPECT_EQ(expected_names[i], initial_request.tools[i].name);
    }
    const auto calculator = std::ranges::find_if(
        initial_request.tools,
        [](const my_agent::ToolSpec& tool) {
            return tool.name == "calculator";
        }
    );
    ASSERT_NE(initial_request.tools.end(), calculator);

    const my_agent::Request& continuation_request = received_requests[1];
    ASSERT_EQ(std::size_t{2}, continuation_request.messages.size());

    const my_agent::Message& tool_message =
        continuation_request.messages[1];
    EXPECT_EQ(my_agent::Role::Assistant, tool_message.role);
    ASSERT_EQ(std::size_t{1}, tool_message.tool_calls.size());
    EXPECT_EQ("calculator", tool_message.tool_calls[0].name);
    EXPECT_EQ("42", tool_message.tool_calls[0].output());

    const my_agent::Thread& thread = model.thread;
    ASSERT_EQ(std::size_t{3}, thread.messages.size());
    EXPECT_EQ(my_agent::Role::User, thread.messages[0].role);
    EXPECT_EQ("Calculate 6 * 7", thread.messages[0].text);

    EXPECT_EQ(my_agent::Role::Assistant, thread.messages[1].role);
    ASSERT_EQ(std::size_t{1}, thread.messages[1].tool_calls.size());
    EXPECT_EQ("42", thread.messages[1].tool_calls[0].output());

    EXPECT_EQ(my_agent::Role::Assistant, thread.messages[2].role);
    EXPECT_EQ("The result is 42.", thread.messages[2].text);
}

TEST(ToolLoopTest, ApprovalRequiresTheExactAssistantAndExecutionReference)
{
    my_agent::Model model;
    model.domain.thread.messages.push_back(my_agent::Message{
        .role = my_agent::Role::Assistant,
        .text = {},
        .error = {},
        .tool_calls = {my_agent::ToolCall{
            .id = "provider-call-id",
            .name = "read",
            .args = nlohmann::json{{"path", "CMakeLists.txt"}},
            .status = my_agent::ToolCall::Pending{
                .binding = my_agent::tool::ToolBinding{
                    .ref = my_agent::tool::ToolRef{2},
                    .effects = {my_agent::tool::Effect::ReadFs},
                },
            },
            .tool_exec_id = my_agent::ToolExecId{9},
        }},
        .id = my_agent::MessageId{40},
        .turn_id = my_agent::TurnId{7},
    });
    const my_agent::ToolCallRef call{
        .assistant_message_id = my_agent::MessageId{40},
        .tool_exec_id = my_agent::ToolExecId{9},
    };
    model.phase = my_agent::AwaitingPermission{.call = call};

    const my_agent::Step wrong_message = my_agent::update(
        model,
        my_agent::Msg{my_agent::PermissionApprove{.call = {
            .assistant_message_id = my_agent::MessageId{41},
            .tool_exec_id = my_agent::ToolExecId{9},
        }}});
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        wrong_message.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(wrong_message.cmd));
    EXPECT_EQ(call, std::get<my_agent::AwaitingPermission>(
        wrong_message.model.phase).call);
    const my_agent::Step rejected_wrong_message = my_agent::update(
        model,
        my_agent::Msg{my_agent::PermissionReject{.call = {
            .assistant_message_id = my_agent::MessageId{41},
            .tool_exec_id = my_agent::ToolExecId{9},
        }}});
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        rejected_wrong_message.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        rejected_wrong_message.cmd));
    EXPECT_EQ(call, std::get<my_agent::AwaitingPermission>(
        rejected_wrong_message.model.phase).call);
    const my_agent::ToolCall* pending_after_wrong_message =
        my_agent::resolve_tool_call(
            rejected_wrong_message.model.domain.thread, call);
    ASSERT_NE(nullptr, pending_after_wrong_message);
    EXPECT_TRUE(pending_after_wrong_message->is_pending());

    const my_agent::Step wrong_execution = my_agent::update(
        model,
        my_agent::Msg{my_agent::PermissionApprove{.call = {
            .assistant_message_id = my_agent::MessageId{40},
            .tool_exec_id = my_agent::ToolExecId{10},
        }}});
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        wrong_execution.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(wrong_execution.cmd));
    EXPECT_EQ(call, std::get<my_agent::AwaitingPermission>(
        wrong_execution.model.phase).call);
    const my_agent::Step rejected_wrong_execution = my_agent::update(
        model,
        my_agent::Msg{my_agent::PermissionReject{.call = {
            .assistant_message_id = my_agent::MessageId{40},
            .tool_exec_id = my_agent::ToolExecId{10},
        }}});
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        rejected_wrong_execution.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        rejected_wrong_execution.cmd));
    EXPECT_EQ(call, std::get<my_agent::AwaitingPermission>(
        rejected_wrong_execution.model.phase).call);

    const my_agent::Step approved = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::PermissionApprove{.call = call}});
    ASSERT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        approved.model.phase));
    EXPECT_EQ(call, std::get<my_agent::ExecutingTool>(approved.model.phase).call);
    const auto* run_tool = std::get_if<my_agent::RunTool>(&approved.cmd);
    ASSERT_NE(nullptr, run_tool);
    EXPECT_EQ(call, run_tool->call);
    EXPECT_EQ(my_agent::tool::ToolRef{2}, run_tool->ref);
    const nlohmann::json expected_args{{"path", "CMakeLists.txt"}};
    EXPECT_EQ(expected_args, run_tool->args);
}

TEST(ToolLoopTest, StalePermissionDecisionsCannotAffectTheNextCall)
{
    my_agent::Model model;
    model.domain.profile = my_agent::Profile::Minimal;
    my_agent::Step started = my_agent_test::prepare_request(my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::Submit{.text = "read two files"}}));
    started = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            "same-provider-id-1", "read",
            nlohmann::json{{"path", "first.txt"}}));
    started = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            "same-provider-id-2", "read",
            nlohmann::json{{"path", "second.txt"}}));
    const my_agent::Step awaiting_first = my_agent::update(
        std::move(started.model), my_agent::Msg{my_agent::StreamFinished{}});
    const auto first_ref = std::get<my_agent::AwaitingPermission>(
        awaiting_first.model.phase).call;

    const my_agent::Step rejected_first = my_agent::update(
        awaiting_first.model,
        my_agent::Msg{my_agent::PermissionReject{.call = first_ref}});
    ASSERT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        rejected_first.model.phase));
    const auto second_ref = std::get<my_agent::AwaitingPermission>(
        rejected_first.model.phase).call;
    EXPECT_NE(first_ref, second_ref);

    const my_agent::Step stale_approval = my_agent::update(
        rejected_first.model,
        my_agent::Msg{my_agent::PermissionApprove{.call = first_ref}});
    const my_agent::Step stale_rejection = my_agent::update(
        rejected_first.model,
        my_agent::Msg{my_agent::PermissionReject{.call = first_ref}});
    for (const my_agent::Step* stale : {&stale_approval, &stale_rejection}) {
        ASSERT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
            stale->model.phase));
        EXPECT_EQ(second_ref,
                  std::get<my_agent::AwaitingPermission>(stale->model.phase).call);
        EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale->cmd));
    }
    const my_agent::ToolCall* second_call = my_agent::resolve_tool_call(
        rejected_first.model.domain.thread, second_ref);
    ASSERT_NE(nullptr, second_call);
    EXPECT_TRUE(second_call->is_pending());
}

TEST(ToolLoopTest, ReusedProviderIdsRemainScopedToTheirAssistantMessage)
{
    my_agent::Model model;
    const auto call = [](std::uint64_t exec_id) {
        return my_agent::ToolCall{
            .id = "provider-reused-id",
            .name = "read",
            .args = nlohmann::json{{"path", "same.txt"}},
            .status = my_agent::ToolCall::Pending{
                .binding = my_agent::tool::ToolBinding{
                    .ref = my_agent::tool::ToolRef{2},
                    .effects = {my_agent::tool::Effect::ReadFs},
                },
            },
            .tool_exec_id = my_agent::ToolExecId{exec_id},
        };
    };
    model.domain.thread.messages = {
        my_agent::Message{
            .role = my_agent::Role::Assistant,
            .tool_calls = {call(1)},
            .id = my_agent::MessageId{40},
            .turn_id = my_agent::TurnId{7},
        },
        my_agent::Message{
            .role = my_agent::Role::Assistant,
            .tool_calls = {call(2)},
            .id = my_agent::MessageId{41},
            .turn_id = my_agent::TurnId{8},
        },
    };
    const my_agent::ToolCallRef first{
        .assistant_message_id = my_agent::MessageId{40},
        .tool_exec_id = my_agent::ToolExecId{1},
    };
    const my_agent::ToolCallRef second{
        .assistant_message_id = my_agent::MessageId{41},
        .tool_exec_id = my_agent::ToolExecId{2},
    };
    model.phase = my_agent::AwaitingPermission{.call = second};

    const my_agent::Step stale = my_agent::update(
        model, my_agent::Msg{my_agent::PermissionApprove{.call = first}});
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        stale.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(stale.cmd));

    const my_agent::Step approved = my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::PermissionApprove{.call = second}});
    const auto* run_tool = std::get_if<my_agent::RunTool>(&approved.cmd);
    ASSERT_NE(nullptr, run_tool);
    EXPECT_EQ(second, run_tool->call);
    const my_agent::ToolCall* first_call = my_agent::resolve_tool_call(
        approved.model.domain.thread, first);
    ASSERT_NE(nullptr, first_call);
    EXPECT_EQ(my_agent::ToolExecId{1}, first_call->tool_exec_id);
    EXPECT_TRUE(first_call->is_pending());
}

TEST(ToolLoopTest, CancellingAwaitingPermissionSettlesSiblingsWithoutHostCancel)
{
    my_agent::Model model;
    model.domain.profile = my_agent::Profile::Minimal;
    my_agent::Step started = my_agent_test::prepare_request(my_agent::update(
        std::move(model),
        my_agent::Msg{my_agent::Submit{.text = "read two files"}}));
    started = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            "cancel-call-1", "read", nlohmann::json{{"path", "first.txt"}}));
    started = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            "cancel-call-2", "read", nlohmann::json{{"path", "second.txt"}}));
    const my_agent::Step awaiting = my_agent::update(
        std::move(started.model), my_agent::Msg{my_agent::StreamFinished{}});
    ASSERT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        awaiting.model.phase));

    const my_agent::Step cancelled = my_agent::update(
        awaiting.model, my_agent::Msg{my_agent::CancelActive{}});
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(cancelled.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(cancelled.cmd));
    const my_agent::Message& assistant = cancelled.model.domain.thread.messages.back();
    ASSERT_EQ(std::size_t{2}, assistant.tool_calls.size());
    for (const my_agent::ToolCall& call : assistant.tool_calls) {
        EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Cancelled>(
            call.status));
    }

    my_agent::Model queued_model = awaiting.model;
    queued_model.session.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{2},
        .message_id = my_agent::MessageId{3},
        .text = "next turn",
    });
    const my_agent::Step queued_cancel = my_agent::update(
        std::move(queued_model), my_agent::Msg{my_agent::CancelActive{}});
    const auto* prepare = std::get_if<my_agent::PrepareRequest>(
        &queued_cancel.cmd);
    ASSERT_NE(nullptr, prepare);
    EXPECT_FALSE(prepare->supersedes.has_value());
    EXPECT_FALSE(std::holds_alternative<my_agent::CancelOperation>(
        queued_cancel.cmd));
}

TEST(ToolLoopTest, CancellingInvalidPermissionReferenceStillExitsThePhase)
{
    my_agent::Model model;
    model.domain.thread.messages.push_back(my_agent::Message{
        .role = my_agent::Role::Assistant,
        .tool_calls = {
            my_agent::ToolCall{
                .id = "first",
                .name = "read",
                .args = nlohmann::json::object(),
                .status = my_agent::ToolCall::Pending{
                    .binding = my_agent::tool::ToolBinding{
                        .ref = my_agent::tool::ToolRef{1},
                        .effects = {my_agent::tool::Effect::ReadFs},
                    },
                },
                .tool_exec_id = my_agent::ToolExecId{1},
            },
            my_agent::ToolCall{
                .id = "second",
                .name = "read",
                .args = nlohmann::json::object(),
                .status = my_agent::ToolCall::Pending{
                    .binding = my_agent::tool::ToolBinding{
                        .ref = my_agent::tool::ToolRef{1},
                        .effects = {my_agent::tool::Effect::ReadFs},
                    },
                },
                .tool_exec_id = my_agent::ToolExecId{2},
            },
        },
        .id = my_agent::MessageId{22},
        .turn_id = my_agent::TurnId{4},
    });
    model.phase = my_agent::AwaitingPermission{.call = {
        .assistant_message_id = my_agent::MessageId{22},
        .tool_exec_id = my_agent::ToolExecId{99},
    }};

    const my_agent::Step cancelled = my_agent::update(
        std::move(model), my_agent::Msg{my_agent::CancelActive{}});
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(cancelled.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(cancelled.cmd));
    const my_agent::Message& assistant = cancelled.model.domain.thread.messages.front();
    ASSERT_EQ(std::size_t{2}, assistant.tool_calls.size());
    for (const my_agent::ToolCall& call : assistant.tool_calls) {
        EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Cancelled>(
            call.status));
    }
}

TEST(ToolLoopTest, RunToolCarriesTheSessionRuntimeResolvedCalculatorRef)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / ("my_agent_ts007_core_"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    const my_agent::tool::SessionServices services{
        my_agent::tool::SessionFacts{
            .workspace_root = fs::canonical(fs::current_path()),
            .memory_roots = my_agent::tool::memory::Roots{
                .user = root / "memory-user",
                .project = root / "memory-project",
            },
        },
    };
    my_agent::tool::ToolRuntime runtime{services.registrations()};
    const auto resolved = runtime.resolve("calculator");
    ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
    const my_agent::tool::ToolRef calculator_ref = resolved->ref;

    my_agent::Step prepared = my_agent_test::prepare_request(my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "calculate 6 * 7"}}));
    const my_agent::Step call_received = my_agent::update(
        std::move(prepared.model),
        my_agent::Msg{my_agent::ResolvedStreamToolCall{
            "calculator-ref-call",
            "calculator",
            nlohmann::json{
                {"operation", "multiply"},
                {"left", 6},
                {"right", 7},
            },
            *resolved,
        }});
    const my_agent::Step execution_started = my_agent::update(
        call_received.model,
        my_agent::Msg{my_agent::StreamFinished{}});
    const auto* run_tool = std::get_if<my_agent::RunTool>(&execution_started.cmd);

    ASSERT_NE(nullptr, run_tool);
    EXPECT_EQ(calculator_ref, run_tool->ref);
}

TEST(ToolLoopTest, QueueWaitsForTheWholeToolContinuationTurn)
{
    my_agent::Step first = my_agent_test::prepare_request(my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "calculate"}}
    ));
    first = my_agent::update(
        std::move(first.model),
resolved_core_tool_call("calculator-call-1", "calculator", nlohmann::json{
                {"operation", "multiply"},
                {"left", 6},
                {"right", 7},
            })
    );
    first = my_agent::update(
        std::move(first.model),
        my_agent::Msg{my_agent::Submit{.text = "follow-up"}}
    );

    const my_agent::Step tool_started = my_agent::update(
        std::move(first.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );
    const auto* run_tool = std::get_if<my_agent::RunTool>(&tool_started.cmd);
    ASSERT_NE(nullptr, run_tool);
    ASSERT_EQ(std::size_t{1}, tool_started.model.queued_messages.size());
    EXPECT_EQ("follow-up", tool_started.model.queued_messages.front().text);

    const my_agent::Step continuation = my_agent_test::prepare_request(
        my_agent::update(
        tool_started.model,
        my_agent::Msg{execution_output(
            *run_tool,
            my_agent::tool::ToolOutput{.text = "42"})}
    ));
    ASSERT_TRUE(std::holds_alternative<my_agent::StartStream>(
        continuation.cmd));
    EXPECT_EQ(source_stream.ref.turn_id,
        std::get<my_agent::StartStream>(continuation.cmd).ref.turn_id);
    EXPECT_EQ(source_stream.ref.turn_id,
        continuation.model.domain.thread.messages.back().turn_id);
    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        continuation.model.phase));
    ASSERT_EQ(std::size_t{1}, continuation.model.queued_messages.size());
    ASSERT_EQ(std::size_t{3}, continuation.model.thread.messages.size());

    const my_agent::Step next_turn = my_agent_test::prepare_request(
        my_agent::update(
        continuation.model,
        my_agent::Msg{my_agent::StreamFinished{}}
    ));
    const auto* next_command = std::get_if<my_agent::StartStream>(
        &next_turn.cmd);
    ASSERT_NE(nullptr, next_command);
    EXPECT_EQ("follow-up", next_command->request.messages.back().text);
    EXPECT_TRUE(next_turn.model.queued_messages.empty());
    ASSERT_EQ(std::size_t{5}, next_turn.model.thread.messages.size());
    EXPECT_EQ("follow-up", next_turn.model.thread.messages[3].text);
}

TEST(ToolLoopTest, PermissionPauseAlsoKeepsQueuedTurnsWaiting)
{
    my_agent::Step step = my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::SetProfile{
            .profile = my_agent::Profile::Minimal,
        }}
    );
    step = my_agent_test::prepare_request(my_agent::update(
        std::move(step.model),
        my_agent::Msg{my_agent::Submit{.text = "read a file"}}
    ));
    step = my_agent::update(
        std::move(step.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "test.txt"}})
    );
    step = my_agent::update(
        std::move(step.model),
        my_agent::Msg{my_agent::Submit{.text = "after permission"}}
    );

    const my_agent::Step permission = my_agent::update(
        std::move(step.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        permission.model.phase));
    ASSERT_EQ(std::size_t{1}, permission.model.queued_messages.size());

    const my_agent::Step id_only = my_agent::update(
        awaiting_permission.model,
        my_agent::Msg{my_agent::PermissionApprove{.id = "read-call-1"}}
    );
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        id_only.model.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(id_only.cmd));

    const my_agent::Step approved = my_agent::update(
        permission.model,
        my_agent::Msg{approve_pending(permission.model)}
    );
    const auto* approved_run = std::get_if<my_agent::RunTool>(&approved.cmd);
    ASSERT_NE(nullptr, approved_run);
    ASSERT_EQ(std::size_t{1}, approved.model.queued_messages.size());

    const my_agent::Step continued = my_agent_test::prepare_request(
        my_agent::update(
        approved.model,
        my_agent::Msg{execution_output(
            *approved_run,
            my_agent::tool::ToolOutput{.text = "content"})}
    ));
    EXPECT_TRUE(std::holds_alternative<my_agent::StartStream>(
        continued.cmd));
    ASSERT_EQ(std::size_t{1}, continued.model.queued_messages.size());

    const my_agent::Step next = my_agent_test::prepare_request(
        my_agent::update(
        continued.model,
        my_agent::Msg{my_agent::StreamFinished{}}
    ));
    const auto* next_command = std::get_if<my_agent::StartStream>(&next.cmd);
    ASSERT_NE(nullptr, next_command);
    EXPECT_EQ("after permission", next_command->request.messages.back().text);
    EXPECT_TRUE(next.model.queued_messages.empty());
}

// M3 的同步调度契约：Core 每次只交付一个 RunTool。
// 进入 Maya 并发 Task 层后，应保留“整批终态后才能 continuation”的语义，
// 但不再要求 StreamFinished 后只能立即产生单个 RunTool。
TEST(ToolLoopTest, MultipleToolCallsRunInEmissionOrderBeforeStreamingContinues)
{
    const my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        my_agent::Model{},
        my_agent::Msg{
            my_agent::Submit{.text = "Calculate 6 * 7 and 5 * 8"},
        }
    ));

    const my_agent::Step first_call_received = my_agent::update(
        submitted.model,
resolved_core_tool_call("calculator-call-1", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 6},
                    {"right", 7},
                })
    );

    const my_agent::Step second_call_received = my_agent::update(
        first_call_received.model,
resolved_core_tool_call("calculator-call-2", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 5},
                    {"right", 8},
                })
    );

    const my_agent::Step stream_finished = my_agent::update(
        second_call_received.model,
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    // 这里之后真实运行可能不满足，因为存在并发
    const auto* run_first = std::get_if<my_agent::RunTool>(
        &stream_finished.cmd
    );
    ASSERT_NE(nullptr, run_first);
    EXPECT_EQ("calculator-call-1", run_first->id);

    const my_agent::Step first_call_completed = my_agent::update(
        stream_finished.model,
        my_agent::Msg{
            execution_output(
                *run_first,
                my_agent::tool::ToolOutput{.text = "42"}),
        }
    );

    const auto* run_second = std::get_if<my_agent::RunTool>(
        &first_call_completed.cmd
    );
    ASSERT_NE(nullptr, run_second);
    EXPECT_EQ("calculator-call-2", run_second->id);

    const my_agent::Step second_call_completed = my_agent_test::prepare_request(
        my_agent::update(
        first_call_completed.model,
        my_agent::Msg{
            execution_output(
                *run_second,
                my_agent::tool::ToolOutput{.text = "40"}),
        }
    ));

    const auto* continuation = std::get_if<my_agent::StartStream>(
        &second_call_completed.cmd
    );
    ASSERT_NE(nullptr, continuation);

    ASSERT_EQ(std::size_t{2}, continuation->request.messages.size());
    const my_agent::Message& tool_message =
        continuation->request.messages[1];
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());
    EXPECT_EQ("calculator-call-1", tool_message.tool_calls[0].id);
    EXPECT_EQ("42", tool_message.tool_calls[0].output());
    EXPECT_EQ("calculator-call-2", tool_message.tool_calls[1].id);
    EXPECT_EQ("40", tool_message.tool_calls[1].output());
}

TEST(ToolLoopTest, SerialToolSchedulingTracksCurrentlyExecutingCallId)
{
    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        my_agent::Model{},
        my_agent::Msg{
            my_agent::Submit{.text = "Calculate 6 * 7 and 5 * 8"},
        }
    ));

    my_agent::Step first_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("calculator-call-1", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 6},
                    {"right", 7},
                })
    );

    my_agent::Step second_call_received = my_agent::update(
        std::move(first_call_received.model),
resolved_core_tool_call("calculator-call-2", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 5},
                    {"right", 8},
                })
    );

    my_agent::Step first_call_started = my_agent::update(
        std::move(second_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    const auto* first_execution = std::get_if<my_agent::ExecutingTool>(
        &first_call_started.model.phase
    );
    ASSERT_NE(nullptr, first_execution);
    EXPECT_EQ("calculator-call-1", first_execution->id);

    const auto* run_first =
        std::get_if<my_agent::RunTool>(&first_call_started.cmd);
    ASSERT_NE(nullptr, run_first);
    EXPECT_EQ("calculator-call-1", run_first->id);

    const my_agent::Step second_call_started = my_agent::update(
        std::move(first_call_started.model),
        my_agent::Msg{
            execution_output(
                *run_first,
                my_agent::tool::ToolOutput{.text = "42"}),
        }
    );

    const auto* second_execution = std::get_if<my_agent::ExecutingTool>(
        &second_call_started.model.phase
    );
    ASSERT_NE(nullptr, second_execution);
    EXPECT_EQ("calculator-call-2", second_execution->id);

    const auto* run_second =
        std::get_if<my_agent::RunTool>(&second_call_started.cmd);
    ASSERT_NE(nullptr, run_second);
    EXPECT_EQ("calculator-call-2", run_second->id);
}

TEST(ToolLoopTest, ContinuesWithLaterToolAfterEarlierToolFails)
{
    std::vector<my_agent::Request> received_requests;
    std::size_t provider_turn = 0;

    my_agent::StreamEffect fake_stream =
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            received_requests.push_back(std::move(request));

            if (provider_turn++ == 0) {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamToolCall{
                        .id = "calculator-call-1",
                        .name = "calculator",
                        .args = nlohmann::json{
                            {"operation", "divide"},
                            {"left", 6},
                            {"right", 3},
                        },
                    },
                });
                sink(my_agent::ProviderEvent{
                    my_agent::StreamToolCall{
                        .id = "calculator-call-2",
                        .name = "calculator",
                        .args = nlohmann::json{
                            {"operation", "multiply"},
                            {"left", 5},
                            {"right", 8},
                        },
                    },
                });
            } else {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamTextDelta{
                        .text = "The first calculation failed; "
                                "the second result is 40.",
                    },
                });
            }

            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        };

    my_agent::HeadlessRunner runner{
        std::move(fake_stream), headless_session_runtime()};

    const my_agent::Model& model = runner.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "Divide 6 by 3 and multiply 5 by 8"},
    });

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(model.phase));
    ASSERT_EQ(std::size_t{2}, received_requests.size());

    const my_agent::Request& continuation_request = received_requests[1];
    ASSERT_EQ(std::size_t{2}, continuation_request.messages.size());

    const my_agent::Message& tool_message =
        continuation_request.messages[1];
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& failed_call = tool_message.tool_calls[0];
    EXPECT_EQ("calculator-call-1", failed_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Failed>(
        failed_call.status
    ));
    EXPECT_EQ(
        "[invalid args] unsupported calculator operation: divide",
        failed_call.output()
    );

    const my_agent::ToolCall& successful_call = tool_message.tool_calls[1];
    EXPECT_EQ("calculator-call-2", successful_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Done>(
        successful_call.status
    ));
    EXPECT_EQ("40", successful_call.output());

    ASSERT_EQ(std::size_t{3}, model.thread.messages.size());
    EXPECT_EQ(
        "The first calculation failed; the second result is 40.",
        model.thread.messages[2].text
    );
}

TEST(ToolLoopTest, ContinuesStreamingWhenFinalToolInBatchFails)
{
    std::vector<my_agent::Request> received_requests;
    std::size_t provider_turn = 0;

    my_agent::StreamEffect fake_stream =
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            received_requests.push_back(std::move(request));

            if (provider_turn++ == 0) {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamToolCall{
                        .id = "calculator-call-1",
                        .name = "calculator",
                        .args = nlohmann::json{
                            {"operation", "multiply"},
                            {"left", 6},
                            {"right", 7},
                        },
                    },
                });
                sink(my_agent::ProviderEvent{
                    my_agent::StreamToolCall{
                        .id = "calculator-call-2",
                        .name = "calculator",
                        .args = nlohmann::json{
                            {"operation", "divide"},
                            {"left", 8},
                            {"right", 2},
                        },
                    },
                });
            } else {
                sink(my_agent::ProviderEvent{
                    my_agent::StreamTextDelta{
                        .text = "The first result is 42; "
                                "the second calculation failed.",
                    },
                });
            }

            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        };

    my_agent::HeadlessRunner runner{
        std::move(fake_stream), headless_session_runtime()};

    const my_agent::Model& model = runner.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "Multiply 6 by 7 and divide 8 by 2"},
    });

    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(model.phase));
    ASSERT_EQ(std::size_t{2}, received_requests.size());

    const my_agent::Request& continuation_request = received_requests[1];
    ASSERT_EQ(std::size_t{2}, continuation_request.messages.size());

    const my_agent::Message& tool_message =
        continuation_request.messages[1];
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& successful_call = tool_message.tool_calls[0];
    EXPECT_EQ("calculator-call-1", successful_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Done>(
        successful_call.status
    ));
    EXPECT_EQ("42", successful_call.output());

    const my_agent::ToolCall& failed_call = tool_message.tool_calls[1];
    EXPECT_EQ("calculator-call-2", failed_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Failed>(
        failed_call.status
    ));
    EXPECT_EQ(
        "[invalid args] unsupported calculator operation: divide",
        failed_call.output()
    );

    ASSERT_EQ(std::size_t{3}, model.thread.messages.size());
    EXPECT_EQ(
        "The first result is 42; the second calculation failed.",
        model.thread.messages[2].text
    );
}

TEST(ToolLoopTest, MinimalProfilePausesReadToolForPermissionAfterStreamFinishes)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read CMakeLists.txt"},
        }
    ));

    my_agent::Step tool_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    const my_agent::Step stream_finished = my_agent::update(
        std::move(tool_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        stream_finished.model.phase
    ));
    ASSERT_TRUE(stream_finished.model.pending_permission.has_value());
    EXPECT_EQ(
        "read-call-1",
        stream_finished.model.pending_permission->id
    );
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        stream_finished.cmd
    ));
}

TEST(ToolLoopTest, MatchingPermissionApprovalRunsToolAndEntersExecutingPhase)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read CMakeLists.txt"},
        }
    ));

    my_agent::Step tool_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step awaiting_permission = my_agent::update(
        std::move(tool_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    const my_agent::Step approved = my_agent::update(
        awaiting_permission.model,
        my_agent::Msg{
            approve_pending(awaiting_permission.model),
        }
    );

    EXPECT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        approved.model.phase
    ));
    EXPECT_FALSE(approved.model.pending_permission.has_value());

    const auto* run_tool = std::get_if<my_agent::RunTool>(&approved.cmd);
    ASSERT_NE(nullptr, run_tool);
    EXPECT_EQ("read-call-1", run_tool->id);
    EXPECT_EQ(std::uint64_t{2}, run_tool->ref.value);
}

TEST(ToolLoopTest, PermissionPauseResumesMixedToolBatchInEmissionOrder)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read the project, then calculate 6 * 7"},
        }
    ));

    my_agent::Step read_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step calculator_call_received = my_agent::update(
        std::move(read_call_received.model),
resolved_core_tool_call("calculator-call-2", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 6},
                    {"right", 7},
                })
    );

    my_agent::Step awaiting_read_permission = my_agent::update(
        std::move(calculator_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        awaiting_read_permission.model.phase
    ));
    ASSERT_TRUE(awaiting_read_permission.model.pending_permission.has_value());
    EXPECT_EQ(
        "read-call-1",
        awaiting_read_permission.model.pending_permission->id
    );

    my_agent::Step read_started = my_agent::update(
        awaiting_read_permission.model,
        my_agent::Msg{
            approve_pending(awaiting_read_permission.model),
        }
    );

    const auto* read_execution = std::get_if<my_agent::ExecutingTool>(
        &read_started.model.phase
    );
    ASSERT_NE(nullptr, read_execution);
    EXPECT_EQ("read-call-1", read_execution->id);

    const auto* run_read = std::get_if<my_agent::RunTool>(&read_started.cmd);
    ASSERT_NE(nullptr, run_read);
    EXPECT_EQ("read-call-1", run_read->id);

    my_agent::Step calculator_started = my_agent::update(
        std::move(read_started.model),
        my_agent::Msg{
            execution_output(
                *run_read,
                my_agent::tool::ToolOutput{.text = "project contents"}),
        }
    );

    const auto* calculator_execution = std::get_if<my_agent::ExecutingTool>(
        &calculator_started.model.phase
    );
    ASSERT_NE(nullptr, calculator_execution);
    EXPECT_EQ("calculator-call-2", calculator_execution->id);

    const auto* run_calculator =
        std::get_if<my_agent::RunTool>(&calculator_started.cmd);
    ASSERT_NE(nullptr, run_calculator);
    EXPECT_EQ("calculator-call-2", run_calculator->id);

    const my_agent::Step batch_completed = my_agent_test::prepare_request(
        my_agent::update(
        std::move(calculator_started.model),
        my_agent::Msg{
            execution_output(
                *run_calculator,
                my_agent::tool::ToolOutput{.text = "42"}),
        }
    ));

    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        batch_completed.model.phase
    ));

    const auto* continuation =
        std::get_if<my_agent::StartStream>(&batch_completed.cmd);
    ASSERT_NE(nullptr, continuation);
    ASSERT_EQ(std::size_t{2}, continuation->request.messages.size());

    const my_agent::Message& tool_message =
        continuation->request.messages.back();
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& read_call = tool_message.tool_calls[0];
    EXPECT_EQ("read-call-1", read_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Done>(
        read_call.status
    ));
    EXPECT_EQ("project contents", read_call.output());

    const my_agent::ToolCall& calculator_call = tool_message.tool_calls[1];
    EXPECT_EQ("calculator-call-2", calculator_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Done>(
        calculator_call.status
    ));
    EXPECT_EQ("42", calculator_call.output());
}

TEST(ToolLoopTest, ApprovedToolFailureContinuesWithLaterToolInBatch)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read the project, then calculate 6 * 7"},
        }
    ));

    my_agent::Step read_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step calculator_call_received = my_agent::update(
        std::move(read_call_received.model),
resolved_core_tool_call("calculator-call-2", "calculator", nlohmann::json{
                    {"operation", "multiply"},
                    {"left", 6},
                    {"right", 7},
                })
    );

    my_agent::Step awaiting_permission = my_agent::update(
        std::move(calculator_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    my_agent::Step read_started = my_agent::update(
        awaiting_permission.model,
        my_agent::Msg{
            approve_pending(awaiting_permission.model),
        }
    );
    const auto* run_read = std::get_if<my_agent::RunTool>(&read_started.cmd);
    ASSERT_NE(nullptr, run_read);

    my_agent::Step calculator_started = my_agent::update(
        std::move(read_started.model),
        my_agent::Msg{
            execution_output(
                *run_read,
                std::unexpected(my_agent::tool::ToolError{
                    .kind = my_agent::tool::ErrorKind::NotFound,
                    .message = "read could not open requested file",
                })),
        }
    );

    const auto* calculator_execution = std::get_if<my_agent::ExecutingTool>(
        &calculator_started.model.phase
    );
    ASSERT_NE(nullptr, calculator_execution);
    EXPECT_EQ("calculator-call-2", calculator_execution->id);

    const auto* run_calculator =
        std::get_if<my_agent::RunTool>(&calculator_started.cmd);
    ASSERT_NE(nullptr, run_calculator);
    EXPECT_EQ("calculator-call-2", run_calculator->id);

    const my_agent::Step batch_completed = my_agent_test::prepare_request(
        my_agent::update(
        std::move(calculator_started.model),
        my_agent::Msg{
            execution_output(
                *run_calculator,
                my_agent::tool::ToolOutput{.text = "42"}),
        }
    ));

    const auto* continuation =
        std::get_if<my_agent::StartStream>(&batch_completed.cmd);
    ASSERT_NE(nullptr, continuation);
    ASSERT_EQ(std::size_t{2}, continuation->request.messages.size());

    const my_agent::Message& tool_message =
        continuation->request.messages.back();
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& failed_read = tool_message.tool_calls[0];
    EXPECT_EQ("read-call-1", failed_read.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Failed>(
        failed_read.status
    ));
    EXPECT_EQ(
        "[not found] read could not open requested file",
        failed_read.output()
    );

    const my_agent::ToolCall& successful_calculation =
        tool_message.tool_calls[1];
    EXPECT_EQ("calculator-call-2", successful_calculation.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Done>(
        successful_calculation.status
    ));
    EXPECT_EQ("42", successful_calculation.output());
}

TEST(ToolLoopTest, ProfileChangeKeepsCurrentPermissionAndAppliesToLaterTool)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read two project files"},
        }
    ));

    my_agent::Step first_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step second_call_received = my_agent::update(
        std::move(first_call_received.model),
resolved_core_tool_call("read-call-2", "read", nlohmann::json{
                    {"path", "include/my_agent/runtime/agent.hpp"},
                })
    );

    my_agent::Step awaiting_first_permission = my_agent::update(
        std::move(second_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    ASSERT_TRUE(awaiting_first_permission.model.pending_permission.has_value());
    EXPECT_EQ(
        "read-call-1",
        awaiting_first_permission.model.pending_permission->id
    );

    my_agent::Step profile_changed = my_agent::update(
        std::move(awaiting_first_permission.model),
        my_agent::Msg{
            my_agent::SetProfile{.profile = my_agent::Profile::Write},
        }
    );

    EXPECT_EQ(my_agent::Profile::Write, profile_changed.model.profile);
    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        profile_changed.model.phase
    ));
    ASSERT_TRUE(profile_changed.model.pending_permission.has_value());
    EXPECT_EQ("read-call-1", profile_changed.model.pending_permission->id);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        profile_changed.cmd
    ));

    my_agent::Step first_call_started = my_agent::update(
        profile_changed.model,
        my_agent::Msg{
            approve_pending(profile_changed.model),
        }
    );

    const auto* first_execution = std::get_if<my_agent::ExecutingTool>(
        &first_call_started.model.phase
    );
    ASSERT_NE(nullptr, first_execution);
    EXPECT_EQ("read-call-1", first_execution->id);

    const auto* run_first =
        std::get_if<my_agent::RunTool>(&first_call_started.cmd);
    ASSERT_NE(nullptr, run_first);
    EXPECT_EQ("read-call-1", run_first->id);

    const my_agent::Step second_call_started = my_agent::update(
        std::move(first_call_started.model),
        my_agent::Msg{
            execution_output(
                *run_first,
                my_agent::tool::ToolOutput{.text = "project contents"}),
        }
    );

    EXPECT_FALSE(second_call_started.model.pending_permission.has_value());

    const auto* second_execution = std::get_if<my_agent::ExecutingTool>(
        &second_call_started.model.phase
    );
    ASSERT_NE(nullptr, second_execution);
    EXPECT_EQ("read-call-2", second_execution->id);

    const auto* run_second =
        std::get_if<my_agent::RunTool>(&second_call_started.cmd);
    ASSERT_NE(nullptr, run_second);
    EXPECT_EQ("read-call-2", run_second->id);
}

TEST(ToolLoopTest, LateStreamFinishedDoesNotRedispatchApprovedTool)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read CMakeLists.txt"},
        }
    ));

    my_agent::Step tool_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step awaiting_permission = my_agent::update(
        std::move(tool_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    my_agent::Step approved = my_agent::update(
        awaiting_permission.model,
        my_agent::Msg{
            approve_pending(awaiting_permission.model),
        }
    );

    const my_agent::Step late_stream_finished = my_agent::update(
        std::move(approved.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    EXPECT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        late_stream_finished.model.phase
    ));
    EXPECT_FALSE(late_stream_finished.model.pending_permission.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(
        late_stream_finished.cmd
    ));
}

TEST(ToolLoopTest, RejectingFirstAwaitingCallAdvancesToNextPermission)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read CMakeLists.txt"},
        }
    ));

    my_agent::Step tool_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step second_tool_call_received = my_agent::update(
        std::move(tool_call_received.model),
resolved_core_tool_call("read-call-2", "read", nlohmann::json{
                    {"path", "include/my_agent/runtime/agent.hpp"},
                })
    );

    my_agent::Step awaiting_permission = my_agent::update(
        std::move(second_tool_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    const my_agent::Step rejected = my_agent::update(
        awaiting_permission.model,
        my_agent::Msg{
            reject_pending(awaiting_permission.model,
                           "Use the information already available."),
        }
    );

    EXPECT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        rejected.model.phase
    ));
    ASSERT_TRUE(rejected.model.pending_permission.has_value());
    EXPECT_EQ(
        "read-call-2",
        rejected.model.pending_permission->id
    );
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(rejected.cmd));

    const my_agent::Message& tool_message =
        rejected.model.thread.messages.back();
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& rejected_call = tool_message.tool_calls[0];
    EXPECT_EQ("read-call-1", rejected_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Rejected>(
        rejected_call.status
    ));
    EXPECT_EQ(
        "User rejected this tool call. Feedback: "
        "Use the information already available.",
        rejected_call.output()
    );

    const my_agent::ToolCall& next_call = tool_message.tool_calls[1];
    EXPECT_EQ("read-call-2", next_call.id);
    EXPECT_TRUE(next_call.is_pending());
}

TEST(ToolLoopTest, RejectingFinalPermissionContinuesWithCompleteBatchResults)
{
    my_agent::Model model{};
    model.profile = my_agent::Profile::Minimal;

    my_agent::Step submitted = my_agent_test::prepare_request(
        my_agent::update(
        std::move(model),
        my_agent::Msg{
            my_agent::Submit{.text = "Read two project files"},
        }
    ));

    my_agent::Step first_call_received = my_agent::update(
        std::move(submitted.model),
resolved_core_tool_call("read-call-1", "read", nlohmann::json{{"path", "CMakeLists.txt"}})
    );

    my_agent::Step second_call_received = my_agent::update(
        std::move(first_call_received.model),
resolved_core_tool_call("read-call-2", "read", nlohmann::json{
                    {"path", "include/my_agent/runtime/agent.hpp"},
                })
    );

    my_agent::Step awaiting_first_permission = my_agent::update(
        std::move(second_call_received.model),
        my_agent::Msg{my_agent::StreamFinished{}}
    );

    my_agent::Step awaiting_second_permission = my_agent::update(
        awaiting_first_permission.model,
        my_agent::Msg{
            reject_pending(awaiting_first_permission.model,
                           "Use the information already available."),
        }
    );

    const my_agent::Step rejected_final = my_agent_test::prepare_request(
        my_agent::update(
        awaiting_second_permission.model,
        my_agent::Msg{
            reject_pending(awaiting_second_permission.model),
        }
    ));

    EXPECT_TRUE(std::holds_alternative<my_agent::Streaming>(
        rejected_final.model.phase
    ));
    EXPECT_FALSE(rejected_final.model.pending_permission.has_value());

    const auto* start_stream =
        std::get_if<my_agent::StartStream>(&rejected_final.cmd);
    ASSERT_NE(nullptr, start_stream);
    ASSERT_EQ(std::size_t{2}, start_stream->request.messages.size());

    const my_agent::Message& tool_message =
        start_stream->request.messages.back();
    ASSERT_EQ(std::size_t{2}, tool_message.tool_calls.size());

    const my_agent::ToolCall& first_call = tool_message.tool_calls[0];
    EXPECT_EQ("read-call-1", first_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Rejected>(
        first_call.status
    ));
    EXPECT_EQ(
        "User rejected this tool call. Feedback: "
        "Use the information already available.",
        first_call.output()
    );

    const my_agent::ToolCall& second_call = tool_message.tool_calls[1];
    EXPECT_EQ("read-call-2", second_call.id);
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Rejected>(
        second_call.status
    ));
    EXPECT_EQ("User rejected this tool call.", second_call.output());
}
