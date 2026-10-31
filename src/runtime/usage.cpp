#include "my_agent/runtime/usage.hpp"
#include "my_agent/runtime/msg.hpp"

namespace my_agent {
namespace {

UsageTotals normalized_totals(const RequestUsage& event)
{
    const auto read = event.cache_read_input_tokens.value_or(0);
    const auto creation = event.cache_creation_input_tokens.value_or(0);
    return UsageTotals{
        .input_tokens = event.input_tokens,
        .cached_input_tokens = event.cached_input_tokens.value_or(read + creation),
        .output_tokens = event.output_tokens,
        .cache_read_input_tokens = read,
        .cache_creation_input_tokens = creation,
    };
}

void accumulate(UsageTotals& target, const UsageTotals& source)
{
    target.input_tokens += source.input_tokens;
    target.cached_input_tokens += source.cached_input_tokens;
    target.output_tokens += source.output_tokens;
    target.cache_read_input_tokens += source.cache_read_input_tokens;
    target.cache_creation_input_tokens += source.cache_creation_input_tokens;
}

} // namespace

void UsageState::begin_turn(TurnId id)
{
    turn = TurnUsageState{.turn_id = id};
}

void UsageState::record_accepted(const RequestUsage& event)
{
    const auto* stream = std::get_if<StreamRef>(&event.operation);
    const bool compaction = std::holds_alternative<CompactionId>(event.operation);
    if (!stream && !compaction) {
        return;
    }
    const auto totals = normalized_totals(event);
    accumulate(session, totals);
    ++request_count;
    if (compaction) {
        accumulate(compaction_session, totals);
        return;
    }
    if (!turn || turn->turn_id != stream->turn_id) {
        begin_turn(stream->turn_id);
    }
    accumulate(turn->totals, totals);
    latest_request = UsageObservation{
        .operation = event.operation,
        .request_fingerprint = event.request_fingerprint,
        .totals = totals,
        .quality = event.quality,
    };
    observed_context = latest_request;
}

void UsageState::mark_request_unavailable(StreamRef ref, const RequestFingerprint& fingerprint)
{
    latest_request = UsageObservation{
        .operation = ref,
        .request_fingerprint = fingerprint,
        .quality = UsageQuality::Unavailable,
    };
    if (!turn || turn->turn_id != ref.turn_id) {
        begin_turn(ref.turn_id);
    }
    if (observed_context) {
        observed_context->quality = UsageQuality::Stale;
    }
}

void UsageState::mark_context_pending()
{
    if (observed_context && observed_context->quality == UsageQuality::Exact) {
        observed_context->quality = UsageQuality::Last;
    }
}

UsageQuality UsageState::current_turn_quality() const noexcept
{
    if (turn && latest_request) {
        if (const auto* ref = std::get_if<StreamRef>(&latest_request->operation);
            ref && ref->turn_id == turn->turn_id) {
            return latest_request->quality;
        }
    }
    return UsageQuality::Unavailable;
}

} // namespace my_agent
