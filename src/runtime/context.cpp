#include "my_agent/runtime/context_admission.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace my_agent {

RequestPolicy::RequestPolicy(
    ContextManagementConfig config, std::string provider_name, std::string provider_model)
    : config_(std::move(config)), provider_name_(std::move(provider_name)),
      provider_model_(std::move(provider_model))
{
    config_.validate();
}

namespace {

std::uint64_t scaled_output_budget(
    double ratio,
    std::uint64_t ceiling,
    std::uint64_t context_limit)
{
    const long double scaled = static_cast<long double>(context_limit)
        * static_cast<long double>(ratio);
    const std::uint64_t rounded_down = static_cast<std::uint64_t>(scaled);
    return std::min(
        context_limit,
        std::max<std::uint64_t>(
            1,
            std::min(ceiling, rounded_down)));
}

}  // namespace

void ToolReplayRule::validate() const
{
    if (weight == 0) {
        throw std::invalid_argument{"tool replay weight must be positive"};
    }
}

void ToolReplayPolicy::validate() const
{
    if (!std::isfinite(fraction) || fraction <= 0.0 || fraction > 1.0) {
        throw std::invalid_argument{
            "tool replay fraction must be finite and in (0, 1]",
        };
    }
    if (ceiling == 0 || floor > ceiling) {
        throw std::invalid_argument{
            "tool replay floor and ceiling are invalid",
        };
    }
    if (recent_item_cap == 0 || stale_item_cap == 0) {
        throw std::invalid_argument{
            "tool replay item caps must be positive",
        };
    }
}

std::uint64_t ToolReplayPolicy::budget_for_context(
    std::uint64_t context_limit) const
{
    validate();
    if (context_limit == 0) {
        return 0;
    }
    const long double scaled = static_cast<long double>(context_limit)
        * static_cast<long double>(fraction);
    const auto rounded = static_cast<std::uint64_t>(scaled);
    return std::min(
        context_limit,
        std::max(floor, std::min(ceiling, rounded)));
}

std::string ToolReplayPolicy::fingerprint() const
{
    const std::string canonical = std::to_string(fraction)
        + ":" + std::to_string(floor)
        + ":" + std::to_string(ceiling)
        + ":" + std::to_string(recent_item_cap)
        + ":" + std::to_string(stale_item_cap)
        + ":" + std::to_string(recent_count);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        digest += hex[(hash >> shift) & 0xfU];
    }
    return digest;
}

namespace {

void add_saturating(std::uint64_t& total, std::uint64_t value)
{
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    if (value > max - total) {
        total = max;
        return;
    }
    total += value;
}

void add_text(
    std::uint64_t& total,
    const GenericUnicodeV1& estimator,
    std::string_view text)
{
    add_saturating(total, estimator.estimate(text).tokens);
}

RequestFingerprint fingerprint_content(std::string_view content)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : content) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }

    std::string digest;
    constexpr char hex[] = "0123456789abcdef";
    digest.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        digest += hex[(hash >> shift) & 0xfU];
    }
    return RequestFingerprint{.value = std::move(digest)};
}

std::size_t utf8_prefix_boundary(std::string_view text, std::size_t limit)
{
    const std::size_t bounded = std::min(limit, text.size());
    std::size_t position = 0;
    while (position < bounded) {
        const unsigned char lead = static_cast<unsigned char>(text[position]);
        std::size_t width = 1;
        if ((lead & 0x80U) == 0) {
            width = 1;
        } else if ((lead & 0xe0U) == 0xc0U) {
            width = 2;
        } else if ((lead & 0xf0U) == 0xe0U) {
            width = 3;
        } else if ((lead & 0xf8U) == 0xf0U) {
            width = 4;
        }
        if (position + width > bounded) {
            break;
        }
        position += width;
    }
    return position;
}

