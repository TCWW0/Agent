#pragma once

#include <chrono>
#include <optional>

namespace my_agent::ui {

// 帧率合并。Ollama 一秒能吐几十个 token，每个都重绘会把时间全花在 write 上，
// 而人眼分辨不出 20fps 以上的差别。
//
// 时间由调用方注入而不是自己读时钟：判定因此是纯的、可确定性测试的 —— 靠 sleep
// 制造时间流逝的测试在负载下会随机失败。
class RepaintClock {
public:
    using Clock = std::chrono::steady_clock;

    explicit RepaintClock(int frames_per_second);

    // 请求一次立即重绘。请求只负责登记一个 deadline；should_paint 才消费它。
    // 调用方必须把同一帧的 now 注入进来，这样动画宿主和领域事件共享一个调度器。
    void request_frame(Clock::time_point now) noexcept;

    // 请求下一次可见变化至少在 delay 后发生。多个请求合并成最早 deadline，
    // 但真实绘制仍不得早于上一帧之后的 frame_budget_。
    void request_frame_after(
        Clock::time_point now,
        std::chrono::milliseconds delay
    ) noexcept;

    // 消费一个已经到期的请求并返回是否现在就画。尚未到 deadline 时返回 false，
    // 不会丢失 pending；无请求时也返回 false。
    [[nodiscard]]
    bool should_paint(Clock::time_point now) noexcept;

    // poll 的超时。有待补的更新时返回到下个窗口边界的剩余时间；无事可做时返回
    // -1（无限等），这是空闲时占 0% CPU 与占满一核的分界。
    [[nodiscard]]
    std::chrono::milliseconds time_until_next_paint(
        Clock::time_point now
    ) const noexcept;

private:
    std::chrono::milliseconds frame_budget_;
    std::optional<Clock::time_point> last_paint_;
    std::optional<Clock::time_point> next_deadline_;
};

}  // namespace my_agent::ui
