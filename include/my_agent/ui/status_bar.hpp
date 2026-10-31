#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace my_agent::ui {

// Persistent, bounded telemetry history for the token sparkline. A sample is
// accepted only with a provider generation, so extra renders of the same
// snapshot cannot manufacture a faster stream.
class TokenRateHistory {
public:
    static constexpr std::size_t kCapacity = 16;

    void observe(
        std::optional<double> rate,
        std::optional<std::uint64_t> generation
    )
    {
        if (!rate || !generation || !std::isfinite(*rate)) {
            return;
        }
        if (last_generation_ && *last_generation_ == *generation) {
            return;
        }
        last_generation_ = *generation;
        samples_.push_back(static_cast<float>(std::max(0.0, *rate)));
        if (samples_.size() > kCapacity) {
            samples_.erase(samples_.begin());
        }
    }

    [[nodiscard]] const std::vector<float>& samples() const noexcept
    {
        return samples_;
    }

private:
    std::optional<std::uint64_t> last_generation_;
    std::vector<float> samples_;
};

// External display configuration and telemetry. Model owns token usage.
struct StatusBarInput {
    std::string model_name;
    std::optional<std::size_t> context_limit;
    std::optional<double> tokens_per_second;
    std::optional<std::size_t> tokens_generated;
    std::optional<std::uint64_t> telemetry_generation;
    std::optional<double> elapsed_seconds;
};

}  // namespace my_agent::ui