std::size_t utf8_suffix_boundary(std::string_view text, std::size_t start)
{
    std::size_t position = std::min(start, text.size());
    while (position < text.size()
        && (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U) {
        ++position;
    }
    return position;
}

std::string elision_marker(
    std::string_view source,
    RequestFingerprint digest)
{
    return "[elided:b=" + std::to_string(source.size())
        + ":d=" + digest.value + "]";
}

std::string head_tail_view(
    std::string_view source,
    std::uint64_t target_tokens,
    const GenericUnicodeV1& estimator,
    const RequestFingerprint& digest)
{
    if (source.empty()) {
        return {};
    }

    const std::string marker = elision_marker(source, digest);
    auto make_candidate = [&](std::size_t kept_bytes) {
        const std::size_t requested_head = (kept_bytes + 1) / 2;
        const std::size_t requested_tail = kept_bytes / 2;
        const std::size_t head = utf8_prefix_boundary(source, requested_head);
        const std::size_t tail_start = utf8_suffix_boundary(
            source,
            source.size() > requested_tail
                ? source.size() - requested_tail
                : 0);
        if (head >= tail_start) {
            return std::string{source};
        }
        return std::string{source.substr(0, head)}
            + marker
            + std::string{source.substr(tail_start)};
    };

    if (estimator.estimate(source).tokens <= target_tokens) {
        return std::string{source};
    }

    std::size_t low = 0;
    std::size_t high = source.size();
    const std::size_t first_end = utf8_prefix_boundary(
        source,
        std::min<std::size_t>(source.size(), 4));
    std::size_t last_start = source.size() - 1;
    while (last_start > 0
        && (static_cast<unsigned char>(source[last_start]) & 0xc0U) == 0x80U) {
        --last_start;
    }
    std::string best = first_end < last_start
        ? std::string{source.substr(0, first_end)}
            + marker
            + std::string{source.substr(last_start)}
        : marker;
    while (low <= high) {
        const std::size_t middle = low + (high - low) / 2;
        std::string candidate = make_candidate(middle);
        if (estimator.estimate(candidate).tokens <= target_tokens) {
            best = std::move(candidate);
            low = middle + 1;
        } else {
            if (middle == 0) {
                break;
            }
            high = middle - 1;
        }
    }
    return best;
}

struct ReplayCandidate {
    std::size_t message_index{0};
    std::size_t call_index{0};
    ToolReplayRule rule;
    std::string original;
    std::uint64_t raw_tokens{0};
    std::uint64_t turn_id{0};
    bool recent{false};
};

ToolReplayRule rule_for(
    const Request& request,
    std::string_view tool_name)
{
    for (const ToolSpec& spec : request.tools) {
        if (spec.name == tool_name) {
            ToolReplayRule rule{
                .mode = spec.replay_mode,
                .weight = spec.replay_weight,
            };
            rule.validate();
            return rule;
        }
    }
    return ToolReplayRule{};
}

Request replay_request(
    const Request& source,
    const ToolReplayPolicy& policy,
    std::uint64_t context_limit)
{
    policy.validate();
    Request result = source;
    const GenericUnicodeV1 estimator;
    std::vector<ReplayCandidate> candidates;
    std::uint64_t latest_turn = 0;
    for (const ToolSpec& spec : source.tools) {
        ToolReplayRule{
            .mode = spec.replay_mode,
            .weight = spec.replay_weight,
        }.validate();
    }
    for (const Message& message : source.messages) {
        latest_turn = std::max(latest_turn, message.turn_id.value);
    }

    for (std::size_t message_index = 0;
         message_index < result.messages.size();
         ++message_index) {
        Message& message = result.messages[message_index];
        for (std::size_t call_index = 0;
             call_index < message.tool_calls.size();
             ++call_index) {
            const ToolCall& call = message.tool_calls[call_index];
            if (call.is_pending() || call.output().empty()) {
                continue;
            }
            candidates.push_back(ReplayCandidate{
                .message_index = message_index,
                .call_index = call_index,
                .rule = rule_for(source, call.name),
                .original = call.output(),
                .raw_tokens = estimator.estimate(call.output()).tokens,
                .turn_id = message.turn_id.value,
            });
        }
    }

    const std::size_t recent_start = candidates.size() > policy.recent_count
        ? candidates.size() - policy.recent_count
        : 0;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index].recent = index >= recent_start
            || (latest_turn != 0 && candidates[index].turn_id == latest_turn);
    }

    std::vector<std::size_t> priority;
    priority.reserve(candidates.size());
    for (std::size_t index = candidates.size(); index-- > 0;) {
        priority.push_back(index);
    }
    std::vector<std::uint64_t> allocations(candidates.size(), 0);
    std::uint64_t total_weight = 0;
    for (const std::size_t index : priority) {
        const std::uint64_t priority_weight =
            static_cast<std::uint64_t>(candidates[index].rule.weight)
            * (candidates[index].recent ? 4U : 1U);
        total_weight += priority_weight;
    }
    std::uint64_t remaining = policy.budget_for_context(context_limit);
    for (const std::size_t index : priority) {
        const ReplayCandidate& candidate = candidates[index];
        const std::uint64_t cap = candidate.recent
            ? policy.recent_item_cap
            : policy.stale_item_cap;
        const std::uint64_t desired = std::min(cap, candidate.raw_tokens);
        const std::uint64_t priority_weight =
            static_cast<std::uint64_t>(candidate.rule.weight)
            * (candidate.recent ? 4U : 1U);
        const auto share = total_weight == 0
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(
                static_cast<long double>(remaining)
                * static_cast<long double>(priority_weight)
                / static_cast<long double>(total_weight));
        allocations[index] = std::min(desired, std::max<std::uint64_t>(
            share, desired != 0 && remaining != 0 ? 1 : 0));
        remaining -= std::min(remaining, allocations[index]);
        total_weight -= priority_weight;
    }
    // Rounding and item caps can leave a few units unused. Give those units
    // back in recency order so a newer result never loses to an older one.
    for (const std::size_t index : priority) {
        if (remaining == 0) {
            break;
        }
        const std::uint64_t cap = candidates[index].recent
            ? policy.recent_item_cap
            : policy.stale_item_cap;
        const std::uint64_t desired = std::min(cap, candidates[index].raw_tokens);
        const std::uint64_t extra = std::min(
            remaining,
            desired - allocations[index]);
        allocations[index] += extra;
        remaining -= extra;
    }

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        ReplayCandidate& candidate = candidates[index];
        ToolCall& call = result.messages[candidate.message_index]
            .tool_calls[candidate.call_index];
        std::string projected;
        if (candidate.rule.mode == ToolReplayMode::Ephemeral
            && !candidate.recent) {
            projected.clear();
        } else if (candidate.raw_tokens <= allocations[index]) {
            projected = candidate.original;
        } else {
            projected = head_tail_view(
                candidate.original,
                allocations[index],
                estimator,
                fingerprint_content(candidate.original));
        }

        if (auto* done = std::get_if<ToolCall::Done>(&call.status)) {
            done->output = projected;
        } else if (auto* failed = std::get_if<ToolCall::Failed>(&call.status)) {
            failed->output = projected;
        } else if (auto* rejected = std::get_if<ToolCall::Rejected>(&call.status)) {
            rejected->output = projected;
        } else if (auto* cancelled = std::get_if<ToolCall::Cancelled>(
                       &call.status)) {
            cancelled->output = projected;
        }
    }
    return result;
}

