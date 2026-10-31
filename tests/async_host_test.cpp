#include "my_agent/runtime/async_host.hpp"
#include "my_agent/runtime/agent.hpp"
#include "my_agent/prompt/system_prompt.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "my_agent/tool/session_services.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace {

using my_agent::ToolReplayMode;
using my_agent::ToolReplayRule;
using my_agent::ToolSpec;
using my_agent::tool::ExecResult;
using my_agent::tool::RegisteredTool;
using my_agent::tool::ToolDescriptor;
using my_agent::tool::ToolExecutionContext;
using my_agent::tool::ToolName;
using my_agent::tool::ToolOutput;
using my_agent::tool::ToolRuntime;

static_assert(!std::is_constructible_v<my_agent::AsyncHost,
                                       my_agent::StreamEffect>);
static_assert(std::is_constructible_v<my_agent::AsyncHost,
                                      my_agent::StreamEffect,
                                      std::shared_ptr<const ToolRuntime>>);

constexpr std::string_view kProbeName = "projection_probe";

nlohmann::json probe_schema(std::string_view marker)
{
    return nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json{
            {"sentinel", nlohmann::json{{"type", "string"}}},
        }},
        {"x-ts004-marker", marker},
    };
}

std::shared_ptr<const ToolRuntime> probe_runtime(
    std::string_view marker = "request-preparation",
    my_agent::tool::ToolHandler handler = [](const nlohmann::json&,
        const ToolExecutionContext&) -> ExecResult {
        return ToolOutput{.text = "unused"};
    })
{
    auto runtime = std::make_shared<ToolRuntime>(
        std::vector<RegisteredTool>{
            RegisteredTool{
                .descriptor = ToolDescriptor{
                    .name = ToolName{.value = std::string{kProbeName}},
                    .description = "TS-004 projection sentinel",
                    .input_schema = probe_schema(marker),
                    .effects = {},
                    .replay = ToolReplayRule{
                        .mode = ToolReplayMode::Full,
                        .weight = 3,
                    },
                },
                .handler = std::move(handler),
            },
        });
    return runtime;
}

std::shared_ptr<const ToolRuntime> empty_runtime()
{
    return std::make_shared<const ToolRuntime>(std::vector<RegisteredTool>{});
}

void expect_probe_tool(
    const my_agent::Request& request,
    std::string_view marker = "request-preparation")
{
    ASSERT_EQ(std::size_t{1}, request.tools.size());
    EXPECT_EQ(kProbeName, request.tools.front().name);
    EXPECT_EQ("TS-004 projection sentinel",
              request.tools.front().description);
    EXPECT_EQ(probe_schema(marker), request.tools.front().input_schema);
    EXPECT_EQ(ToolReplayMode::Full, request.tools.front().replay_mode);
    EXPECT_EQ(std::uint32_t{3}, request.tools.front().replay_weight);
}

struct RequestCapture {
    void add(my_agent::Request request)
    {
        std::lock_guard<std::mutex> lock{mutex};
        requests.push_back(std::move(request));
    }

    [[nodiscard]] std::vector<my_agent::Request> snapshot() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return requests;
    }

    mutable std::mutex mutex;
    std::vector<my_agent::Request> requests;
};

void install_context_management(my_agent::AsyncHost& host)
{
    my_agent::ContextManagementConfig management;
    management.configured_limit = 8192;
    management.safety_margin = 64;
    host.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{management, "test-provider", "test-model"}}});
}

struct CalculatorHostRun {
    std::vector<my_agent::Request> requests;
    my_agent::Model model;
};

