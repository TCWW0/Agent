#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "request_preparation_test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <stdexcept>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using my_agent::ProviderEvent;
using my_agent::ProviderEventSink;
using my_agent::ResolvedStreamToolCall;
using my_agent::StreamFinished;
using my_agent::StreamToolCall;
using my_agent::ToolCall;
using my_agent::ToolCallResolution;
using my_agent::ToolReplayMode;
using my_agent::ToolReplayRule;
using my_agent::TurnId;
using my_agent::StreamId;
using my_agent::tool::Effect;
using my_agent::tool::EffectSet;
using my_agent::tool::ExecResult;
using my_agent::tool::RegisteredTool;
using my_agent::tool::ToolBinding;
using my_agent::tool::ToolDescriptor;
using my_agent::tool::ToolExecutionContext;
using my_agent::tool::ToolName;
using my_agent::tool::ToolOutput;
using my_agent::tool::ToolRef;
using my_agent::tool::ToolResolutionError;
using my_agent::tool::ToolRuntime;

constexpr std::string_view kProbeName = "ts005_probe";

nlohmann::json probe_schema()
{
    return nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json{
            {"value", nlohmann::json{{"type", "string"}}},
        }},
    };
}

std::shared_ptr<const ToolRuntime> probe_runtime()
{
    auto runtime = std::make_shared<ToolRuntime>(
        std::vector<RegisteredTool>{
            RegisteredTool{
                .descriptor = ToolDescriptor{
                    .name = ToolName{.value = std::string{kProbeName}},
                    .description = "TS-005 resolution probe",
                    .input_schema = probe_schema(),
                    .effects = EffectSet{Effect::ReadFs},
                    .replay = ToolReplayRule{
                        .mode = ToolReplayMode::Full,
                        .weight = 1,
                    },
                },
                .handler = [](
                    const nlohmann::json&,
                    const ToolExecutionContext&) -> ExecResult {
                    return ToolOutput{.text = "probe output"};
                },
            },
        });
    return runtime;
}

void emit_raw_tool_call(
    const ProviderEventSink& sink,
    std::string id,
    std::string name)
{
    sink(ProviderEvent{StreamToolCall{
        .id = std::move(id),
        .name = std::move(name),
        .args = nlohmann::json{{"value", "payload"}},
    }});
}

my_agent::Msg resolved_core_tool_call(
    TurnId turn_id,
    StreamId stream_id,
    std::string id,
    std::string name,
    ToolCallResolution resolution)
{
    return my_agent::Msg{ResolvedStreamToolCall{
        my_agent::StreamRef{.turn_id = turn_id, .stream_id = stream_id},
        std::move(id),
        std::move(name),
        nlohmann::json{{"value", "payload"}},
        std::move(resolution),
    }};
}

ToolCallResolution resolved_binding(
    std::uint64_t ref,
    EffectSet effects = EffectSet{Effect::ReadFs})
{
    return ToolBinding{
        .ref = ToolRef{.value = ref},
        .effects = effects,
    };
}

my_agent::Step started_stream()
{
    return my_agent_test::prepare_request(my_agent::update(
        my_agent::Model{},
        my_agent::Msg{my_agent::Submit{.text = "resolve a tool"}}));
}

const my_agent::Message& assistant_message(const my_agent::Model& model)
{
    for (auto it = model.thread.messages.rbegin();
         it != model.thread.messages.rend();
         ++it) {
        if (it->role == my_agent::Role::Assistant && !it->tool_calls.empty()) {
            return *it;
        }
    }

    ADD_FAILURE() << "expected an assistant message containing a tool call";
    return model.thread.messages.back();
}

}  // namespace