void append_segment(
    std::vector<ProjectionSegment>& segments,
    ProjectionSegmentKind kind,
    std::string stable_id,
    std::string content,
    std::uint64_t raw_estimate)
{
    segments.push_back(ProjectionSegment{
        .kind = kind,
        .stable_id = std::move(stable_id),
        .content_fingerprint = fingerprint_content(content),
        .raw_estimate = raw_estimate,
    });
}

std::vector<ProjectionSegment> manifest_for(const Request& request)
{
    const GenericUnicodeV1 estimator;
    std::vector<ProjectionSegment> segments;
    segments.reserve(2 + request.messages.size() + request.tools.size());

    append_segment(
        segments,
        ProjectionSegmentKind::Framing,
        "framing:envelope",
        "request-envelope-v1",
        8);
    append_segment(
        segments,
        ProjectionSegmentKind::SystemPrompt,
        "system-prompt",
        request.system_prompt,
        estimator.estimate(request.system_prompt).tokens);

    for (std::size_t index = 0; index < request.tools.size(); ++index) {
        const ToolSpec& tool = request.tools[index];
        const std::string schema = tool.input_schema.dump();
        const std::string stable_id = tool.name.empty()
            ? "tool-catalog:" + std::to_string(index)
            : "tool-catalog:" + tool.name;
        std::uint64_t estimate = 16;
        add_text(estimate, estimator, tool.name);
        add_text(estimate, estimator, tool.description);
        add_text(estimate, estimator, schema);
        append_segment(
            segments,
            ProjectionSegmentKind::ToolCatalog,
            stable_id,
            tool.name + "\n" + tool.description + "\n" + schema,
            estimate);
    }

    for (std::size_t message_index = 0;
         message_index < request.messages.size(); ++message_index) {
        const Message& message = request.messages[message_index];
        const std::string stable_id = message.id.value != 0
            ? "message:" + std::to_string(message.id.value)
            : "message:index:" + std::to_string(message_index);
        std::uint64_t estimate = 8;
        const std::string role = message.role == Role::User
            ? "user"
            : "assistant";
        add_text(estimate, estimator, role);
        add_text(estimate, estimator, message.text);
        if (message.error) {
            add_saturating(estimate, 2);
            add_text(estimate, estimator, *message.error);
        }
        append_segment(
            segments,
            ProjectionSegmentKind::ConversationMessage,
            stable_id,
            role + "\n" + message.text + "\n"
                + (message.error ? *message.error : std::string{}),
            estimate);
        segments.back().assistant_message = message.role == Role::Assistant;
        segments.back().framing_estimate = estimate
            - estimator.estimate(message.text).tokens;

        for (std::size_t call_index = 0;
             call_index < message.tool_calls.size(); ++call_index) {
            const ToolCall& call = message.tool_calls[call_index];
            const std::string call_id = call.id.empty()
                ? std::to_string(call_index)
                : call.id;
            const std::string args = call.args.dump();
            std::uint64_t call_estimate = 8;
            add_text(call_estimate, estimator, call.id);
            add_text(call_estimate, estimator, call.name);
            add_text(call_estimate, estimator, args);
            append_segment(
                segments,
                ProjectionSegmentKind::ToolCall,
                stable_id + ":tool-call:" + call_id,
                call.id + "\n" + call.name + "\n" + args,
                call_estimate);

            append_segment(
                segments,
                ProjectionSegmentKind::ToolResult,
                stable_id + ":tool-result:" + call_id,
                call.output(),
                estimator.estimate(call.output()).tokens);
        }
    }

    // Keep a request-level framing charge even for an empty request. It is a
    // named segment so a later anchor can replace only the changed boundary.
    append_segment(
        segments,
        ProjectionSegmentKind::Framing,
        "framing:request",
        "request-framing-v1",
        32);
    return segments;
}

