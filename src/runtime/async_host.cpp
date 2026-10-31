#include "my_agent/provider/provider.hpp"
#include "my_agent/runtime/async_host.hpp"
#include "my_agent/runtime/agent.hpp"
#include "my_agent/runtime/request_preparation.hpp"
#include "my_agent/runtime/identity.hpp"
#include "my_agent/runtime/msg.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace my_agent {

struct AsyncHost::InboxState {
    explicit InboxState(std::shared_ptr<WakeSignal> wake)
        : wake(std::move(wake))
    {
    }

    void post(Msg msg)
    {
        bool should_wake = false;
        {
            std::lock_guard<std::mutex> lock{mutex};
            should_wake = messages.empty();
            messages.push_back(std::move(msg));
        }

        if (should_wake && wake) {
            wake->signal();
        }
    }

    std::vector<Msg> drain()
    {
        std::vector<Msg> batch;
        {
            std::lock_guard<std::mutex> lock{mutex};
            batch.swap(messages);
        }
        return batch;
    }

    std::shared_ptr<WakeSignal> wake;
    std::mutex mutex;
    std::vector<Msg> messages;
};

struct AsyncHost::OperationState {
    std::atomic_bool cancelled{false};
};

namespace {

Msg bind_stream_identity(
    Msg msg, TurnId turn_id,
    StreamId stream_id, RequestFingerprint request_fingerprint)
{
    std::visit(
        [turn_id, stream_id, request_fingerprint = std::move(
            request_fingerprint)](auto& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, StreamTextDelta>
                || std::is_same_v<Event, StreamFinished>
                || std::is_same_v<Event, StreamError>) {
                event.turn_id = turn_id;
                event.stream_id = stream_id;
            } else if constexpr (std::is_same_v<Event, RequestUsage>) {
                event.operation = OperationRef{
                    .owner = turn_id,
                    .kind = OperationKind::Stream,
                    .value = stream_id.value,
                };
                event.request_fingerprint = request_fingerprint;
            }
        },
        msg);
    return msg;
}

Msg resolve_stream_event(
    ProviderEvent event, TurnId turn_id, StreamId stream_id,
    RequestFingerprint request_fingerprint,
    const tool::ToolRuntime& runtime)
{
    return std::visit(
        [turn_id, stream_id,
         request_fingerprint = std::move(request_fingerprint), &runtime]
        (auto&& provider_event) -> Msg {
            using Event = std::decay_t<decltype(provider_event)>;
            if constexpr (std::is_same_v<Event, StreamToolCall>) {
                provider_event.turn_id = turn_id;
                provider_event.stream_id = stream_id;
                const auto resolution = runtime.resolve(provider_event.name);
                ToolCallResolution resolved = resolution
                    ? ToolCallResolution{*resolution}
                    : ToolCallResolution{resolution.error()};
                return Msg{ResolvedStreamToolCall{
                    StreamRef{.turn_id = provider_event.turn_id, .stream_id = provider_event.stream_id},
                    std::move(provider_event.id),
                    std::move(provider_event.name),
                    std::move(provider_event.args),
                    std::move(resolved),
                }};
            } else {
                Msg msg{std::forward<Event>(provider_event)};
                return bind_stream_identity(std::move(msg), turn_id, stream_id, request_fingerprint);
            }
        },
        std::move(event)
    );
}
Msg bind_compaction_identity(
    Msg msg,
    CompactionId compaction_id,
    RequestFingerprint request_fingerprint)
{
    std::optional<Msg> bound;
    std::visit(
        [compaction_id, request_fingerprint = std::move(
            request_fingerprint), &bound](auto& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, StreamTextDelta>) {
                bound = Msg{CompactionDelta{
                    .compaction_id = compaction_id,
                    .text = std::move(event.text),
                }};
            } else if constexpr (std::is_same_v<Event, StreamFinished>) {
                bound = Msg{CompactionFinished{
                    .compaction_id = compaction_id,
                }};
            } else if constexpr (std::is_same_v<Event, StreamError>) {
                bound = Msg{CompactionError{
                    .compaction_id = compaction_id,
                    .message = std::move(event.message),
                }};
            } else if constexpr (std::is_same_v<Event, RequestUsage>) {
                event.operation = OperationRef{
                    .owner = compaction_id,
                    .kind = OperationKind::Compaction,
                    .value = compaction_id.value,
                };
                event.request_fingerprint = std::move(request_fingerprint);
                bound = Msg{std::move(event)};
            }
        },
        msg);
    return bound.value_or(Msg{CompactionError{
        .compaction_id = compaction_id,
        .message = "compaction provider emitted an unsupported event",
    }});
}

