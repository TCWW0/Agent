#include "my_agent/ui/stream_rate.hpp"

#include <algorithm>
#include <cmath>

namespace my_agent::ui {

LiveTokenRate::Snapshot LiveTokenRate::observe(
    std::size_t output_bytes,
    std::uint64_t source_generation,
    bool active,
    Clock::time_point now
) noexcept
{
    if (!active) {
        active_ = false;
        first_delta_at_.reset();
        last_sample_at_.reset();
        last_sample_bytes_ = 0;
        last_source_generation_ = source_generation;
        last_rate_at_.reset();
        displayed_rate_ = 0.0;
        return {};
    }

    if (!active_) {
        active_ = true;
        first_delta_at_.reset();
        last_sample_at_.reset();
        last_sample_bytes_ = 0;
        last_rate_at_.reset();
        displayed_rate_ = 0.0;
    }

    if (source_generation < last_source_generation_) {
        // A provider/core reset started a new counter without an observed
        // idle frame. Treat it like a fresh sub-turn instead of producing a
        // negative window or carrying the previous turn's TTFT.
        first_delta_at_.reset();
        last_sample_at_.reset();
        last_sample_bytes_ = 0;
        last_rate_at_.reset();
        displayed_rate_ = 0.0;
    }
    last_source_generation_ = source_generation;

    if (output_bytes > 0 && !first_delta_at_) {
        first_delta_at_ = now;
        last_sample_at_ = now;
        last_sample_bytes_ = output_bytes;
    }

    Snapshot snapshot{
        .active = true,
        .rate = 0.0,
        .tokens_generated = output_bytes / 4,
    };

    if (first_delta_at_) {
        const auto elapsed = now - *first_delta_at_;
        if (elapsed >= std::chrono::milliseconds{250}) {
            const double seconds = std::max(
                0.001,
                std::chrono::duration<double>{elapsed}.count());
            const double raw_rate =
                (static_cast<double>(output_bytes) / 4.0) / seconds;
            if (!last_rate_at_) {
                displayed_rate_ = raw_rate;
            } else {
                const double rate_dt = std::clamp(
                    std::chrono::duration<double>{now - *last_rate_at_}.count(),
                    0.0,
                    0.25);
                const double alpha = 1.0 - std::exp(-rate_dt / 0.25);
                displayed_rate_ += (raw_rate - displayed_rate_) * alpha;
            }
            last_rate_at_ = now;
            snapshot.rate = displayed_rate_;
        }

        if (last_sample_at_
            && now - *last_sample_at_ >= std::chrono::milliseconds{500}) {
            const auto sample_elapsed = now - *last_sample_at_;
            const double seconds = std::max(
                0.001,
                std::chrono::duration<double>{sample_elapsed}.count());
            const std::size_t bytes_delta = output_bytes >= last_sample_bytes_
                ? output_bytes - last_sample_bytes_
                : 0;
            snapshot.sample = Sample{
                .rate = (static_cast<double>(bytes_delta) / 4.0) / seconds,
                .generation = ++sample_generation_,
            };
            *last_sample_at_ = now;
            last_sample_bytes_ = output_bytes;
        }
    }

    return snapshot;
}

}  // namespace my_agent::ui