std::size_t appendable_segment_count(
    const std::vector<ProjectionSegment>& segments)
{
    if (!segments.empty()
        && segments.back().kind == ProjectionSegmentKind::Framing
        && segments.back().stable_id == "framing:request") {
        return segments.size() - 1;
    }
    return segments.size();
}

bool compatible(
    const ProjectionSegment& left,
    const ProjectionSegment& right)
{
    return left.kind == right.kind
        && left.stable_id == right.stable_id
        && left.content_fingerprint == right.content_fingerprint;
}

const SessionCalibration* find_calibration(
    const ContextProjectionLedger& ledger,
    std::string_view provider,
    std::string_view model,
    std::string_view estimator_version,
    std::string_view policy_version)
{
    for (const SessionCalibration& calibration : ledger.calibrations) {
        if (calibration.provider == provider
            && calibration.model == model
            && calibration.estimator_version == estimator_version
            && calibration.policy_version == policy_version) {
            return &calibration;
        }
    }
    return nullptr;
}

std::uint64_t calibrated_value(
    std::uint64_t raw,
    const SessionCalibration* calibration)
{
    if (calibration == nullptr) {
        return raw;
    }
    const double multiplier = std::clamp(
        calibration->multiplier,
        calibration->minimum_multiplier,
        calibration->maximum_multiplier);
    const double scaled = static_cast<double>(raw) * multiplier;
    return static_cast<std::uint64_t>(std::ceil(scaled));
}

}  // namespace