Msg bind_compaction_provider_event(
    ProviderEvent event,
    CompactionId compaction_id,
    RequestFingerprint request_fingerprint)
{
    return std::visit(
        [compaction_id, request_fingerprint = std::move(request_fingerprint)](auto&& provider_event) -> Msg {
            using Event = std::decay_t<decltype(provider_event)>;
            if constexpr (std::is_same_v<Event, StreamToolCall>) {
                return Msg{CompactionError{
                    .compaction_id = compaction_id,
                    .message =
                        "compaction provider emitted an unsupported tool call",
                }};
            } else {
                return bind_compaction_identity(
                    Msg{std::forward<Event>(provider_event)},
                    compaction_id,
                    request_fingerprint);
            }
        },
        std::move(event));
}

}  // namespace

AsyncHost::AsyncHost(
    StreamEffect stream,
    std::shared_ptr<const tool::ToolRuntime> runtime,
    ToolExecEffect execute_tool)
    : stream_(std::move(stream)),
      execute_tool_(std::move(execute_tool)),
      tool_runtime_(std::move(runtime)),
      wake_(std::make_shared<WakeSignal>()),
      inbox_(std::make_shared<InboxState>(wake_))
{
    if (!tool_runtime_) {
        throw std::invalid_argument{"AsyncHost requires a ToolRuntime"};
    }
}

std::shared_ptr<AsyncHost::OperationState> AsyncHost::begin_operation(
    OperationId operation)
{
    auto state = std::make_shared<OperationState>();
    std::lock_guard<std::mutex> lock{operations_mutex_};
    if (const auto it = operations_.find(operation); it != operations_.end()) {
        it->second->cancelled.store(true, std::memory_order_release);
    }
    operations_[operation] = state;
    return state;
}

void AsyncHost::cancel_operation(OperationId operation) noexcept
{
    std::lock_guard<std::mutex> lock{operations_mutex_};
    if (const auto it = operations_.find(operation); it != operations_.end()) {
        it->second->cancelled.store(true, std::memory_order_release);
    }
}

void AsyncHost::retire_operation(OperationId operation) noexcept
{
    std::lock_guard<std::mutex> lock{operations_mutex_};
    operations_.erase(operation);
}

void AsyncHost::shutdown()
{
    pool_.request_stop();
    std::lock_guard<std::mutex> lock{operations_mutex_};
    for (const auto& [operation, state] : operations_) {
        static_cast<void>(operation);
        state->cancelled.store(true, std::memory_order_release);
    }
}

AsyncHost::~AsyncHost()
{
    shutdown();
}

const Model& AsyncHost::model() const noexcept
{
    return current_model_;
}

void AsyncHost::dispatch(Msg msg)
{
    process_msg(std::move(msg));
}

bool AsyncHost::drain_inbox()
{
    std::vector<Msg> batch = inbox_->drain();
    for (Msg& msg : batch) {
        process_msg(std::move(msg));
    }
    return !batch.empty();
}

bool AsyncHost::is_quiescent() const noexcept
{
    return std::holds_alternative<Idle>(current_model_.phase)
        || std::holds_alternative<AwaitingPermission>(current_model_.phase)
        || std::holds_alternative<Blocked>(current_model_.phase);
}

void AsyncHost::run_until_quiescent()
{
    while (true) {
        if (pool_.stop_token().stop_requested()) {
            return;
        }

        static_cast<void>(drain_inbox());
        if (is_quiescent()) {
            return;
        }
        wake_->wait();
    }
}

bool AsyncHost::wait_wake(std::chrono::milliseconds timeout)
{
    return wake_->wait_for(timeout);
}

int AsyncHost::wake_fd() const noexcept
{
    return wake_->fd();
}

