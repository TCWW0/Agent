#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace my_agent {

enum class ToolReplayMode : std::uint8_t {
    Full,
    HeadTail,
    Ephemeral,
};

struct ToolReplayRule {
    ToolReplayMode mode{ToolReplayMode::HeadTail};
    std::uint32_t weight{1};

    void validate() const;
};

// These are request-level limits. They are deliberately expressed as
// estimator-token budgets rather than provider-specific token counts.
struct ToolReplayPolicy {
    double fraction{0.125};
    std::uint64_t floor{256};
    std::uint64_t ceiling{4096};
    std::uint64_t recent_item_cap{2048};
    std::uint64_t stale_item_cap{512};
    std::size_t recent_count{2};

    void validate() const;

    [[nodiscard]]
    std::uint64_t budget_for_context(std::uint64_t context_limit) const;

    [[nodiscard]]
    std::string fingerprint() const;
};

}  // namespace my_agent
