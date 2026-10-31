#pragma once

#include "my_agent/runtime/identity.hpp"

#include <cstdint>
#include <optional>

namespace my_agent {

struct RequestUsage;

struct UsageTotals {
    std::uint64_t input_tokens{0};
    std::uint64_t cached_input_tokens{0};
    std::uint64_t output_tokens{0};
    std::uint64_t cache_read_input_tokens{0};
    std::uint64_t cache_creation_input_tokens{0};
};

struct UsageObservation {
    OperationRef operation{};
    RequestFingerprint request_fingerprint{};
    UsageTotals totals{};
    UsageQuality quality{UsageQuality::Unavailable};
};

struct TurnUsageState {
    TurnId turn_id{};
    UsageTotals totals{};
};

// Values only. Core validates operation/fingerprint and accepts an event once
// before handing it to these shared normalization and accumulation rules.
struct UsageState {
    std::optional<UsageObservation> latest_request{};
    std::optional<TurnUsageState> turn{};
    UsageTotals session{};
    UsageTotals compaction_session{};
    std::uint64_t request_count{0};
    std::optional<UsageObservation> observed_context{};

    void begin_turn(TurnId id);
    void record_accepted(const RequestUsage& event);
    void mark_request_unavailable(StreamRef ref, const RequestFingerprint& fingerprint);
    void mark_context_pending();
    [[nodiscard]] UsageQuality current_turn_quality() const noexcept;
};

} // namespace my_agent
