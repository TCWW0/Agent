#include "my_agent/ui/transcript_viewport.hpp"

#include <maya/core/scroll_state.hpp>

#include <gtest/gtest.h>

TEST(TranscriptViewportTest, PagesPreserveOneRowOfVisibleOverlap)
{
    maya::ScrollState state;
    state.max_y = 20;
    state.y = 20;
    state.viewport_bounds = {.x = 0, .y = 0, .w = 80, .h = 6};
    my_agent::ui::TranscriptViewport viewport{state};

    viewport.apply(my_agent::ui::TranscriptNavigation::PageUp);
    EXPECT_EQ(15, viewport.snapshot().offset);

    viewport.apply(my_agent::ui::TranscriptNavigation::PageDown);
    EXPECT_EQ(20, viewport.snapshot().offset);
}

TEST(TranscriptViewportTest, NavigationClampsToTheRealGeometry)
{
    maya::ScrollState state;
    state.max_y = 7;
    state.y = 99;
    state.viewport_bounds = {.x = 0, .y = 0, .w = 80, .h = 4};
    my_agent::ui::TranscriptViewport viewport{state};

    const auto clamped = viewport.snapshot();
    EXPECT_EQ(7, clamped.offset);
    EXPECT_EQ(7, clamped.maximum);
    EXPECT_TRUE(clamped.at_bottom);

    viewport.apply(my_agent::ui::TranscriptNavigation::Top);
    EXPECT_EQ(0, viewport.snapshot().offset);
    EXPECT_FALSE(viewport.snapshot().at_bottom);

    viewport.apply(my_agent::ui::TranscriptNavigation::Bottom);
    EXPECT_EQ(7, viewport.snapshot().offset);
    EXPECT_TRUE(viewport.snapshot().at_bottom);
}

TEST(TranscriptViewportTest, AtBottomChangesOnlyWhenGeometryChanges)
{
    maya::ScrollState state;
    state.max_y = 10;
    state.y = 4;
    state.viewport_bounds = {.x = 0, .y = 0, .w = 80, .h = 6};
    my_agent::ui::TranscriptViewport viewport{state};

    EXPECT_FALSE(viewport.snapshot().at_bottom);

    state.max_y = 4;
    EXPECT_TRUE(viewport.snapshot().at_bottom);
}

TEST(TranscriptViewportTest, RoutesOnlyPlainVerticalWheelInsideTheViewport)
{
    maya::ScrollState state;
    state.max_y = 20;
    state.y = 20;
    state.viewport_bounds = {.x = 4, .y = 5, .w = 30, .h = 6};
    my_agent::ui::TranscriptViewport viewport{state};

    my_agent::ui::MouseEvent up{
        .button = my_agent::ui::MouseButton::ScrollUp,
        .kind = my_agent::ui::MouseEventKind::Press,
        .x = 10,
        .y = 6,
    };
    EXPECT_TRUE(viewport.apply_mouse(up, 3));
    EXPECT_EQ(17, viewport.snapshot().offset);

    my_agent::ui::MouseEvent outside = up;
    outside.y = 1;
    EXPECT_FALSE(viewport.apply_mouse(outside, 3));
    EXPECT_EQ(17, viewport.snapshot().offset);

    my_agent::ui::MouseEvent shifted = up;
    shifted.modifiers.shift = true;
    EXPECT_FALSE(viewport.apply_mouse(shifted, 3));
    EXPECT_EQ(17, viewport.snapshot().offset);

    my_agent::ui::MouseEvent click = up;
    click.button = my_agent::ui::MouseButton::Left;
    EXPECT_FALSE(viewport.apply_mouse(click, 3));
    EXPECT_EQ(17, viewport.snapshot().offset);

    my_agent::ui::MouseEvent moved = up;
    moved.kind = my_agent::ui::MouseEventKind::Move;
    EXPECT_FALSE(viewport.apply_mouse(moved, 3));
    EXPECT_EQ(17, viewport.snapshot().offset);

    my_agent::ui::MouseEvent down = up;
    down.button = my_agent::ui::MouseButton::ScrollDown;
    EXPECT_TRUE(viewport.apply_mouse(down, 5));
    EXPECT_EQ(20, viewport.snapshot().offset);
}