void AsyncHost::process_msg(Msg msg)
{
    std::visit(
        [this](const auto& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, StreamFinished>
                || std::is_same_v<Event, StreamError>) {
                if (event.turn_id.value != 0 || event.stream_id.value != 0) {
                    retire_operation(OperationId{
                        .turn_id = event.turn_id,
                        .kind = OperationKind::Stream,
                        .value = event.stream_id.value,
                    });
                }
            } else if constexpr (
                std::is_same_v<Event, CompactionFinished>
                || std::is_same_v<Event, CompactionError>) {
                if (event.compaction_id.value != 0) {
                    retire_operation(OperationId{
                        .turn_id = {},
                        .kind = OperationKind::Compaction,
                        .value = event.compaction_id.value,
                    });
                }
            }
        },
        msg);
    std::optional<ToolCallRef> reported_call;
    if (const auto* tool_report = std::get_if<ToolExecOutput>(&msg)) {
        reported_call = tool_report->call;
    }
    Step step = std::holds_alternative<SetRequestPolicy>(msg)
        ? update(current_model_, std::move(msg))
        : update(std::move(current_model_), std::move(msg));
    current_model_ = std::move(step.model);
    if (reported_call && reported_call->tool_exec_id.value != 0) {
        const OperationId reported_operation = tool_operation_id(*reported_call);
        const auto* executing_after =
            std::get_if<ExecutingTool>(&current_model_.phase);
        const bool operation_still_active = executing_after != nullptr
            && tool_operation_id(executing_after->call) == reported_operation;
        if (!operation_still_active) {
            retire_operation(reported_operation);
        }
    }
    std::visit(
        [this](auto&& command) {
            execute_cmd(std::move(command));
        },
        std::move(step.cmd));
}

void AsyncHost::execute_cmd(NoCommand)
{
}

void AsyncHost::set_system_prompt_provider(SystemPromptProvider provider)
{
    system_prompt_provider_ = std::move(provider);
}

void AsyncHost::execute_cmd(CancelOperation command)
{
    if (current_operation_ && current_operation_->ref == command.operation) {
        invalidate_current_operation();
    }
}

void AsyncHost::execute_cmd(PrepareRequest command)
{
    if (command.supersedes) {
        cancel_operation(*command.supersedes);
    }
    try {
        PreparedRequestSnapshot snapshot = prepare_request_snapshot(
            std::move(command.request),
            tool_runtime_->request_tools(),
            system_prompt_provider_
                ? std::optional<std::string>{system_prompt_provider_()}
                : std::nullopt,
            command.provider_name,
            command.provider_model,
            command.policy,
            command.ledger);

        inbox_->post(Msg{RequestPrepared{
            .preparation_id = command.preparation_id,
            .snapshot = std::move(snapshot),
        }});
    } catch (const std::exception& error) {
        inbox_->post(Msg{RequestPreparationFailed{
            .preparation_id = command.preparation_id,
            .message = std::string{"request preparation failed: "}
                + error.what(),
        }});
    } catch (...) {
        inbox_->post(Msg{RequestPreparationFailed{
            .preparation_id = command.preparation_id,
            .message = "request preparation failed: unknown error",
        }});
    }
}

void AsyncHost::execute_cmd(StartCompaction command)
{
    StreamEffect stream = stream_;
    std::shared_ptr<InboxState> inbox = inbox_;
    std::stop_token token = pool_.stop_token();
    const CompactionId compaction_id = command.compaction_id;
    const RequestFingerprint request_fingerprint = command.request_fingerprint;
    const OperationId operation{
        .turn_id = {},
        .kind = OperationKind::Compaction,
        .value = compaction_id.value,
    };
    const std::shared_ptr<OperationState> state = begin_operation(operation);

    pool_.task(
        [
            stream = std::move(stream),
            inbox = std::move(inbox),
            token,
            request = std::move(command.request),
            compaction_id,
            request_fingerprint,
            state
        ]() mutable {
            const auto terminal_posted =
                std::make_shared<std::atomic_bool>(false);
            ProviderEventSink sink = [
                inbox,
                token,
                compaction_id,
                request_fingerprint,
                terminal_posted,
                state
            ](ProviderEvent event) mutable {
                if (terminal_posted->load(std::memory_order_acquire)) {
                    return;
                }
                const bool terminal = std::holds_alternative<StreamFinished>(event)
                    || std::holds_alternative<StreamError>(event);
                if (terminal) {
                    bool expected = false;
                    if (!terminal_posted->compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel)) {
                        return;
                    }
                }
                if (token.stop_requested()) {
                    return;
                }
                if (state->cancelled.load(std::memory_order_acquire)) {
                    return;
                }
                inbox->post(bind_compaction_provider_event(
                    std::move(event), compaction_id, request_fingerprint));
            };

            try {
                stream(std::move(request), sink);
            } catch (const std::exception& error) {
                sink(ProviderEvent{StreamError{.message = error.what()}});
            } catch (...) {
                sink(ProviderEvent{StreamError{.message = "unknown compaction failure"}});
            }

            if (!terminal_posted->load(std::memory_order_acquire)) {
                sink(ProviderEvent{StreamError{.message = "compaction returned without a terminal event"}});
            }
        }
    );
}

