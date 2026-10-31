#pragma once

#include "my_agent/domain/conversation.hpp"
#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/context_types.hpp"
#include "my_agent/runtime/identity.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace my_agent {

inline constexpr std::string_view kCompactionPromptVersion =
    "context-compaction-v3";
inline constexpr std::string_view kCompactionSourceBegin =
    "<context-compaction-source>";
inline constexpr std::string_view kCompactionSourceEnd =
    "</context-compaction-source>";

enum class CompactionFailureKind : std::uint8_t {
    Identity,
    Coverage,
    Schema,
    Size,
    Projection,
    NoGain,
};

enum class CompactionPlanFailureKind : std::uint8_t {
    NoCompleteHistory,
    SourceTooLarge,
};

struct CompactionPlanFailure {
    CompactionPlanFailureKind kind{CompactionPlanFailureKind::NoCompleteHistory};
    std::string message;
    std::optional<RequestFingerprint> source_digest{};
};

struct CompactionPlan {
    CompactionId compaction_id{};
    RequestPolicy policy{};
    MessageId covered_through{};
    Request source_request;
    Request compaction_request;
    Request next_request;
    RequestFingerprint source_digest;
    std::uint64_t before_tokens{0};
};

// The effective source is a wire-only request. It contains only complete
// transcript turns, and, when present, the previous summary as a synthetic
// User message. It never mutates the Thread or includes the queued request.
[[nodiscard]]
Request effective_completed_history(const Thread& thread);

[[nodiscard]] std::expected<CompactionPlan, CompactionPlanFailure> make_compaction_plan(
    CompactionId compaction_id,
    const Thread& thread,
    Request next_request,
    const RequestPolicy& policy,
    std::optional<ContextBudgetAssessment> next_budget = {});

struct CompactionValidationFailure {
    CompactionFailureKind kind;
    std::string rejection_reason;
};

[[nodiscard]] Request canonical_request(const Thread& thread);

[[nodiscard]]
std::expected<AcceptedCompaction, CompactionValidationFailure>
validate_compaction(
    const CompactionPlan& plan,
    std::string_view raw_summary);

}  // namespace my_agent