CalculatorHostRun run_native_calculator(
    nlohmann::json args,
    std::string final_answer)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / ("my_agent_ts007_async_"
           + std::to_string(::getpid()) + "_"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "skills");
    fs::create_directories(root / "docs");

    my_agent::tool::SessionFacts facts{
        .workspace_root = root / "workspace",
        .memory_roots = my_agent::tool::memory::Roots{
            .user = root / "memory-user",
            .project = root / "memory-project",
        },
        .skill_roots = {root / "skills"},
        .docs = my_agent::tool::docs::DocsConfig{
            .root = root / "docs",
            .root_missing = false,
        },
    };
    const my_agent::tool::SessionServices services{std::move(facts)};
    auto runtime = std::make_shared<ToolRuntime>(services.registrations());

    RequestCapture capture;
    std::atomic_size_t provider_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            capture.add(std::move(request));
            if (provider_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "calculator-call",
                    .name = "calculator",
                    .args = std::move(args),
                }});
            } else {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = std::move(final_answer),
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        std::move(runtime),
    };
    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "use calculator"}});
    host.run_until_quiescent();

    std::error_code ec;
    fs::remove_all(root, ec);
    return CalculatorHostRun{
        .requests = capture.snapshot(),
        .model = host.model(),
    };
}

}  // namespace

TEST(AsyncHostTest, InstallsOnlyTheUnifiedContextConfiguration)
{
    my_agent::AsyncHost host{
        [](my_agent::Request, my_agent::ProviderEventSink) {},
        empty_runtime(),
    };
    my_agent::ContextManagementConfig management;
    management.configured_limit = 4096;
    management.safety_margin = 64;

    host.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{management, "test-provider", "test-model"}}});

    EXPECT_EQ(4096U, host.model().domain.request_policy.config().effective_limit());
    EXPECT_EQ("test-model", host.model().domain.request_policy.provider_model());
    EXPECT_EQ("test-provider", host.model().domain.request_policy.provider_name());
}

TEST(AsyncHostTest, PreparedNormalRequestCarriesTheExplicitOutputCap)
{
    std::atomic<std::uint64_t> observed_output_budget{0};
    std::binary_semaphore provider_called{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            observed_output_budget.store(
                request.max_output_tokens,
                std::memory_order_release);
            provider_called.release();
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        empty_runtime(),
    };
    my_agent::ContextManagementConfig management;
    management.configured_limit = 8192;
    management.safety_margin = 64;
    host.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{management, "provider", "test-model"}}});

    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "hello"}});
    static_cast<void>(host.drain_inbox());
    ASSERT_TRUE(provider_called.try_acquire_for(1s));
    static_cast<void>(host.drain_inbox());
    static_cast<void>(host.drain_inbox());

    EXPECT_EQ(
        management.normal_output_budget(),
        observed_output_budget.load(std::memory_order_acquire));
}

TEST(AsyncHostTest, CoreDraftStartsWithoutToolsOrPrompt)
{
    const my_agent::Request draft = my_agent::canonical_request(
        my_agent::Thread{});

    EXPECT_TRUE(draft.tools.empty());
    EXPECT_TRUE(draft.system_prompt.empty());
}

TEST(AsyncHostTest, PreparedStreamReceivesInjectedRuntimeProjection)
{
    std::optional<my_agent::Request> observed_request;
    std::binary_semaphore provider_called{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            observed_request = std::move(request);
            provider_called.release();
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        probe_runtime(),
    };
    host.set_system_prompt_provider([] {
        return std::string{"TS-004 system prompt"};
    });

    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "prepare this request"},
    });
    host.run_until_quiescent();

    ASSERT_TRUE(provider_called.try_acquire_for(1s));
    ASSERT_TRUE(observed_request.has_value());
    expect_probe_tool(*observed_request);
    EXPECT_EQ("TS-004 system prompt", observed_request->system_prompt);
}

TEST(AsyncHostTest, QueuedContinuationUsesTheSamePreparationProjection)
{
    RequestCapture capture;
    std::atomic_size_t stream_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            capture.add(std::move(request));
            if (stream_calls.fetch_add(1) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "calculator-call",
                    .name = "unregistered_projection_probe",
                    .args = nlohmann::json{
                        {"operation", "multiply"},
                        {"left", 6},
                        {"right", 7},
                    },
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        probe_runtime(),
    };

    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "continue after the tool"},
    });
    host.run_until_quiescent();

    const std::vector<my_agent::Request> requests = capture.snapshot();
    ASSERT_EQ(std::size_t{2}, requests.size());
    expect_probe_tool(requests[0]);
    expect_probe_tool(requests[1]);
}

