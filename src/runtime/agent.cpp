#include "my_agent/runtime/agent.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/compaction.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/identity.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/runtime/msg.hpp"
#include "my_agent/tool/policy.hpp"
#include "my_agent/tool/tool_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace my_agent {
namespace {

Message* message_for(Model& model, MessageId id)
{
    if (id.value == 0) {
        return nullptr;
    }
    for (Message& message : model.domain.thread.messages) {
        if (message.id == id) {
            return &message;
        }
    }
    return nullptr;
}

bool stream_event_matches(const Streaming& active, StreamRef ref)
{
    return is_valid(ref) && active.ref == ref;
}

Cmd drive(Model& model);
void block_context_limit(
    Model& model,
    std::string message,
    std::optional<ContextBlockDetail> detail =
        ContextBlockDetail::RequestTooLarge,
    Message* assistant = nullptr,
    std::optional<ContextBudgetAssessment> budget = {});

ContextBlockDetail detail_for_plan_failure(
    CompactionPlanFailureKind kind)
{
    return kind == CompactionPlanFailureKind::NoCompleteHistory
        ? ContextBlockDetail::NoCompleteHistory
        : ContextBlockDetail::CompactionSourceTooLarge;
}

ContextBlockDetail detail_for_validation_failure(
    CompactionFailureKind kind)
{
    return kind == CompactionFailureKind::Projection
            || kind == CompactionFailureKind::NoGain
        ? ContextBlockDetail::CompactionInsufficient
        : ContextBlockDetail::CompactionFailed;
}

TurnId intent_turn(const RequestIntent& intent)
{
    return std::visit([](const auto& request) { return request.turn_id; }, intent);
}

std::optional<MessageId> intent_assistant(const RequestIntent& intent)
{
    return std::visit([](const auto& request) -> std::optional<MessageId> {
        using Intent = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<Intent, ToolContinuationRequest>) {
            return request.previous_assistant_id;
        } else if constexpr (std::is_same_v<Intent, RetryRequest>) {
            return request.target_assistant_id;
        } else {
            return std::nullopt;
        }
    }, intent);
}

std::optional<Request> request_for_intent(const Model& model, const RequestIntent& intent)
{
    Request request = canonical_request(model.domain.thread);
    if (const auto* queued = std::get_if<QueuedTurnRequest>(&intent)) {
        if (model.session.queued_messages.empty()) {
            return std::nullopt;
        }
        const auto& head = model.session.queued_messages.front();
        if (head.turn_id != queued->turn_id || head.message_id != queued->queued_message_id) {
            return std::nullopt;
        }
        request.messages.push_back(Message{
            .role = Role::User, .text = head.text,
            .id = head.message_id, .turn_id = head.turn_id,
        });
    }
    return request;
}

Cmd start_request_preparation(Model& model, RequestIntent intent)
{
    auto request = request_for_intent(model, intent);
    if (!request) {
        model.phase = Idle{};
        return drive(model);
    }
    const PreparationId preparation_id{
        model.session.ids.next_preparation++};
    model.phase = PreparingRequest{.preparation_id = preparation_id, .intent = std::move(intent)};
    return PrepareRequest{
        .preparation_id = preparation_id,
        .request = std::move(*request),
        .policy = model.domain.request_policy,
        .ledger = model.session.projection_ledger,
    };
}

Cmd start_compaction(
    Model& model,
    CompactionPlan plan)
{
    const RequestFingerprint request_fingerprint = fingerprint_request(
        plan.policy.provider_model(),
        plan.compaction_request);
    model.phase = Compacting{
        .plan = plan,
    };
    return StartCompaction{
        .compaction_id = plan.compaction_id,
        .request = std::move(plan.compaction_request),
        .request_fingerprint = request_fingerprint,
    };
}

Cmd finish_compaction_failure(
    Model& model,
    const Compacting& active,
    ContextBlockDetail detail)
{
    model.session.last_failed_compaction = FailedCompactionAttempt{
        .source_digest = active.plan.source_digest,
        .detail = detail,
    };
    model.phase = Idle{};
    return drive(model);
}

