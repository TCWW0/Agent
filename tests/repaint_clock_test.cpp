#include "my_agent/ui/repaint_clock.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using my_agent::ui::RepaintClock;

// 时间由调用方注入而不是让 RepaintClock 读时钟：帧率合并的判定必须可确定性测试，
// 不能靠 sleep 来制造时间流逝 —— 那种测试在负载下会随机失败。
constexpr std::chrono::steady_clock::time_point kStart{};

// 场景：一个 token 到达。
// 领域语义：第一次总要画。合并的目的是防止一秒几十个 token 引发几十次重绘，
// 而不是延迟第一次显示 —— 用户等的就是第一个字出现。
// Red 原因：仓库尚不存在 ui::RepaintClock（编译错误）。
TEST(RepaintClockTest, PaintsImmediatelyOnTheFirstRequest)
{
    RepaintClock clock{20};  // 每秒 20 帧 = 每 50ms 至多一帧

    clock.request_frame(kStart);
    EXPECT_TRUE(clock.should_paint(kStart));
}

// 场景：一批 token 在同一个 50ms 窗口内密集到达。
// 领域语义：Ollama 一秒能吐几十个 token，每个都重绘会把全部时间花在 write 上，
// 而人眼分辨不出 20fps 以上的差别。窗口内的后续请求必须被压掉。
// Red 原因：最小实现无条件返回 true。
TEST(RepaintClockTest, CoalescesRequestsWithinTheSameFrameWindow)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));
    clock.request_frame(kStart + 10ms);
    EXPECT_FALSE(clock.should_paint(kStart + 10ms));
    clock.request_frame(kStart + 30ms);
    EXPECT_FALSE(clock.should_paint(kStart + 30ms));
    clock.request_frame(kStart + 49ms);
    EXPECT_FALSE(clock.should_paint(kStart + 49ms));
}

// 场景：窗口过去之后又来了 token。
// 领域语义：合并是**延迟**而不是丢弃。压掉的那些更新必须在下个窗口补上，
// 否则流式文本会停在某一帧不再前进 —— 那比不合并更糟。
// Red 原因：只记「画过一次」的实现会永远返回 false。
TEST(RepaintClockTest, PaintsAgainOnceTheFrameWindowHasElapsed)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));
    clock.request_frame(kStart + 10ms);
    ASSERT_FALSE(clock.should_paint(kStart + 10ms));
    clock.request_frame(kStart + 50ms);
    EXPECT_TRUE(clock.should_paint(kStart + 50ms));
}

// 场景：窗口内被压掉之后，问 poll 该等多久。
// 领域语义：poll 的超时必须正好是「到下个窗口边界还差多少」。给 -1（无限等）
// 会让被压掉的那次更新永远补不上 —— 后面没有新 token 时不会有新的唤醒，
// 屏幕就停在旧内容上。给 0 则退化成忙轮询。
// Red 原因：仓库尚不存在 time_until_next_paint。
TEST(RepaintClockTest, ReportsHowLongToWaitSoASuppressedPaintIsNotLost)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));
    clock.request_frame(kStart + 20ms);
    ASSERT_FALSE(clock.should_paint(kStart + 20ms));

    EXPECT_EQ(30ms, clock.time_until_next_paint(kStart + 20ms));
}

// 场景：没有待补的更新时问等多久。
// 领域语义：无事可做时应该无限等（-1），把 CPU 完全让出去。这是 TUI 空闲时
// 占 0% CPU 与占满一核的分界。
// Red 原因：无条件返回剩余窗口的实现会让空闲循环每 50ms 醒一次。
TEST(RepaintClockTest, WaitsIndefinitelyWhenNothingIsPending)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));

    EXPECT_EQ(-1ms, clock.time_until_next_paint(kStart + 100ms));
}

TEST(RepaintClockTest, MergesImmediateAndDelayedRequestsIntoEarliestDeadline)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));

    clock.request_frame_after(kStart, 100ms);
    clock.request_frame_after(kStart + 1ms, 80ms);
    EXPECT_EQ(81ms, clock.time_until_next_paint(kStart));

    clock.request_frame(kStart + 10ms);
    EXPECT_EQ(40ms, clock.time_until_next_paint(kStart + 10ms));
    EXPECT_FALSE(clock.should_paint(kStart + 49ms));
    EXPECT_TRUE(clock.should_paint(kStart + 50ms));
}

TEST(RepaintClockTest, AConsumedRequestMustBeRearmedByTheNextPaint)
{
    RepaintClock clock{20};

    clock.request_frame(kStart);
    ASSERT_TRUE(clock.should_paint(kStart));
    EXPECT_EQ(-1ms, clock.time_until_next_paint(kStart));

    clock.request_frame_after(kStart + 1ms, 5ms);
    EXPECT_TRUE(clock.should_paint(kStart + 50ms));
    EXPECT_EQ(-1ms, clock.time_until_next_paint(kStart + 50ms));
}

}  // namespace
