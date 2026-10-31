#pragma once

#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/generic_unicode_estimator.hpp"
#include "my_agent/runtime/identity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent {

inline constexpr std::string_view kContextProjectionPolicyV1Version =
    "context-projection-v1";

enum class ProjectionSegmentKind : std::uint8_t {
    SystemPrompt,
    ToolCatalog,
    ConversationMessage,
    ToolCall,
    ToolResult,
    Framing,
};

struct ProjectionSegment {
    ProjectionSegmentKind kind{ProjectionSegmentKind::Framing};
    std::string stable_id;
    RequestFingerprint content_fingerprint;
    std::uint64_t raw_estimate{0};
    std::uint64_t framing_estimate{0};
    std::optional<std::uint64_t> calibrated_estimate{};
    bool assistant_message{false};
};

struct ContextAggregateAnchor {
    std::string provider;
    std::string model;
    std::string estimator_version{kGenericUnicodeV1Version};
    std::string policy_version{kContextProjectionPolicyV1Version};
    std::vector<ProjectionSegment> covered_segments;
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
};

struct SessionCalibration {
    std::string provider;
    std::string model;
    std::string estimator_version{kGenericUnicodeV1Version};
    std::string policy_version{kContextProjectionPolicyV1Version};
    double smoothing_weight{0.25};
    double multiplier{1.0};
    double minimum_multiplier{0.75};
    double maximum_multiplier{1.25};
};

struct ContextProjectionLedger {
    std::optional<ContextAggregateAnchor> exact_anchor;
    std::vector<SessionCalibration> calibrations;
};

// The projection is the provider-facing request value plus the local,
// versioned admission estimate. Without an accepted compaction it is
// semantically identical to Request; keeping it separate gives later
// compaction a single outbound seam.
struct ContextProjection {
    Request request;
    std::string provider;
    std::string model;
    RequestFingerprint fingerprint;
    std::string estimator_version{
        kGenericUnicodeV1Version,
    };
    std::string policy_version{kContextProjectionPolicyV1Version};
    std::vector<ProjectionSegment> segments;
    std::uint64_t realistic_input_tokens{0};
    std::uint64_t projected_input_tokens{0};
    std::uint64_t incremental_input_tokens{0};
    bool used_exact_anchor{false};
    bool exact_anchor_covers_request{false};
};

struct ContextProjectionReceipt {
    std::string provider;
    std::string model;
    RequestFingerprint fingerprint;
    std::string estimator_version{kGenericUnicodeV1Version};
    std::string policy_version{kContextProjectionPolicyV1Version};
    std::vector<ProjectionSegment> segments;
    std::uint64_t realistic_input_tokens{0};
};

[[nodiscard]]
ContextProjection project_context(
    std::string_view provider,
    std::string_view model,
    const Request& request,
    const ContextProjectionLedger& ledger,
    std::string_view estimator_version,
    std::string_view policy_version,
    const ToolReplayPolicy& replay_policy,
    std::uint64_t context_limit);

void record_exact_usage(ContextProjectionLedger& ledger,
    const ContextProjectionReceipt& receipt,
    std::uint64_t input_tokens,
    std::uint64_t output_tokens);

}  // namespace my_agent
