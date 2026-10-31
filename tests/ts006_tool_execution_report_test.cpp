#include "my_agent/provider/anthropic.hpp"
#include "my_agent/provider/openai.hpp"
#include "my_agent/provider/ollama.hpp"
#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "request_preparation_test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using my_agent::Message;
using my_agent::Model;
using my_agent::Request;
using my_agent::Role;
using my_agent::RunTool;
using my_agent::Step;
using my_agent::StreamFinished;
using my_agent::StreamToolCall;
using my_agent::ToolCall;
using my_agent::ToolExecOutput;
using my_agent::ToolReplayMode;
using my_agent::ToolReplayRule;
using my_agent::TurnId;
using my_agent::StreamId;
using my_agent::ProviderEvent;
using my_agent::ProviderEventSink;
using my_agent::ResolvedStreamToolCall;
using my_agent::ToolCallResolution;
using my_agent::tool::EffectSet;
using my_agent::tool::ErrorKind;
using my_agent::tool::ExecResult;
using my_agent::tool::RegisteredTool;
using my_agent::tool::ToolDescriptor;
using my_agent::tool::ToolHandler;
using my_agent::tool::ToolExecutionContext;
using my_agent::tool::ToolName;
using my_agent::tool::ToolOutput;
using my_agent::tool::ToolRef;
using my_agent::tool::ToolRuntime;

constexpr std::string_view kToolName = "ts006_probe";
constexpr std::string_view kCancelledOutput =
    "Tool execution was cancelled before a result was available.";

RegisteredTool registration(ToolHandler handler)
{
    return RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = std::string{kToolName}},
            .description = "TS-006 execution probe",
            .input_schema = nlohmann::json{{"type", "object"}},
            .effects = EffectSet{},
            .replay = ToolReplayRule{
                .mode = ToolReplayMode::Full,
                .weight = 1,
            },
        },
        .handler = std::move(handler),
    };
}

std::shared_ptr<const ToolRuntime> runtime_with_handler(ToolHandler handler)
{
    return std::make_shared<const ToolRuntime>(
        std::vector<RegisteredTool>{registration(std::move(handler))});
}

my_agent::Msg resolved_tool_call(
    TurnId turn_id,
    StreamId stream_id,
    std::string id,
    std::uint64_t ref)
{
    return my_agent::Msg{ResolvedStreamToolCall{
        my_agent::StreamRef{.turn_id = turn_id, .stream_id = stream_id},
        std::move(id),
        std::string{kToolName},
        nlohmann::json{{"value", "payload"}},
        ToolCallResolution{
            my_agent::tool::ToolBinding{
                .ref = ToolRef{.value = ref},
                .effects = EffectSet{},
            },
        },
    }};
}

struct RunningTool {
    Model model;
    RunTool command;
};

RunningTool running_tool()
{
    Step step = my_agent_test::prepare_request(my_agent::update(
        Model{},
        my_agent::Msg{my_agent::Submit{.text = "run the probe"}}));
    const auto& stream = std::get<my_agent::StartStream>(step.cmd);
    const TurnId turn_id = stream.ref.turn_id;
    const StreamId stream_id = stream.ref.stream_id;

    step = my_agent::update(
        std::move(step.model),
        resolved_tool_call(turn_id, stream_id, "probe-call", 1));
    step = my_agent::update(
        std::move(step.model),
        my_agent::Msg{StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = turn_id, .stream_id = stream_id},
        }});

    return RunningTool{
        .model = std::move(step.model),
        .command = std::get<RunTool>(step.cmd),
    };
}

Step two_pending_tools()
{
    Step step = my_agent_test::prepare_request(my_agent::update(
        Model{},
        my_agent::Msg{my_agent::Submit{.text = "run two probes"}}));
    const auto& stream = std::get<my_agent::StartStream>(step.cmd);
    const TurnId turn_id = stream.ref.turn_id;
    const StreamId stream_id = stream.ref.stream_id;

    step = my_agent::update(
        std::move(step.model),
        resolved_tool_call(turn_id, stream_id, "probe-call-1", 1));
    step = my_agent::update(
        std::move(step.model),
        resolved_tool_call(turn_id, stream_id, "probe-call-2", 1));
    return my_agent::update(
        std::move(step.model),
        my_agent::Msg{StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = turn_id, .stream_id = stream_id},
        }});
}

const Message& assistant_with_tool(const Model& model)
{
    for (const Message& message : model.domain.thread.messages) {
        if (message.role == Role::Assistant && !message.tool_calls.empty()) {
            return message;
        }
    }
    ADD_FAILURE() << "expected assistant tool message";
    return model.domain.thread.messages.back();
}

ToolExecOutput report_for(
    const RunTool& command,
    std::uint64_t ref,
    std::string text = "probe result")
{
    return ToolExecOutput{
        .call = command.call,
        .ref = ToolRef{.value = ref},
        .result = ToolOutput{.text = std::move(text)},
    };
}