TEST(AsyncHostTest, NativeCalculatorUsesSessionRuntimeAndContinues)
{
    const CalculatorHostRun run = run_native_calculator(
        nlohmann::json{
            {"operation", "multiply"},
            {"left", 6},
            {"right", 7},
        },
        "The result is 42.");

    ASSERT_EQ(2u, run.requests.size());
    const my_agent::Request& request = run.requests.front();
    constexpr std::array<std::string_view, 6> expected_names{
        "calculator", "read", "remember", "forget", "skill", "search_docs",
    };
    ASSERT_EQ(expected_names.size(), request.tools.size());
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        EXPECT_EQ(expected_names[i], request.tools[i].name);
    }
    const auto& calculator = request.tools.front();
    EXPECT_EQ("Multiply two numbers.", calculator.description);
    EXPECT_EQ("multiply",
              calculator.input_schema["properties"]["operation"]["enum"][0]);

    const my_agent::Request& continuation = run.requests.back();
    ASSERT_GE(continuation.messages.size(), 2u);
    const my_agent::Message& assistant_tool_call = continuation.messages[1];
    ASSERT_EQ(1u, assistant_tool_call.tool_calls.size());
    EXPECT_EQ("calculator", assistant_tool_call.tool_calls.front().name);
    EXPECT_EQ("42", assistant_tool_call.tool_calls.front().output());

    ASSERT_FALSE(run.model.domain.thread.messages.empty());
    EXPECT_EQ("The result is 42.", run.model.domain.thread.messages.back().text);
}

TEST(AsyncHostTest, NativeCalculatorInvalidArgsReachContinuation)
{
    const CalculatorHostRun run = run_native_calculator(
        nlohmann::json{
            {"operation", "divide"},
            {"left", 6},
            {"right", 7},
        },
        "The calculator rejected that operation.");

    ASSERT_EQ(2u, run.requests.size());
    ASSERT_GE(run.requests.back().messages.size(), 2u);
    const my_agent::Message& assistant_tool_call = run.requests.back().messages[1];
    ASSERT_EQ(1u, assistant_tool_call.tool_calls.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::ToolCall::Failed>(
        assistant_tool_call.tool_calls.front().status));
    EXPECT_EQ(
        "[invalid args] unsupported calculator operation: divide",
        assistant_tool_call.tool_calls.front().output());

    ASSERT_FALSE(run.model.domain.thread.messages.empty());
    EXPECT_EQ(
        "The calculator rejected that operation.",
        run.model.domain.thread.messages.back().text);
}

TEST(AsyncHostTest, SessionMemoryToolsRefreshContinuationPrompt)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / ("my_agent_ts009_async_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "skills");
    fs::create_directories(root / "docs");

    const my_agent::tool::SessionServices services{
        my_agent::tool::SessionFacts{
            .workspace_root = root / "workspace",
            .memory_roots = my_agent::tool::memory::Roots{
                .user = root / "memory-user",
                .project = root / "memory-project",
            },
            .skill_roots = {root / "skills"},
            .docs = my_agent::tool::docs::DocsConfig{
                .root = root / "docs",
                .root_missing = false,
            },
        }};
    auto runtime = std::make_shared<ToolRuntime>(services.registrations());
    RequestCapture capture;
    std::atomic_size_t provider_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            capture.add(std::move(request));
            const std::size_t call = provider_calls.fetch_add(1);
            if (call == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "remember-call",
                    .name = "remember",
                    .args = nlohmann::json{{"text", "prefers the fish shell"}},
                }});
            } else if (call == 1) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "forget-call",
                    .name = "forget",
                    .args = nlohmann::json{{"text", "prefers the fish shell"}},
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        runtime,
    };
    host.set_system_prompt_provider([&services] {
        return my_agent::prompt::build(my_agent::prompt::Context{
            .working_directory = services.workspace_root().string(),
            .operating_system = "test OS",
            .memories = services.memory_lines(),
            .skills_catalog = services.skill_catalog(),
        });
    });
    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "remember then forget"}});
    host.run_until_quiescent();

    const std::vector<my_agent::Request> requests = capture.snapshot();
    ASSERT_EQ(3u, requests.size());
    EXPECT_EQ(std::string::npos,
              requests[0].system_prompt.find("prefers the fish shell"));
    EXPECT_NE(std::string::npos,
              requests[1].system_prompt.find("prefers the fish shell"));
    EXPECT_EQ(std::string::npos,
              requests[2].system_prompt.find("prefers the fish shell"));
    EXPECT_TRUE(services.memory_lines().empty());

    std::error_code error;
    fs::remove_all(root, error);
}