void AsyncHost::execute_cmd(StartStream command)
{
    if (command.supersedes) {
        cancel_operation(*command.supersedes);
    }

    const OperationId operation{
        .turn_id = command.turn_id,
        .kind = OperationKind::Stream,
        .value = command.stream_id.value,
    };
    const std::shared_ptr<OperationState> state = begin_operation(operation);
    StreamEffect stream = stream_;
    std::shared_ptr<InboxState> inbox = inbox_;
    std::shared_ptr<const tool::ToolRuntime> tool_runtime = tool_runtime_;
    std::stop_token token = pool_.stop_token();
    const TurnId turn_id = command.turn_id;
    const StreamId stream_id = command.stream_id;
    const RequestFingerprint request_fingerprint = command.request_fingerprint;

    pool_.task(
        [
            stream = std::move(stream),
            token,
            inbox = std::move(inbox),
            tool_runtime,
            request = std::move(command.request),
            state,
            operation,
            turn_id,
            stream_id,
            request_fingerprint
        ]() mutable {
            const auto terminal_posted =
                std::make_shared<std::atomic_bool>(false);
            ProviderEventSink sink = [
                inbox,
                token,
                state,
                tool_runtime,
                turn_id,
                stream_id,
                request_fingerprint,
                terminal_posted
            ](ProviderEvent event) mutable {
                if (terminal_posted->load(std::memory_order_acquire)) {
                    return;
                }
                const bool terminal = std::holds_alternative<StreamFinished>(event)
                    || std::holds_alternative<StreamError>(event);
                if (terminal) {
                    bool expected = false;
                    if (!terminal_posted->compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        return;
                    }
                }
                if (token.stop_requested()
                    || state->cancelled.load(std::memory_order_acquire)) {
                    return;
                }
                inbox->post(resolve_stream_event(
                    std::move(event), turn_id, stream_id,
                    request_fingerprint, *tool_runtime));
            };

            try {
                stream(std::move(request), sink);
            } catch (const std::exception& error) {
                sink(ProviderEvent{StreamError{.message = error.what()}});
            } catch (...) {
                sink(ProviderEvent{StreamError{.message = "unknown stream failure"}});
            }

            if (!terminal_posted->load(std::memory_order_acquire)) {
                sink(ProviderEvent{StreamError{
                    .message = "stream returned without a terminal event",
                }});
            }
        }
    );
}

void AsyncHost::execute_cmd(RunTool command)
{
    const OperationId operation = tool_operation_id(command.call);

    const std::shared_ptr<OperationState> state = begin_operation(operation);

    std::shared_ptr<InboxState> inbox = inbox_;
    std::shared_ptr<const tool::ToolRuntime> tool_runtime = tool_runtime_;
    std::stop_token token = pool_.stop_token();

    pool_.task_isolated(
        [
            execute_tool = std::move(execute_tool),
            inbox = std::move(inbox),
            tool_runtime = std::move(tool_runtime),
            command = std::move(command),
            state,
            token
        ]() mutable {
            const auto cancelled = [&] {
                return token.stop_requested()
                    || state->cancelled.load(std::memory_order_acquire);
            };

            if (cancelled()) {
                return;
            }

                const tool::ToolExecutionContext context{
                .cancelled = cancelled,
            };
            tool::ExecResult result;
            try {
                if (execute_tool) {
                    result = execute_tool(command.ref, command.args, context);
                } else {
                    result = tool_runtime->execute(
                        command.ref, command.args, context);
                }
            } catch (const std::exception& error) {
                result = std::unexpected(tool::ToolError{
                    .kind = tool::ErrorKind::ExecutionFailed,
                    .message = error.what(),
                });
            } catch (...) {
                result = std::unexpected(tool::ToolError{
                    .kind = tool::ErrorKind::ExecutionFailed,
                    .message = "unknown tool failure",
                });
            }

            if (cancelled()) {
                return;
            }

            inbox->post(Msg{
                ToolExecOutput{
                    .call = command.call,
                    .ref = command.ref,
                    .result = std::move(result),
                }
            });
        }
    );
}

}  // namespace my_agent