Message cancelled_tool_message()
{
    Message message{
        .role = Role::Assistant,
        .text = {},
        .error = {},
    };
    message.tool_calls.push_back(ToolCall{
        .id = "cancelled-call",
        .name = std::string{kToolName},
        .args = nlohmann::json{{"value", "payload"}},
        .status = ToolCall::Cancelled{
            .output = std::string{kCancelledOutput},
        },
    });
    return message;
}

Request request_with_cancelled_tool()
{
    Request request;
    request.messages.push_back(cancelled_tool_message());
    return request;
}

}  // namespace

TEST(TS006ToolRuntimeTest, ConvertsStandardHandlerExceptionToTypedFailure)
{
    ToolRuntime runtime{std::vector<RegisteredTool>{registration(
        [](const nlohmann::json&, const ToolExecutionContext&) -> ExecResult {
            throw std::runtime_error{"handler exploded"};
        })}};
    const auto resolved = runtime.resolve(kToolName);
    ASSERT_TRUE(resolved.has_value());

    const ExecResult result = runtime.execute(resolved->ref, nlohmann::json{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorKind::ExecutionFailed, result.error().kind);
    EXPECT_EQ("handler exploded", result.error().message);
}

TEST(TS006ToolRuntimeTest, ConvertsNonStandardHandlerExceptionToTypedFailure)
{
    ToolRuntime runtime{std::vector<RegisteredTool>{registration(
        [](const nlohmann::json&, const ToolExecutionContext&) -> ExecResult {
            throw 42;
        })}};
    const auto resolved = runtime.resolve(kToolName);
    ASSERT_TRUE(resolved.has_value());

    const ExecResult result = runtime.execute(resolved->ref, nlohmann::json{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(ErrorKind::ExecutionFailed, result.error().kind);
    EXPECT_EQ("unknown tool failure", result.error().message);
}

TEST(TS006CoreTest, AcceptsMatchingResultAndPreparesContinuation)
{
    const RunningTool running = running_tool();
    const Step completed = my_agent::update(
        running.model,
        my_agent::Msg{report_for(running.command, 1)});

    const Message& assistant = assistant_with_tool(completed.model);
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    const auto* done = std::get_if<ToolCall::Done>(
        &assistant.tool_calls.front().status);
    ASSERT_NE(nullptr, done);
    EXPECT_EQ("probe result", done->output);
    EXPECT_TRUE(std::holds_alternative<my_agent::PrepareRequest>(completed.cmd));
    EXPECT_EQ(std::string::npos,
              completed.model.domain.thread.messages.front().text.find("elapsed_ms"));
}

TEST(TS006CoreTest, IgnoresReportWithMismatchedToolRef)
{
    const RunningTool running = running_tool();
    const Step ignored = my_agent::update(
        running.model,
        my_agent::Msg{report_for(running.command, 99, "wrong tool")});

    const Message& assistant = assistant_with_tool(ignored.model);
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    EXPECT_TRUE(std::holds_alternative<ToolCall::Pending>(
        assistant.tool_calls.front().status));
    EXPECT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        ignored.model.session.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(ignored.cmd));
}

TEST(TS006CoreTest, CancelSettlesAllPendingCallsBeforeAdvancing)
{
    const Step running = two_pending_tools();
    const Step cancelled = my_agent::update(
        running.model,
        my_agent::Msg{my_agent::CancelActive{}});

    const Message& assistant = assistant_with_tool(cancelled.model);
    ASSERT_EQ(std::size_t{2}, assistant.tool_calls.size());
    for (const ToolCall& call : assistant.tool_calls) {
        const auto* status = std::get_if<ToolCall::Cancelled>(&call.status);
        ASSERT_NE(nullptr, status);
        EXPECT_EQ(std::string{kCancelledOutput}, status->output);
    }
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(cancelled.model.session.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::CancelOperation>(cancelled.cmd));
}

TEST(TS006CoreTest, LateReportCannotOverwriteCancelledCall)
{
    const RunningTool running = running_tool();
    const Step cancelled = my_agent::update(
        running.model,
        my_agent::Msg{my_agent::CancelActive{}});
    const Step late = my_agent::update(
        cancelled.model,
        my_agent::Msg{report_for(running.command, 1, "late result")});

    const Message& assistant = assistant_with_tool(late.model);
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    const auto* status = std::get_if<ToolCall::Cancelled>(
        &assistant.tool_calls.front().status);
    ASSERT_NE(nullptr, status);
    EXPECT_EQ(kCancelledOutput, status->output);
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(late.model.session.phase));
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(late.cmd));
}

TEST(TS006AsyncHostTest, RunsRegisteredHandlerWithArgumentsAndCancellationContext)
{
    std::atomic_bool observed_args{false};
    std::atomic_bool observed_probe{false};
    std::atomic_size_t stream_calls{0};

    my_agent::AsyncHost host{
        [&](Request, ProviderEventSink sink) {
            if (stream_calls.fetch_add(1) == 0) {
                sink(ProviderEvent{StreamToolCall{
                    .id = "probe-call",
                    .name = std::string{kToolName},
                    .args = nlohmann::json{{"value", "payload"}},
                }});
            }
            sink(ProviderEvent{StreamFinished{}});
        },
        runtime_with_handler([&](const nlohmann::json& args,
            const ToolExecutionContext& context) -> ExecResult {
            observed_args.store(args == nlohmann::json{{"value", "payload"}},
                std::memory_order_release);
            observed_probe.store(
                static_cast<bool>(context.cancelled),
                std::memory_order_release);
            return ToolOutput{.text = "host result"};
        }),
    };

    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "execute probe"}});
    host.run_until_quiescent();

    EXPECT_TRUE(observed_args.load(std::memory_order_acquire));
    EXPECT_EQ("host result", assistant_with_tool(host.model()).tool_calls.front().output());
    EXPECT_TRUE(observed_probe.load(std::memory_order_acquire));
}