TEST(TS005HostTest, ResolvesRawProviderCallIntoPendingBindingBeforeCore)
{
    std::atomic_size_t stream_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request, ProviderEventSink sink) {
            ++stream_calls;
            emit_raw_tool_call(sink, "probe-call", std::string{kProbeName});
            sink(ProviderEvent{StreamFinished{}});
        },
        probe_runtime(),
    };

    host.dispatch(my_agent::Msg{
        my_agent::SetProfile{.profile = my_agent::Profile::Minimal},
    });
    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "use the probe"},
    });
    host.run_until_quiescent();

    EXPECT_EQ(std::size_t{1}, stream_calls.load());
    ASSERT_TRUE(std::holds_alternative<my_agent::AwaitingPermission>(
        host.model().phase));

    const my_agent::Message& assistant = assistant_message(host.model());
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    const ToolCall& call = assistant.tool_calls.front();
    const auto* pending = std::get_if<ToolCall::Pending>(&call.status);
    ASSERT_NE(nullptr, pending);
    EXPECT_EQ(std::uint64_t{1}, pending->binding.ref.value);
    EXPECT_TRUE(pending->binding.effects.has(Effect::ReadFs));
    EXPECT_EQ("probe-call", call.id);
    EXPECT_EQ(kProbeName, call.name);
}

TEST(TS005HostTest, UnknownCallKeepsRawFieldsAndProducesAvailableToolFailure)
{
    std::atomic_size_t stream_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request, ProviderEventSink sink) {
            if (stream_calls.fetch_add(1) == 0) {
                emit_raw_tool_call(sink, "unknown-call", "ts005_missing");
            } else {
                sink(ProviderEvent{
                    my_agent::StreamTextDelta{.text = "recovered"},
                });
            }
            sink(ProviderEvent{StreamFinished{}});
        },
        probe_runtime(),
    };
    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "use an unknown tool"},
    });
    host.run_until_quiescent();

    ASSERT_TRUE(std::holds_alternative<my_agent::Idle>(host.model().phase));
    EXPECT_FALSE(host.model().pending_permission.has_value());

    const my_agent::Message& assistant = assistant_message(host.model());
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    const ToolCall& call = assistant.tool_calls.front();
    EXPECT_EQ("unknown-call", call.id);
    EXPECT_EQ("ts005_missing", call.name);
    const nlohmann::json expected_args{{"value", "payload"}};
    EXPECT_EQ(expected_args, call.args);
    const auto* failed = std::get_if<ToolCall::Failed>(&call.status);
    ASSERT_NE(nullptr, failed);
    EXPECT_NE(std::string::npos, failed->output.find("available tools"));
    EXPECT_NE(std::string::npos, failed->output.find(std::string{kProbeName}));
}
TEST(TS005HostTest, CaseAndWhitespaceMismatchesRemainUnknown)
{
    const std::vector<std::string> rejected_names{
        "TS005_PROBE",
        " ts005_probe",
        "ts005_probe ",
    };

    for (const std::string& rejected_name : rejected_names) {
        std::atomic_size_t stream_calls{0};
        my_agent::AsyncHost host{
            [&](my_agent::Request, ProviderEventSink sink) {
                if (stream_calls.fetch_add(1) == 0) {
                    emit_raw_tool_call(sink, "mismatch-call", rejected_name);
                } else {
                    sink(ProviderEvent{
                        my_agent::StreamTextDelta{.text = "recovered"},
                    });
                }
                sink(ProviderEvent{StreamFinished{}});
            },
            probe_runtime(),
        };
        host.dispatch(my_agent::Msg{
            my_agent::Submit{.text = "exact matching"},
        });
        host.run_until_quiescent();

        const my_agent::Message& assistant = assistant_message(host.model());
        ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size())
            << rejected_name;
        const ToolCall& call = assistant.tool_calls.front();
        EXPECT_EQ(rejected_name, call.name);
        EXPECT_TRUE(std::holds_alternative<ToolCall::Failed>(call.status))
            << rejected_name;
    }
}

// The Host must supply a resolution before a call can cross the Core boundary.
static_assert(!std::is_default_constructible_v<ResolvedStreamToolCall>);
static_assert(!std::is_constructible_v<ResolvedStreamToolCall,
    my_agent::StreamRef, std::string, std::string, nlohmann::json>);
static_assert(std::is_constructible_v<ResolvedStreamToolCall,
    my_agent::StreamRef, std::string, std::string, nlohmann::json, ToolBinding>);
static_assert(std::is_constructible_v<ResolvedStreamToolCall,
    my_agent::StreamRef, std::string, std::string, nlohmann::json, ToolResolutionError>);

