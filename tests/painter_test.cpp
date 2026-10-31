#include "my_agent/ui/painter.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

my_agent::ui::ScreenConfig long_transcript(int count)
{
    my_agent::ui::ScreenConfig screen;
    screen.transcript.fill_available_height = false;
    for (int index = 0; index < count; ++index) {
        maya::Turn::Config turn;
        turn.label = "agent";
        turn.body.emplace_back(maya::Turn::PlainText{
            .content = "transcript row " + std::to_string(index),
        });
        screen.transcript.turns.push_back(std::move(turn));
    }
    return screen;
}

}  // namespace

TEST(PainterTest, RendersWithExplicitSizeAndInjectedWriter)
{
    my_agent::ui::ScreenConfig screen = long_transcript(0);
    screen.transcript.turns.push_back(maya::Turn::Config{
        .label = "agent",
        .body = {maya::Turn::PlainText{.content = "painter"}},
    });

    my_agent::ui::Painter painter;
    std::string written;
    const my_agent::ui::WriteSink writer = [&written](std::string_view bytes) {
        written.append(bytes);
        return true;
    };

    ASSERT_TRUE(painter.render(screen, my_agent::ui::Size{20, 4}, writer));
    EXPECT_NE(written.find("painter"), std::string::npos);
}

TEST(PainterTest, FrozenTranscriptHoldsItsPositionWhileTailContinuesGrowing)
{
    my_agent::ui::Painter painter;
    std::string written;
    const my_agent::ui::WriteSink writer = [&written](std::string_view bytes) {
        written.append(bytes);
        return true;
    };
    const my_agent::ui::Size size{40, 8};

    my_agent::ui::ScreenConfig screen = long_transcript(20);
    ASSERT_TRUE(painter.render(screen, size, writer));
    const auto initial = painter.transcript_viewport_snapshot();
    ASSERT_GT(initial.maximum, 0);
    ASSERT_TRUE(initial.at_bottom);

    screen.transcript.turns.push_back(maya::Turn::Config{
        .label = "agent",
        .body = {maya::Turn::PlainText{.content = "bottom-follow output"}},
    });
    ASSERT_TRUE(painter.render(screen, size, writer));
    const auto followed = painter.transcript_viewport_snapshot();
    EXPECT_TRUE(followed.at_bottom);
    EXPECT_GT(followed.maximum, initial.maximum);

    painter.apply_transcript_navigation(
        my_agent::ui::TranscriptNavigation::PageUp);
    const auto frozen = painter.transcript_viewport_snapshot();
    ASSERT_FALSE(frozen.at_bottom);
    ASSERT_LT(frozen.offset, frozen.maximum);

    ASSERT_TRUE(painter.render(screen, size, writer));
    EXPECT_EQ(frozen.offset, painter.transcript_viewport_snapshot().offset);

    screen.transcript.turns.push_back(maya::Turn::Config{
        .label = "agent",
        .body = {maya::Turn::PlainText{.content = "new tail output"}},
    });
    ASSERT_TRUE(painter.render(screen, size, writer));
    const auto after_growth = painter.transcript_viewport_snapshot();
    EXPECT_EQ(frozen.offset, after_growth.offset);
    EXPECT_GT(after_growth.maximum, frozen.maximum);

    painter.apply_transcript_navigation(
        my_agent::ui::TranscriptNavigation::Bottom);
    EXPECT_TRUE(painter.transcript_viewport_snapshot().at_bottom);
}

TEST(PainterTest, ResizeFollowsTheTailButPreservesAHistoricalOffset)
{
    my_agent::ui::Painter painter;
    const my_agent::ui::WriteSink writer = [](std::string_view) {
        return true;
    };
    const my_agent::ui::ScreenConfig screen = long_transcript(30);

    ASSERT_TRUE(painter.render(screen, my_agent::ui::Size{40, 8}, writer));
    ASSERT_TRUE(painter.transcript_viewport_snapshot().at_bottom);

    ASSERT_TRUE(painter.render(screen, my_agent::ui::Size{40, 12}, writer));
    EXPECT_TRUE(painter.transcript_viewport_snapshot().at_bottom);

    painter.apply_transcript_navigation(
        my_agent::ui::TranscriptNavigation::PageUp);
    const auto frozen = painter.transcript_viewport_snapshot();
    ASSERT_FALSE(frozen.at_bottom);

    ASSERT_TRUE(painter.render(screen, my_agent::ui::Size{40, 6}, writer));
    const auto resized = painter.transcript_viewport_snapshot();
    EXPECT_EQ(frozen.offset, resized.offset);
    EXPECT_GT(resized.maximum, resized.offset);
}