TEST(AsyncHostTest, RetryUsesTheSamePreparationProjection)
{
    RequestCapture capture;
    std::atomic_size_t stream_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            capture.add(std::move(request));
            if (stream_calls.fetch_add(1) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamError{
                    .message = "retry this request",
                    .disposition = my_agent::FailureDisposition::RetryCurrent,
                }});
                return;
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        probe_runtime(),
    };

    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "retry this request"},
    });
    host.run_until_quiescent();

    const std::vector<my_agent::Request> requests = capture.snapshot();
    ASSERT_EQ(std::size_t{2}, requests.size());
    expect_probe_tool(requests[0]);
    expect_probe_tool(requests[1]);
}

TEST(AsyncHostTest, CompactionRequestDoesNotReceiveRuntimeTools)
{
    RequestCapture capture;
    std::atomic_size_t stream_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request request, my_agent::ProviderEventSink sink) {
            capture.add(std::move(request));
            if (stream_calls.fetch_add(1) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = "first answer",
                }});
            } else {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = R"({"goal":"","constraints":[],"facts":[],"decisions":[],"state":[],"next_steps":[]})",
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        },
        probe_runtime(),
    };
    install_context_management(host);

    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "build history"},
    });
    host.run_until_quiescent();
    host.dispatch(my_agent::Msg{my_agent::RequestCompaction{}});
    host.run_until_quiescent();

    const std::vector<my_agent::Request> requests = capture.snapshot();
    ASSERT_EQ(std::size_t{2}, requests.size());
    EXPECT_FALSE(requests[0].tools.empty());
    EXPECT_TRUE(requests[1].tools.empty());
}

TEST(AsyncHostTest, PreparationFailureBlocksWithoutCallingProvider)
{
    std::atomic_size_t provider_calls{0};
    my_agent::AsyncHost host{
        [&](my_agent::Request, my_agent::ProviderEventSink) {
            ++provider_calls;
        },
        probe_runtime(),
    };
    host.set_system_prompt_provider([]() -> std::string {
        throw std::runtime_error{"prompt unavailable"};
    });

    host.dispatch(my_agent::Msg{
        my_agent::Submit{.text = "must not escape the host"},
    });
    host.run_until_quiescent();

    EXPECT_EQ(std::size_t{0}, provider_calls.load());
    const auto* blocked = std::get_if<my_agent::Blocked>(
        &host.model().session.phase);
    ASSERT_NE(nullptr, blocked);
    EXPECT_EQ(my_agent::BlockReason::ExplicitResumeRequired, blocked->reason);
    EXPECT_NE(std::string::npos, blocked->message.find("prompt unavailable"));
    ASSERT_EQ(std::size_t{1}, host.model().session.queued_messages.size());
}

TEST(AsyncHostTest, InjectedToolsParticipateInRequestFingerprint)
{
    auto run = [](std::shared_ptr<const ToolRuntime> runtime) {
        RequestCapture capture;
        my_agent::AsyncHost host{
            [&capture](my_agent::Request request,
                       my_agent::ProviderEventSink sink) {
                capture.add(std::move(request));
                sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
            },
            std::move(runtime),
        };
        install_context_management(host);
        host.dispatch(my_agent::Msg{
            my_agent::Submit{.text = "fingerprint this"},
        });
        host.run_until_quiescent();
        const auto requests = capture.snapshot();
        EXPECT_EQ(1U, requests.size());
        return !requests.empty()
            ? my_agent::fingerprint_request("test-model", requests.front())
            : my_agent::RequestFingerprint{};
    };

    const my_agent::RequestFingerprint first = run(
        probe_runtime("fingerprint-a"));
    const my_agent::RequestFingerprint second = run(
        probe_runtime("fingerprint-b"));

    EXPECT_NE(first, second);
}