TEST(TS005MessageTest, EmptyResolutionCannotMasqueradeAsASuccessfulBinding)
{
    EXPECT_THROW((ResolvedStreamToolCall{
        my_agent::StreamRef{my_agent::TurnId{1}, my_agent::StreamId{1}},
        "probe-call", std::string{kProbeName}, nlohmann::json::object(),
        ToolCallResolution{}}), std::invalid_argument);
}

TEST(TS005CoreTest, UnknownResolutionIsFailedAndDoesNotEnterPermissionOrExecution)
{
    my_agent::Step started = started_stream();
    const auto* stream = std::get_if<my_agent::StartStream>(&started.cmd);
    ASSERT_NE(nullptr, stream);

    const my_agent::Step failed = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            stream->ref.turn_id,
            stream->ref.stream_id,
            "unknown-call",
            "ts005_missing",
            ToolResolutionError{
                .name = "ts005_missing",
                .message =
                    "unknown tool: ts005_missing; available tools: ts005_probe",
            }));

    const my_agent::Message& assistant = assistant_message(failed.model);
    ASSERT_EQ(std::size_t{1}, assistant.tool_calls.size());
    const ToolCall& call = assistant.tool_calls.front();
    const auto* failed_status = std::get_if<ToolCall::Failed>(&call.status);
    ASSERT_NE(nullptr, failed_status);
    EXPECT_EQ(
        "unknown tool: ts005_missing; available tools: ts005_probe",
        failed_status->output);
    EXPECT_TRUE(std::holds_alternative<my_agent::NoCommand>(failed.cmd));

    const my_agent::Step after_finished = my_agent::update(
        std::move(failed.model),
        my_agent::Msg{my_agent::StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = stream->ref.turn_id, .stream_id = stream->ref.stream_id},
        }});
    EXPECT_FALSE(std::holds_alternative<my_agent::AwaitingPermission>(
        after_finished.model.phase));
    EXPECT_FALSE(std::holds_alternative<my_agent::ExecutingTool>(
        after_finished.model.phase));
    EXPECT_FALSE(std::holds_alternative<my_agent::RunTool>(
        after_finished.cmd));
}

TEST(TS005CoreTest, ResolvedCallsRunSequentiallyWithTheirOwnOpaqueReferences)
{
    my_agent::Step started = started_stream();
    const auto* stream = std::get_if<my_agent::StartStream>(&started.cmd);
    ASSERT_NE(nullptr, stream);

    my_agent::Step first = my_agent::update(
        std::move(started.model),
        resolved_core_tool_call(
            stream->ref.turn_id,
            stream->ref.stream_id,
            "first-call",
            std::string{kProbeName},
            resolved_binding(41)));
    my_agent::Step second = my_agent::update(
        std::move(first.model),
        resolved_core_tool_call(
            stream->ref.turn_id,
            stream->ref.stream_id,
            "second-call",
            std::string{kProbeName},
            resolved_binding(42)));

    const my_agent::Step first_started = my_agent::update(
        std::move(second.model),
        my_agent::Msg{my_agent::StreamFinished{
            .ref = my_agent::StreamRef{.turn_id = stream->ref.turn_id, .stream_id = stream->ref.stream_id},
        }});
    const auto* first_run = std::get_if<my_agent::RunTool>(
        &first_started.cmd);
    ASSERT_NE(nullptr, first_run);
    EXPECT_EQ(my_agent::ToolExecId{1}, first_run->call.tool_exec_id);
    EXPECT_EQ(std::uint64_t{41}, first_run->ref.value);

    const my_agent::Step second_started = my_agent::update(
        std::move(first_started.model),
        my_agent::Msg{my_agent::ToolExecOutput{
            .call = first_run->call,
            .ref = first_run->ref,
            .result = ToolOutput{.text = "first output"},
        }});
    const auto* second_run = std::get_if<my_agent::RunTool>(
        &second_started.cmd);
    ASSERT_NE(nullptr, second_run);
    EXPECT_EQ(my_agent::ToolExecId{2}, second_run->call.tool_exec_id);
    EXPECT_EQ(std::uint64_t{42}, second_run->ref.value);
}