Cmd drive(Model& model)
{
    if (!std::holds_alternative<Idle>(model.phase)
        || model.session.queued_messages.empty()) {
        return NoCommand{};
    }

    const auto& item = model.session.queued_messages.front();
    return start_request_preparation(model, QueuedTurnRequest{
        .turn_id = item.turn_id, .queued_message_id = item.message_id,
    });
}

Cmd finish_turn(Model& model)
{
    model.phase = Idle{};
    return drive(model);
}

std::optional<MessageId> active_assistant(const Model& model)
{
    return std::visit(
        [](const auto& phase) -> std::optional<MessageId> {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, Streaming>) {
                return phase.target_message_id;
            } else if constexpr (std::is_same_v<Phase, AwaitingPermission>) {
                return phase.call.assistant_message_id;
            } else if constexpr (std::is_same_v<Phase, ExecutingTool>) {
                return phase.call.assistant_message_id;
            } else {
                return std::nullopt;
            }
        },
        model.phase);
}

Cmd start_continuation(Model& model, TurnId turn_id)
{
    for (auto it = model.domain.thread.messages.rbegin(); it != model.domain.thread.messages.rend(); ++it) {
        if (it->id.value != 0 && it->turn_id == turn_id && it->role == Role::Assistant) {
            return start_request_preparation(model, ToolContinuationRequest{
                .turn_id = turn_id, .previous_assistant_id = it->id,
            });
        }
    }
    return finish_turn(model);
}

void block_context_limit(
    Model& model,
    std::string message,
    std::optional<ContextBlockDetail> detail,
    Message* assistant,
    std::optional<ContextBudgetAssessment> budget)
{
    if (assistant != nullptr) {
        assistant->error = message;
    }
    model.phase = Blocked{
        .reason = BlockReason::ContextLimit,
        .context_detail = detail,
        .budget = std::move(budget),
        .message = std::move(message),
    };
}

bool rollback_empty_queued_turn(
    Model& model,
    const Streaming& active)
{
    if (!active.rollback_user_message) {
        return false;
    }
    Message* assistant = message_for(model, active.target_message_id);
    if (assistant == nullptr
        || !assistant->text.empty()
        || !assistant->tool_calls.empty()) {
        return false;
    }
    auto assistant_position = model.domain.thread.messages.end();
    for (auto it = model.domain.thread.messages.begin();
         it != model.domain.thread.messages.end(); ++it) {
        if (it->id == active.target_message_id) {
            assistant_position = it;
            break;
        }
    }
    if (assistant_position == model.domain.thread.messages.end()
        || assistant_position == model.domain.thread.messages.begin()
        || std::next(assistant_position) != model.domain.thread.messages.end()) {
        return false;
    }
    auto user_position = std::prev(assistant_position);
    if (user_position->role != Role::User
        || user_position->id != *active.rollback_user_message
        || user_position->turn_id != active.ref.turn_id) {
        return false;
    }
    const QueuedMessage item{
        .turn_id = user_position->turn_id,
        .message_id = user_position->id,
        .text = user_position->text,
    };
    model.domain.thread.messages.erase(user_position, model.domain.thread.messages.end());
    model.session.queued_messages.push_front(item);
    return true;
}

Cmd kick_pending_tools(Model& model)
{
    const std::optional<MessageId> assistant_id = active_assistant(model);
    if (!assistant_id) {
        return NoCommand{};
    }
    Message* assistant = message_for(model, *assistant_id);
    if (assistant == nullptr || assistant->role != Role::Assistant) {
        return NoCommand{};
    }

    for (ToolCall& toolcall : assistant->tool_calls) {
        if (!toolcall.is_pending()) {
            continue;
        }
        const auto* pending = std::get_if<ToolCall::Pending>(&toolcall.status);
        if (pending == nullptr || pending->binding.ref.value == 0) {
            model.phase = Blocked{
                .reason = BlockReason::InvalidConfiguration,
                .message = "pending tool call has no resolved binding",
            };
            return NoCommand{};
        }

        const bool need_permission = tool::policy::permission(
            pending->binding.effects, model.domain.profile) == tool::policy::Decision::Prompt;

        if (need_permission) {
            model.phase = AwaitingPermission{
                .call = ToolCallRef{
                    .assistant_message_id = assistant->id,
                    .tool_exec_id = toolcall.tool_exec_id,
                },
            };
            return NoCommand{};
        }

        const ToolCallRef call_ref{
            .assistant_message_id = assistant->id,
            .tool_exec_id = toolcall.tool_exec_id,
        };
        model.phase = ExecutingTool{
            .call = call_ref,
        };
        return RunTool{
            .call = call_ref,
            .ref = pending->binding.ref,
            .args = toolcall.args,
        };
    }

    if (assistant->tool_calls.empty()) {
        return finish_turn(model);
    }
    return start_continuation(model, assistant->turn_id);
}