TEST(TS006AsyncHostTest, RejectedReportKeepsOperationAvailableForCancellation)
{
    std::binary_semaphore handler_started{0};
    std::binary_semaphore release_handler{0};
    std::binary_semaphore handler_finished{0};
    std::atomic_bool observed_cancellation{false};
    std::atomic_size_t stream_calls{0};

    my_agent::AsyncHost host{
        [&](Request, ProviderEventSink sink) {
            if (stream_calls.fetch_add(1) == 0) {
                sink(ProviderEvent{StreamToolCall{
                    .id = "probe-call",
                    .name = std::string{kToolName},
                    .args = nlohmann::json{{"value", "payload"}},
                }});
            }
            sink(ProviderEvent{StreamFinished{}});
        },
        runtime_with_handler([&](const nlohmann::json&,
            const ToolExecutionContext& context) -> ExecResult {
            handler_started.release();
            release_handler.acquire();
            observed_cancellation.store(
                context.cancelled(), std::memory_order_release);
            handler_finished.release();
            return ToolOutput{.text = "late result"};
        }),
    };

    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "execute probe"}});
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{3};
    while (!std::holds_alternative<my_agent::ExecutingTool>(host.model().phase)
        && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(host.drain_inbox());
        static_cast<void>(host.wait_wake(std::chrono::milliseconds{20}));
    }
    static_cast<void>(host.drain_inbox());
    const auto* active = std::get_if<my_agent::ExecutingTool>(
        &host.model().phase);
    if (active == nullptr) {
        release_handler.release();
        host.shutdown();
        FAIL() << "tool execution did not start";
    }
    const my_agent::ToolCallRef call = active->call;
    const RunTool command{.call = call, .ref = ToolRef{1}};
    if (!handler_started.try_acquire_for(std::chrono::seconds{3})) {
        release_handler.release();
        host.shutdown();
        FAIL() << "tool handler did not enter its blocking section";
    }

    auto wrong_message_report = report_for(command, 1, "wrong message");
    wrong_message_report.call.assistant_message_id =
        my_agent::MessageId{call.assistant_message_id.value + 1};
    host.dispatch(my_agent::Msg{std::move(wrong_message_report)});
    EXPECT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        host.model().phase));
    host.dispatch(my_agent::Msg{report_for(command, 99, "wrong tool")});
    EXPECT_TRUE(std::holds_alternative<my_agent::ExecutingTool>(
        host.model().phase));
    host.dispatch(my_agent::Msg{my_agent::CancelActive{}});
    release_handler.release();

    ASSERT_TRUE(handler_finished.try_acquire_for(std::chrono::seconds{3}));
    EXPECT_TRUE(observed_cancellation.load(std::memory_order_acquire));
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(host.model().phase));
    host.dispatch(my_agent::Msg{report_for(command, 1, "late result")});
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(host.model().phase));
    const Message& assistant = assistant_with_tool(host.model());
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    EXPECT_TRUE(std::holds_alternative<ToolCall::Cancelled>(
        assistant.tool_calls.front().status));
}

TEST(TS006ProviderHistoryTest, OpenAIEmitsResultForCancelledToolCall)
{
    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::openai::build_request_body(
            "model", request_with_cancelled_tool()));

    ASSERT_EQ(std::size_t{2}, body.at("messages").size());
    EXPECT_EQ("tool", body.at("messages").at(1).at("role"));
    EXPECT_EQ(std::string{kCancelledOutput},
              body.at("messages").at(1).at("content").get<std::string>());
}

TEST(TS006ProviderHistoryTest, AnthropicEmitsResultForCancelledToolCall)
{
    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::anthropic::build_request_body(
            "model", request_with_cancelled_tool()));

    ASSERT_EQ(std::size_t{2}, body.at("messages").size());
    EXPECT_EQ("user", body.at("messages").at(1).at("role"));
    EXPECT_EQ("tool_result",
              body.at("messages").at(1).at("content").at(0).at("type"));
    EXPECT_EQ(std::string{kCancelledOutput},
              body.at("messages").at(1).at("content").at(0).at("content")
                  .get<std::string>());
}

TEST(TS006ProviderHistoryTest, OllamaEmitsResultForCancelledToolCall)
{
    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body(
            "model", request_with_cancelled_tool()));

    ASSERT_EQ(std::size_t{2}, body.at("messages").size());
    EXPECT_EQ("tool", body.at("messages").at(1).at("role"));
    EXPECT_EQ(std::string{kCancelledOutput},
              body.at("messages").at(1).at("content").get<std::string>());
}