ContextProjection project_context(
    std::string_view provider,
    std::string_view model,
    const Request& request,
    const ContextProjectionLedger& ledger,
    std::string_view estimator_version,
    std::string_view policy_version,
    const ToolReplayPolicy& replay_policy,
    std::uint64_t context_limit)
{
    Request wire_request = replay_request(request, replay_policy, context_limit);
    const std::string projection_policy_version = std::string{policy_version}
        + ":tool-replay:" + replay_policy.fingerprint();
    std::vector<ProjectionSegment> segments = manifest_for(wire_request);
    const std::string projection_estimator_version{estimator_version};
    const RequestFingerprint request_fingerprint = fingerprint_request(
        model,
        wire_request);
    const SessionCalibration* calibration = find_calibration(
        ledger,
        provider,
        model,
        projection_estimator_version,
        projection_policy_version);

    std::uint64_t raw_total = 0;
    std::uint64_t calibrated_total = 0;
    for (ProjectionSegment& segment : segments) {
        add_saturating(raw_total, segment.raw_estimate);
        const std::uint64_t calibrated = calibrated_value(
            segment.raw_estimate, calibration);
        if (calibration != nullptr) {
            segment.calibrated_estimate = calibrated;
        }
        add_saturating(calibrated_total, calibrated);
    }

    ContextProjection projection{
        .request = std::move(wire_request),
        .provider = std::string{provider},
        .model = std::string{model},
        .fingerprint = request_fingerprint,
        .estimator_version = projection_estimator_version,
        .policy_version = projection_policy_version,
        .segments = std::move(segments),
        .realistic_input_tokens = raw_total,
        .projected_input_tokens = calibrated_total,
    };

    if (!ledger.exact_anchor) {
        return projection;
    }

    const ContextAggregateAnchor& anchor = *ledger.exact_anchor;
    const std::size_t anchor_count = appendable_segment_count(
        anchor.covered_segments);
    const std::size_t current_count = appendable_segment_count(
        projection.segments);
    const bool identity_matches = anchor.provider == provider
        && anchor.model == model
        && anchor.estimator_version == projection.estimator_version
        && anchor.policy_version == projection.policy_version
        && current_count >= anchor_count;
    if (!identity_matches) {
        return projection;
    }
    for (std::size_t index = 0; index < anchor_count; ++index) {
        if (!compatible(
                anchor.covered_segments[index], projection.segments[index])) {
            return projection;
        }
    }

    projection.used_exact_anchor = true;
    projection.exact_anchor_covers_request = current_count == anchor_count
        && projection.segments.size() == anchor.covered_segments.size();

    std::uint64_t delta = 0;
    bool output_applied = false;
    for (std::size_t index = anchor_count;
         index < current_count;
         ++index) {
        const ProjectionSegment& segment = projection.segments[index];
        if (!output_applied && segment.assistant_message) {
            add_saturating(delta, anchor.output_tokens);
            add_saturating(delta, segment.framing_estimate);
            output_applied = true;
            continue;
        }
        add_saturating(
            delta,
            segment.calibrated_estimate.value_or(segment.raw_estimate));
    }

    // The request boundary is represented in both manifests. Its next
    // framing estimate replaces, rather than stacks on, the old boundary.
    // Keeping this explicit makes the append rule safe if that charge changes
    // in a future policy version.
    if (!anchor.covered_segments.empty()
        && !projection.segments.empty()
        && anchor.covered_segments.back().stable_id == "framing:request"
        && projection.segments.back().stable_id == "framing:request") {
        const std::uint64_t next = projection.segments.back()
            .calibrated_estimate.value_or(
                projection.segments.back().raw_estimate);
        const std::uint64_t previous = anchor.covered_segments.back()
            .raw_estimate;
        if (next >= previous) {
            add_saturating(delta, next - previous);
        }
    }
    projection.incremental_input_tokens = delta;

    projection.projected_input_tokens = anchor.input_tokens;
    add_saturating(projection.projected_input_tokens, delta);
    return projection;
}

void record_exact_usage(ContextProjectionLedger& ledger,
    const ContextProjectionReceipt& receipt,
    std::uint64_t input_tokens,
    std::uint64_t output_tokens)
{
    ContextAggregateAnchor anchor{
        .provider = receipt.provider,
        .model = receipt.model,
        .estimator_version = receipt.estimator_version,
        .policy_version = receipt.policy_version,
        .covered_segments = receipt.segments,
        .input_tokens = input_tokens,
        .output_tokens = output_tokens,
    };
    for (ProjectionSegment& segment : anchor.covered_segments) {
        segment.calibrated_estimate.reset();
    }
    ledger.exact_anchor = std::move(anchor);

    SessionCalibration* calibration = nullptr;
    for (SessionCalibration& candidate : ledger.calibrations) {
        if (candidate.provider == receipt.provider
            && candidate.model == receipt.model
            && candidate.estimator_version == receipt.estimator_version
            && candidate.policy_version == receipt.policy_version) {
            calibration = &candidate;
            break;
        }
    }
    if (calibration == nullptr) {
        ledger.calibrations.push_back(SessionCalibration{
            .provider = receipt.provider,
            .model = receipt.model,
            .estimator_version = receipt.estimator_version,
            .policy_version = receipt.policy_version,
        });
        calibration = &ledger.calibrations.back();
    }

    if (receipt.realistic_input_tokens == 0) {
        return;
    }
    const double observed_ratio = std::clamp(
        static_cast<double>(input_tokens)
            / static_cast<double>(receipt.realistic_input_tokens),
        calibration->minimum_multiplier,
        calibration->maximum_multiplier);
    const double alpha = std::clamp(
        calibration->smoothing_weight, 0.0, 1.0);
    calibration->multiplier = std::clamp(
        calibration->multiplier
            + alpha * (observed_ratio - calibration->multiplier),
        calibration->minimum_multiplier,
        calibration->maximum_multiplier);
}