Cmd apply(Model& model, const Submit& submit)
{
    const TurnId turn_id{model.session.ids.next_turn++};
    const MessageId message_id{model.session.ids.next_message++};
    model.session.queued_messages.push_back(QueuedMessage{
        .turn_id = turn_id,
        .message_id = message_id,
        .text = submit.text,
    });
    return drive(model);
}

Cmd apply(Model& model, const RequestCompaction&)
{
    if (!std::holds_alternative<Idle>(model.phase)
        || !model.session.queued_messages.empty()
        ) {
        return NoCommand{};
    }

    Request next_request = canonical_request(model.domain.thread);
    const auto planned = make_compaction_plan(
        CompactionId{model.session.ids.next_compaction++},
        model.domain.thread,
        next_request,
        model.domain.request_policy);
    if (!planned) {
        const ContextBlockDetail detail = detail_for_plan_failure(
            planned.error().kind);
        model.session.last_failed_compaction = FailedCompactionAttempt{
            .source_digest = planned.error().source_digest,
            .detail = detail,
        };
        return NoCommand{};
    }
    return start_compaction(
        model, *planned);
}

Cmd apply(Model& model, const StreamTextDelta& delta)
{
    const auto* active = std::get_if<Streaming>(&model.phase);
    if (active == nullptr
        || !stream_event_matches(*active, delta.ref)) {
        return NoCommand{};
    }

    Message* assistant = message_for(model, active->target_message_id);
    if (assistant == nullptr || assistant->role != Role::Assistant) {
        return NoCommand{};
    }
    assistant->text += delta.text;
    if (!delta.text.empty()) {
        model.session.stream_telemetry.output_bytes += delta.text.size();
        ++model.session.stream_telemetry.generation;
    }
    return NoCommand{};
}

Cmd apply(Model& model, const ResolvedStreamToolCall& event)
{
    const auto* active = std::get_if<Streaming>(&model.phase);
    if (active == nullptr || !stream_event_matches(*active, event.ref)){
        return NoCommand{};
    }
    Message* assistant = message_for(model, active->target_message_id);
    if (assistant == nullptr || assistant->role != Role::Assistant) {
        return NoCommand{};
    }
    for (const ToolCall& existing : assistant->tool_calls) {
        if (existing.id == event.id) {
            return NoCommand{};
        }
    }

    const ToolExecId tool_exec_id{model.session.ids.next_tool_exec++};
    ToolCall call{
        .id = event.id,
        .name = event.name,
        .args = event.args,
        .status = ToolCall::Pending{},
        .tool_exec_id = tool_exec_id,
    };
    bool unresolved = false;
    std::visit(
        [&call, &unresolved](const auto& resolution) {
            using Resolution = std::decay_t<decltype(resolution)>;
            if constexpr (std::is_same_v<Resolution, tool::ToolBinding>) {
                call.status = ToolCall::Pending{.binding = resolution};
            } else if constexpr (
                std::is_same_v<Resolution, tool::ToolResolutionError>) {
                call.status = ToolCall::Failed{.output = resolution.message};
            } else {
                unresolved = true;
                call.status = ToolCall::Failed{
                    .output = "tool resolution boundary produced no result: "
                        + call.name,
                };
            }
        },
        event.resolution);
    assistant->tool_calls.push_back(std::move(call));

    const std::size_t argument_bytes = event.args.dump().size();
    if (argument_bytes > 0){
        model.session.stream_telemetry.output_bytes += argument_bytes;
        ++model.session.stream_telemetry.generation;
    }
    if (unresolved) {
        model.phase = Blocked{
            .reason = BlockReason::InvalidConfiguration,
            .message = "tool resolution boundary produced no result: " + event.name,
        };
    }
    return NoCommand{};
}

