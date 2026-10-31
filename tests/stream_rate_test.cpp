#include "my_agent/ui/stream_rate.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using my_agent::ui::LiveTokenRate;

constexpr LiveTokenRate::Clock::time_point kStart{};

TEST(StreamRateTest, WarmsUpBeforeReportingAnInstantaneousRate)
{
    LiveTokenRate rate;

    static_cast<void>(rate.observe(0, 0, true, kStart));
    static_cast<void>(rate.observe(4, 1, true, kStart + 100ms));

    const LiveTokenRate::Snapshot snapshot = rate.observe(
        8, 2, true, kStart + 349ms);

    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(std::size_t{2}, snapshot.tokens_generated);
    EXPECT_DOUBLE_EQ(0.0, snapshot.rate);
}

TEST(StreamRateTest, ComputesLiveRateAndSamplesHistoryEveryHalfSecond)
{
    LiveTokenRate rate;

    static_cast<void>(rate.observe(4, 1, true, kStart + 100ms));
    static_cast<void>(rate.observe(8, 2, true, kStart + 350ms));
    const LiveTokenRate::Snapshot snapshot = rate.observe(
        20, 3, true, kStart + 600ms);

    EXPECT_GT(snapshot.rate, 8.0);
    EXPECT_LT(snapshot.rate, 10.0);
    EXPECT_EQ(std::size_t{5}, snapshot.tokens_generated);
    ASSERT_TRUE(snapshot.sample.has_value());
    EXPECT_DOUBLE_EQ(8.0, snapshot.sample->rate);
    EXPECT_EQ(std::uint64_t{1}, snapshot.sample->generation);
}

TEST(StreamRateTest, LeavingStreamingResetsTheNextSubTurn)
{
    LiveTokenRate rate;

    static_cast<void>(rate.observe(40, 1, true, kStart));
    const LiveTokenRate::Snapshot idle = rate.observe(
        40, 1, false, kStart + 1s);
    const LiveTokenRate::Snapshot next = rate.observe(
        4, 1, true, kStart + 2s);

    EXPECT_FALSE(idle.active);
    EXPECT_EQ(0.0, next.rate);
    EXPECT_EQ(std::size_t{1}, next.tokens_generated);
    EXPECT_FALSE(next.sample.has_value());
}

}  // namespace
