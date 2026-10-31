#include "my_agent/ui/repaint_clock.hpp"

#include <algorithm>
#include <chrono>

namespace my_agent::ui {

RepaintClock::RepaintClock(int frames_per_second)
    : frame_budget_{
          std::chrono::milliseconds{frames_per_second > 0 ? 1000 / frames_per_second : 0}
      }
{
}

void RepaintClock::request_frame(Clock::time_point now) noexcept
{
    request_frame_after(now, std::chrono::milliseconds{0});
}

void RepaintClock::request_frame_after(
    Clock::time_point now,
    std::chrono::milliseconds delay
) noexcept
{
    if (delay < std::chrono::milliseconds{0}) {
        delay = std::chrono::milliseconds{0};
    }

    const auto earliest_allowed = last_paint_
        ? std::max(now, *last_paint_ + frame_budget_)
        : now;
    const auto requested_deadline = now + delay;
    const auto deadline = std::max(earliest_allowed, requested_deadline);

    if (!next_deadline_ || deadline < *next_deadline_) {
        next_deadline_ = deadline;
    }
}

bool RepaintClock::should_paint(Clock::time_point now) noexcept
{
    if (!next_deadline_) {
        return false;
    }
    if (now < *next_deadline_) {
        return false;
    }

    last_paint_ = now;
    next_deadline_.reset();
    return true;
}

std::chrono::milliseconds RepaintClock::time_until_next_paint(
    Clock::time_point now
) const noexcept
{
    if (!next_deadline_) {
        return std::chrono::milliseconds{-1};  // 无事可做：无限等，让出 CPU
    }
    if (now >= *next_deadline_) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::ceil<std::chrono::milliseconds>(*next_deadline_ - now);
}

}  // namespace my_agent::ui