Cmd apply(Model& model, const StreamFinished& event)
{
    auto* active = std::get_if<Streaming>(&model.phase);
    if (active == nullptr
        || !stream_event_matches(*active, event.ref)) {
        return NoCommand{};
    }
    if (!active->usage_accepted) {
        model.session.usage.mark_request_unavailable(active->ref, active->projection_receipt.fingerprint);
        active->usage_accepted = true;
    }
    return kick_pending_tools(model);
}

Cmd apply(Model& model, const RequestUsage& event)
{
    if (auto* compacting = std::get_if<Compacting>(&model.phase);
        compacting != nullptr) {
        if (compacting->plan.compaction_id.value == 0
            || compacting->usage_accepted
            || event.operation != OperationRef{compacting->plan.compaction_id}
            || event.request_fingerprint
                != fingerprint_request(
                    compacting->plan.policy.provider_model(),
                    compacting->plan.compaction_request)) {
            return NoCommand{};
        }

        model.session.usage.record_accepted(event);
        compacting->usage_accepted = true;
        return NoCommand{};
    }

    auto* active = std::get_if<Streaming>(&model.phase);
    if (active == nullptr
        || !is_valid(active->ref)
        || active->usage_accepted
        || event.operation != OperationRef{active->ref}
        || event.request_fingerprint != active->projection_receipt.fingerprint) {
        return NoCommand{};
    }

    active->usage_accepted = true;
    if (event.quality == UsageQuality::Exact
        && !active->projection_receipt.segments.empty()) {
        record_exact_usage(
            model.session.projection_ledger,
            active->projection_receipt,
            event.input_tokens,
            event.output_tokens);
    }
    model.session.usage.record_accepted(event);
    return NoCommand{};
}

Cmd apply(Model& model, const RequestPreparationFailed& event)
{
    const auto* preparing = std::get_if<PreparingRequest>(&model.phase);
    if (preparing == nullptr
        || event.preparation_id.value == 0
        || preparing->preparation_id != event.preparation_id) {
        return NoCommand{};
    }

    const PreparingRequest active = *preparing;
    if (std::holds_alternative<QueuedTurnRequest>(active.intent)) {
        model.phase = Blocked{
            .reason = BlockReason::ExplicitResumeRequired,
            .message = event.message,
        };
        return NoCommand{};
    }
    if (const auto id = intent_assistant(active.intent)) {
        if (auto* assistant = message_for(model, *id)) {
            assistant->error = event.message;
        }
    }
    model.phase = Idle{};
    return drive(model);
}

Cmd accept_prepared_request(Model& model, const PreparingRequest& active,
                            const PreparedRequestSnapshot& snapshot)
{
    const TurnId turn_id = intent_turn(active.intent);
    std::optional<MessageId> rollback_user_message;
    if (const auto* queued = std::get_if<QueuedTurnRequest>(&active.intent)) {
        auto item = std::move(model.session.queued_messages.front());
        model.session.queued_messages.pop_front();
        model.domain.thread.messages.push_back(Message{
            .role = Role::User, .text = std::move(item.text),
            .id = item.message_id, .turn_id = item.turn_id,
        });
        model.session.usage.begin_turn(item.turn_id);
        rollback_user_message = queued->queued_message_id;
    }
    MessageId assistant_id;
    if (const auto* retry = std::get_if<RetryRequest>(&active.intent)) {
        assistant_id = retry->target_assistant_id;
    } else {
        assistant_id = MessageId{model.session.ids.next_message++};
        model.domain.thread.messages.push_back(Message{
            .role = Role::Assistant, .text = {}, .id = assistant_id, .turn_id = turn_id,
        });
    }
    const StreamRef ref{
        .turn_id = turn_id,
        .stream_id = StreamId{model.session.ids.next_stream++},
    };
    model.phase = Streaming{
        .ref = ref, .target_message_id = assistant_id,
        .projection_receipt = snapshot.receipt,
        .rollback_user_message = rollback_user_message,
    };
    model.session.usage.mark_context_pending();
    model.session.stream_telemetry = {};
    return StartStream{
        .ref = ref,
        .request = snapshot.request, .request_fingerprint = snapshot.receipt.fingerprint,
    };
}

