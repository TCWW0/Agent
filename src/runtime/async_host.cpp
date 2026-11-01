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

Msg resolve_stream_event(
    ProviderEvent event,
    StreamRef ref,
    const RequestFingerprint& request_fingerprint,
    const tool::ToolRuntime& runtime)
{
    return std::visit(
        [ref, &request_fingerprint, &runtime](auto&& provider_event) -> Msg {
            using Event = std::decay_t<decltype(provider_event)>;
            if constexpr (std::is_same_v<Event, StreamToolCall>) {
                const auto resolution = runtime.resolve(provider_event.name);
                ToolCallResolution resolved = resolution
                    ? ToolCallResolution{*resolution}
                    : ToolCallResolution{resolution.error()};
                return Msg{ResolvedStreamToolCall{
                    ref,
                    std::move(provider_event.id),
                    std::move(provider_event.name),
                    std::move(provider_event.args),
                    std::move(resolved),
                }};
            } else if constexpr (std::is_same_v<Event, RequestUsage>) {
                provider_event.operation = ref;
                provider_event.request_fingerprint = request_fingerprint;
                return Msg{std::move(provider_event)};
            } else {
                provider_event.ref = ref;
                return Msg{std::move(provider_event)};
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
                event.operation = compaction_id;
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
    std::shared_ptr<const tool::ToolRuntime> runtime)
    : stream_(std::move(stream)),
      tool_runtime_(std::move(runtime)),
      wake_(std::make_shared<WakeSignal>()),
      inbox_(std::make_shared<InboxState>(wake_))
{
    if (!tool_runtime_) {
        throw std::invalid_argument{"AsyncHost requires a ToolRuntime"};
    }
}

std::shared_ptr<AsyncHost::OperationState> AsyncHost::begin_operation(
    OperationRef ref)
{
    invalidate_current_operation();
    auto state = std::make_shared<OperationState>();
    current_operation_ = CurrentOperation{
        .ref = std::move(ref),
        .state = state,
    };
    return state;
}

void AsyncHost::invalidate_current_operation() noexcept
{
    if (current_operation_) {
        current_operation_->state->cancelled.store(
            true, std::memory_order_release);
        current_operation_.reset();
    }
}

void AsyncHost::shutdown()
{
    pool_.request_stop();
    wake_->signal();
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
    Step step = update(std::move(current_model_), std::move(msg));
    current_model_ = std::move(step.model);
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
    invalidate_current_operation();
    try {
        PreparedRequestSnapshot snapshot = prepare_request_snapshot(
            std::move(command.request),
            tool_runtime_->request_tools(),
            system_prompt_provider_
                ? std::optional<std::string>{system_prompt_provider_()}
                : std::nullopt,
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
    const std::shared_ptr<OperationState> state =
        begin_operation(OperationRef{compaction_id});

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
    const StreamRef ref = command.ref;
    const std::shared_ptr<OperationState> state =
        begin_operation(OperationRef{ref});
    StreamEffect stream = stream_;
    std::shared_ptr<InboxState> inbox = inbox_;
    std::shared_ptr<const tool::ToolRuntime> tool_runtime = tool_runtime_;
    std::stop_token token = pool_.stop_token();
    const RequestFingerprint request_fingerprint = command.request_fingerprint;

    pool_.task(
        [
            stream = std::move(stream),
            token,
            inbox = std::move(inbox),
            tool_runtime,
            request = std::move(command.request),
            state,
            ref,
            request_fingerprint
        ]() mutable {
            const auto terminal_posted =
                std::make_shared<std::atomic_bool>(false);
            ProviderEventSink sink = [
                inbox,
                token,
                state,
                tool_runtime,
                ref,
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
                    std::move(event), ref,
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
    const std::shared_ptr<OperationState> state =
        begin_operation(OperationRef{command.call});

    std::shared_ptr<InboxState> inbox = inbox_;
    std::shared_ptr<const tool::ToolRuntime> tool_runtime = tool_runtime_;
    std::stop_token token = pool_.stop_token();

    pool_.task_isolated(
        [
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
                // ToolRuntime converts handler exceptions. This guard covers
                // errors in runtime invocation before a result exists.
                result = tool_runtime->execute(
                    command.ref, command.args, context);
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