void ContextManagementConfig::validate() const
{
    if (configured_limit == 0) {
        throw std::invalid_argument{"context limit must be positive"};
    }
    if (provider_limit && *provider_limit == 0) {
        throw std::invalid_argument{"provider context limit must be positive"};
    }
    if (safety_margin >= effective_limit()) {
        throw std::invalid_argument{
            "safety margin must be smaller than the effective context limit",
        };
    }
    if (!std::isfinite(normal_output_ratio)
        || normal_output_ratio <= 0.0 || normal_output_ratio > 1.0
        || !std::isfinite(compaction_output_ratio)
        || compaction_output_ratio <= 0.0 || compaction_output_ratio > 1.0
        || !std::isfinite(compaction_trigger_ratio)
        || compaction_trigger_ratio <= 0.0 || compaction_trigger_ratio > 1.0) {
        throw std::invalid_argument{
            "context management ratios must be finite and in (0, 1]",
        };
    }
    if (normal_output_ceiling == 0 || compaction_output_ceiling == 0) {
        throw std::invalid_argument{
            "context management output ceilings must be positive",
        };
    }
    if (estimator_version.empty() || projection_policy_version.empty()) {
        throw std::invalid_argument{
            "context management version strings must not be empty",
        };
    }
    tool_replay_policy.validate();

    const std::uint64_t effective = effective_limit();
    if (scaled_output_budget(
            normal_output_ratio,
            normal_output_ceiling,
            effective)
            > effective - safety_margin
        || scaled_output_budget(
            compaction_output_ratio,
            compaction_output_ceiling,
            effective)
            > effective - safety_margin) {
        throw std::invalid_argument{
            "output budget and safety margin exceed context limit",
        };
    }
}

std::uint64_t ContextManagementConfig::effective_limit() const noexcept
{
    return provider_limit
        ? std::min(configured_limit, *provider_limit)
        : configured_limit;
}

std::uint64_t ContextManagementConfig::normal_output_budget() const
{
    validate();
    return scaled_output_budget(
        normal_output_ratio,
        normal_output_ceiling,
        effective_limit());
}

std::uint64_t ContextManagementConfig::compaction_output_budget() const
{
    validate();
    return scaled_output_budget(
        compaction_output_ratio,
        compaction_output_ceiling,
        effective_limit());
}

ContextBudgetAssessment assess_context_admission(
    const ContextProjection& projection,
    const ContextManagementConfig& config,
    std::uint64_t request_output_budget)
{
    config.validate();
    if (request_output_budget == 0
        || request_output_budget > config.effective_limit()
            - config.safety_margin) {
        throw std::invalid_argument{
            "request output budget and safety margin exceed context limit",
        };
    }

    const std::uint64_t effective = config.effective_limit();
    const std::uint64_t hard = effective
        - request_output_budget
        - config.safety_margin;
    const std::uint64_t trigger = static_cast<std::uint64_t>(
        static_cast<long double>(hard)
        * static_cast<long double>(config.compaction_trigger_ratio));

    const std::uint64_t projected = projection.projected_input_tokens;
    const bool estimated = !projection.exact_anchor_covers_request;
    return ContextBudgetAssessment{
        .projected_input_tokens = projected,
        .hard_input_limit = hard,
        .estimated = estimated,
        .triggered = projected >= trigger,
        .admitted = projected <= hard,
        .incremental_input_tokens = projection.incremental_input_tokens,
        .exact_anchor_reused = projection.used_exact_anchor,
    };
}

}  // namespace my_agent