Cmd apply(Model& model, const RequestPrepared& event)
{
    const auto* preparing = std::get_if<PreparingRequest>(&model.phase);
    if (preparing == nullptr
        || event.preparation_id.value == 0
        || preparing->preparation_id != event.preparation_id) {
        return NoCommand{};
    }

    const PreparingRequest active = *preparing;
    if (!request_for_intent(model, active.intent)) {
        return NoCommand{};
    }
    const ContextBudgetAssessment& budget = event.snapshot.budget;
    if (std::holds_alternative<QueuedTurnRequest>(active.intent)
        && (budget.triggered || !budget.admitted)) {
        const auto planned = make_compaction_plan(
            CompactionId{model.session.ids.next_compaction++},
            model.domain.thread,
            event.snapshot.request,
            model.domain.request_policy,
            budget);
        if (planned) {
            const bool same_failed_source =
                model.session.last_failed_compaction
                && model.session.last_failed_compaction->source_digest
                && *model.session.last_failed_compaction->source_digest
                    == planned->source_digest;
            if (!same_failed_source) {
                return start_compaction(
                    model, *planned);
            }
            if (!budget.admitted) {
                block_context_limit(
                    model,
                    "automatic context compaction is suppressed for the same failed source",
                    model.session.last_failed_compaction->detail,
                    nullptr,
                    budget);
                return NoCommand{};
            }
        } else {
            const ContextBlockDetail detail = detail_for_plan_failure(
                planned.error().kind);
            if (planned.error().source_digest) {
                model.session.last_failed_compaction = FailedCompactionAttempt{
                    .source_digest = planned.error().source_digest,
                    .detail = detail,
                };
            }
            if (!budget.admitted) {
                block_context_limit(
                    model,
                    planned.error().message,
                    detail,
                    nullptr,
                    budget);
                return NoCommand{};
            }
        }
    }

    if (!budget.admitted) {
        if (const auto id = intent_assistant(active.intent)) {
            if (auto* assistant = message_for(model, *id)) {
                assistant->error = "context limit exceeded before request";
            }
        }
        model.phase = Blocked{
            .reason = BlockReason::ContextLimit,
            .context_detail = ContextBlockDetail::RequestTooLarge,
            .budget = budget,
            .message = "request exceeds the available context budget",
        };
        return NoCommand{};
    }

    return accept_prepared_request(model, active, event.snapshot);
}

Cmd apply(Model& model, const CompactionDelta& event)
{
    auto* active = std::get_if<Compacting>(&model.phase);
    if (active == nullptr
        || event.compaction_id.value == 0
        || active->plan.compaction_id != event.compaction_id) {
        return NoCommand{};
    }
    active->pending_summary += event.text;
    return NoCommand{};
}

Cmd apply(Model& model, const CompactionFinished& event)
{
    auto* active = std::get_if<Compacting>(&model.phase);
    if (active == nullptr
        || event.compaction_id.value == 0
        || active->plan.compaction_id != event.compaction_id) {
        return NoCommand{};
    }

    const auto validation = validate_compaction(
        active->plan,
        active->pending_summary);
    if (!validation) {
        return finish_compaction_failure(
            model,
            *active,
            detail_for_validation_failure(validation.error().kind));
    }

    model.domain.thread.accepted_compaction = *validation;
    model.session.last_failed_compaction.reset();
    model.phase = Idle{};
    return drive(model);
}

Cmd apply(Model& model, const CompactionError& event)
{
    const auto* active = std::get_if<Compacting>(&model.phase);
    if (active == nullptr
        || event.compaction_id.value == 0
        || active->plan.compaction_id != event.compaction_id) {
        return NoCommand{};
    }
    return finish_compaction_failure(
        model,
        *active,
        ContextBlockDetail::CompactionFailed);
}

