#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace my_agent::ui {

// Live rate tracker for one streaming sub-turn. The provider/core boundary
// supplies cumulative output bytes; this UI seam owns the time-based
// approximation and coarse history sampling.
class LiveTokenRate {
public:
    using Clock = std::chrono::steady_clock;

    struct Sample {
        double rate{0.0};
        std::uint64_t generation{0};
    };

    struct Snapshot {
        bool active{false};
        double rate{0.0};
        std::size_t tokens_generated{0};
        std::optional<Sample> sample;
    };

    [[nodiscard]] Snapshot observe(
        std::size_t output_bytes,
        std::uint64_t source_generation,
        bool active,
        Clock::time_point now
    ) noexcept;

private:
    bool active_{false};
    std::optional<Clock::time_point> first_delta_at_;
    std::optional<Clock::time_point> last_sample_at_;
    std::size_t last_sample_bytes_{0};
    std::uint64_t last_source_generation_{0};
    std::uint64_t sample_generation_{0};
    std::optional<Clock::time_point> last_rate_at_;
    double displayed_rate_{0.0};
};

}  // namespace my_agent::ui