Cmd apply(Model& model, const StreamError& error)
{
    const auto* active = std::get_if<Streaming>(&model.phase);
    if (active == nullptr) {
        return NoCommand{};
    }

    const bool provider_context_limit = error.provider_error
        && error.provider_error->kind == ProviderErrorKind::ContextLimit;
    if (!stream_event_matches(*active, error.ref)) {
        return NoCommand{};
    }

    Message* assistant = message_for(model, active->target_message_id);
    if (assistant == nullptr || assistant->role != Role::Assistant) {
        return NoCommand{};
    }

    if (!active->usage_accepted) {
        model.session.usage.mark_request_unavailable(active->ref, active->projection_receipt.fingerprint);
    }

    if (provider_context_limit) {
        const bool rolled_back = rollback_empty_queued_turn(
            model, *active);
        block_context_limit(
            model,
            error.message,
            ContextBlockDetail::RequestTooLarge,
            rolled_back ? nullptr : assistant);
        return NoCommand{};
    }

    if (error.disposition == FailureDisposition::RetryCurrent) {
        return start_request_preparation(model, RetryRequest{
            .turn_id = active->ref.turn_id, .target_assistant_id = active->target_message_id,
        });
    }

    assistant->error = error.message;
    if (error.disposition == FailureDisposition::BlockSession) {
        model.phase = Blocked{
            .reason = error.block_reason,
            .message = error.message,
        };
        return NoCommand{};
    }
    return finish_turn(model);
}

Cmd apply(Model& model, const ToolExecOutput& event)
{
    const auto* active = std::get_if<ExecutingTool>(&model.phase);
    if (active == nullptr
        || !is_valid(event.call)
        || active->call != event.call) {
        return NoCommand{};
    }

    ToolCall* matched_call = resolve_tool_call(model.domain.thread, event.call);
    if (matched_call == nullptr || !matched_call->is_pending()) {
        return NoCommand{};
    }
    const auto* pending = std::get_if<ToolCall::Pending>(&matched_call->status);
    if (pending == nullptr || pending->binding.ref != event.ref) {
        return NoCommand{};
    }

    if (event.result) {
        matched_call->status = ToolCall::Done{
            .output = event.result->text,
        };
    } else {
        matched_call->status = ToolCall::Failed{
            .output = event.result.error().render(),
        };
    }
    return kick_pending_tools(model);
}

Cmd apply(Model& model, const SetProfile& event)
{
    model.domain.profile = event.profile;
    return NoCommand{};
}

Cmd apply(Model& model, const SetRequestPolicy& event)
{
    model.domain.request_policy = event.policy;
    if (const auto* preparing = std::get_if<PreparingRequest>(&model.phase)) {
        return start_request_preparation(model, preparing->intent);
    }
    return NoCommand{};
}

Cmd apply(Model& model, const PermissionApprove& event)
{
    const auto* active = std::get_if<AwaitingPermission>(&model.phase);
    if (active == nullptr
        || !is_valid(event.call)
        || active->call != event.call) {
        return NoCommand{};
    }

    ToolCall* toolcall = resolve_tool_call(model.domain.thread, event.call);
    if (toolcall == nullptr || !toolcall->is_pending()) {
        return NoCommand{};
    }
    const auto& pending = std::get<ToolCall::Pending>(toolcall->status);
    if (pending.binding.ref.value == 0) {
        return NoCommand{};
    }
    model.phase = ExecutingTool{
        .call = event.call,
    };
    return RunTool{
        .call = event.call,
        .ref = pending.binding.ref,
        .args = toolcall->args,
    };
}

Cmd apply(Model& model, const PermissionReject& event)
{
    const auto* active = std::get_if<AwaitingPermission>(&model.phase);
    if (active == nullptr
        || !is_valid(event.call)
        || active->call != event.call) {
        return NoCommand{};
    }

    ToolCall* toolcall = resolve_tool_call(model.domain.thread, event.call);
    if (toolcall == nullptr || !toolcall->is_pending()) {
        return NoCommand{};
    }

    std::string output{"User rejected this tool call."};
    if (event.feedback && !event.feedback->empty()) {
        output += " Feedback: ";
        output += *event.feedback;
    }
    toolcall->status = ToolCall::Rejected{.output = std::move(output)};
    return kick_pending_tools(model);
}

Cmd apply(Model& model, const CancelActive&)
{
    std::optional<OperationRef> operation;
    MessageId assistant_id{};
    bool active = false;

    std::visit(
        [&](const auto& phase) {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, Streaming>) {
                active = true;
                operation = phase.ref;
                assistant_id = phase.target_message_id;
            } else if constexpr (std::is_same_v<Phase, AwaitingPermission>) {
                active = true;
                assistant_id = phase.call.assistant_message_id;
            } else if constexpr (std::is_same_v<Phase, ExecutingTool>) {
                active = true;
                operation = phase.call;
                assistant_id = phase.call.assistant_message_id;
            } else if constexpr (std::is_same_v<Phase, Compacting>) {
                active = true;
                operation = phase.plan.compaction_id;
            }
        },
        model.phase);

    if (!active) {
        return NoCommand{};
    }

    if (const auto* compacting = std::get_if<Compacting>(&model.phase);
        compacting != nullptr) {
        Cmd next = finish_compaction_failure(
            model,
            *compacting,
            ContextBlockDetail::CompactionFailed);
        if (std::holds_alternative<NoCommand>(next) && operation) {
            return CancelOperation{.operation = *operation};
        }
        return next;
    }

    if (const auto* streaming = std::get_if<Streaming>(&model.phase);
        streaming != nullptr && !streaming->usage_accepted) {
        model.session.usage.mark_request_unavailable(streaming->ref, streaming->projection_receipt.fingerprint);
    }

    if (Message* assistant = message_for(model, assistant_id)) {
        assistant->error = "cancelled";
        constexpr std::string_view cancelled_output =
            "Tool execution was cancelled before a result was available.";
        for (ToolCall& call : assistant->tool_calls) {
            if (call.is_pending()) {
                call.status = ToolCall::Cancelled{
                    .output = std::string{cancelled_output},
                };
            }
        }
    }
    model.phase = Idle{};
    if (!model.session.queued_messages.empty()) {
        return drive(model);
    }
    if (operation) {
        return CancelOperation{.operation = *operation};
    }
    return NoCommand{};
}

Cmd apply(Model& model, const ResumeQueue&)
{
    if (std::holds_alternative<Blocked>(model.phase)) {
        model.phase = Idle{};
    }
    return drive(model);
}

Cmd apply(Model& model, const ClearQueue&)
{
    if (model.session.queued_messages.empty()) {
        return NoCommand{};
    }

    model.session.queued_messages.clear();

    // A queued-turn preparation owns no background provider operation. Moving
    // away from its identity is enough to make the already-posted prepared
    // event stale when the owner thread next drains the inbox.
    if (const auto* preparing = std::get_if<PreparingRequest>(&model.phase);
        preparing != nullptr
        && (std::holds_alternative<QueuedTurnRequest>(preparing->intent)
            || std::holds_alternative<RetryRequest>(preparing->intent))) {
        model.phase = Idle{};
        return NoCommand{};
    }

    // Compaction is private work performed solely for the queued head. Once
    // that head is dropped, invalidate the operation without committing its
    // pending summary or disturbing the last accepted compaction record.
    if (const auto* compacting = std::get_if<Compacting>(&model.phase);
        compacting != nullptr) {
        const OperationRef operation = compacting->plan.compaction_id;
        model.phase = Idle{};
        return CancelOperation{.operation = operation};
    }

    if (const auto* blocked = std::get_if<Blocked>(&model.phase);
        blocked != nullptr && blocked->reason == BlockReason::ContextLimit) {
        model.phase = Idle{};
    }
    return NoCommand{};
}

}  // namespace

Step update(Model model, Msg msg)
{
    Cmd cmd = std::visit(
        [&model](const auto& event) -> Cmd {
            return apply(model, event);
        },
        msg);
    return Step{
        .model = std::move(model),
        .cmd = std::move(cmd),
    };
}

}  // namespace my_agent
