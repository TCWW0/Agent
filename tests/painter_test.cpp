#include "my_agent/ui/painter.hpp"
#include "virtual_terminal.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

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

std::string rendered_screen(
    const my_agent::ui::ScreenConfig& screen,
    my_agent::ui::Size size)
{
    my_agent::ui::Painter painter;
    std::string bytes;
    EXPECT_TRUE(painter.render(
        screen, size, [&bytes](std::string_view chunk) {
            bytes.append(chunk);
            return true;
        }));

    my_agent::test::VirtualTerminal terminal{size.columns, size.rows};
    terminal.feed(bytes);
    EXPECT_TRUE(terminal.unhandled().empty())
        << (terminal.unhandled().empty() ? "" : terminal.unhandled().front());

    std::string text;
    for (const std::string& line : terminal.screen()) {
        text += line;
        text += '\n';
    }
    return text;
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

TEST(PainterTest, RendersTurnBodiesInANormalHeightViewport)
{
    my_agent::ui::ScreenConfig screen;
    screen.transcript.fill_available_height = false;
    screen.transcript.turns.push_back(maya::Turn::Config{
        .label = "you",
        .body = {maya::Turn::PlainText{.content = "NORMAL_USER_BODY"}},
    });
    screen.transcript.turns.push_back(maya::Turn::Config{
        .label = "agent",
        .body = {maya::Turn::PlainText{.content = "NORMAL_AGENT_BODY"}},
    });

    my_agent::ui::Painter painter;
    std::string written;
    const my_agent::ui::WriteSink writer = [&written](std::string_view bytes) {
        written.append(bytes);
        return true;
    };

    ASSERT_TRUE(painter.render(screen, my_agent::ui::Size{80, 24}, writer));
    EXPECT_NE(written.find("NORMAL_USER_BODY"), std::string::npos) << written;
    EXPECT_NE(written.find("NORMAL_AGENT_BODY"), std::string::npos) << written;
}

TEST(PainterTest, RendersConversationIdentityAndRoleSpecificTextAtEightyByTwentyFour)
{
    my_agent::Model model;
    model.domain.thread.messages = {
        {.role = my_agent::Role::User, .text = "# literal prompt"},
        {.role = my_agent::Role::Assistant, .text = "## Rendered answer"},
    };
    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, ui, maya::theme::dark);
    const std::string visible = rendered_screen(
        screen, my_agent::ui::Size{80, 24});

    EXPECT_NE(std::string::npos, visible.find("❯ You")) << visible;
    EXPECT_NE(std::string::npos, visible.find("# literal prompt")) << visible;
    EXPECT_NE(std::string::npos, visible.find("✦ qwen3.5:latest")) << visible;
    EXPECT_NE(std::string::npos, visible.find("Rendered answer")) << visible;
    EXPECT_EQ(std::string::npos, visible.find("## Rendered answer")) << visible;
}

TEST(PainterTest, KeepsTheAssistantBodyVisibleInANarrowViewport)
{
    my_agent::Model model;
    model.domain.thread.messages = {
        {.role = my_agent::Role::Assistant,
         .text = "NARROW **ANSWER** remains visible"},
    };
    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, ui, maya::theme::dark);
    const std::string visible = rendered_screen(
        screen, my_agent::ui::Size{36, 10});

    EXPECT_NE(std::string::npos, visible.find("NARROW ANSWER")) << visible;
    EXPECT_NE(std::string::npos, visible.find("remains visible")) << visible;
}

TEST(PainterTest, UsesAHeaderAnchorWithoutAHangingRailForMultilineMarkdown)
{
    my_agent::Model model;
    model.domain.thread.messages = {
        {.role = my_agent::Role::Assistant,
         .text = "BODY_FIRST\n\nBODY_SECOND"},
    };
    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, ui, maya::theme::dark);
    const std::string visible = rendered_screen(
        screen, my_agent::ui::Size{80, 24});

    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < visible.size()) {
        const std::size_t end = visible.find('\n', start);
        lines.emplace_back(visible.data() + start,
            (end == std::string::npos ? visible.size() : end) - start);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    const auto row_with = [&lines](std::string_view needle) {
        for (std::size_t row = 0; row < lines.size(); ++row) {
            if (lines[row].find(needle) != std::string_view::npos) {
                return row;
            }
        }
        return lines.size();
    };

    const std::size_t header = row_with("✦ qwen3.5:latest");
    const std::size_t first_body = row_with("BODY_FIRST");
    const std::size_t last_body = row_with("BODY_SECOND");
    ASSERT_LT(header, lines.size()) << visible;
    ASSERT_LT(first_body, lines.size()) << visible;
    ASSERT_LT(last_body, lines.size()) << visible;
    ASSERT_LT(header, first_body);
    ASSERT_LE(first_body, last_body);
    for (std::size_t row = header; row <= last_body; ++row) {
        EXPECT_EQ(std::string_view::npos, lines[row].find("┃"))
            << "hanging rail at row " << row << '\n' << visible;
    }
}

TEST(PainterTest, AlignsTheInterTurnDividerWithTheBodyTextColumn)
{
    my_agent::Model model;
    model.domain.thread.messages = {
        {.role = my_agent::Role::User, .text = "FIRST_BODY_ANCHOR"},
        {.role = my_agent::Role::Assistant, .text = "SECOND_BODY_ANCHOR"},
    };
    my_agent::ui::UiState ui;
    ui.status.model_name = "qwen3.5:latest";

    const my_agent::ui::ScreenConfig screen = my_agent::ui::project_screen(
        model, ui, maya::theme::dark);
    const std::string visible = rendered_screen(
        screen, my_agent::ui::Size{80, 24});

    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < visible.size()) {
        const std::size_t end = visible.find('\n', start);
        lines.emplace_back(visible.data() + start,
            (end == std::string::npos ? visible.size() : end) - start);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::size_t first_body_row = lines.size();
    std::size_t second_body_row = lines.size();
    for (std::size_t row = 0; row < lines.size(); ++row) {
        if (lines[row].find("FIRST_BODY_ANCHOR") != std::string_view::npos) {
            first_body_row = row;
        }
        if (lines[row].find("SECOND_BODY_ANCHOR") != std::string_view::npos) {
            second_body_row = row;
        }
    }

    ASSERT_LT(first_body_row, second_body_row) << visible;
    std::size_t divider_row = lines.size();
    for (std::size_t row = first_body_row + 1; row < second_body_row; ++row) {
        if (lines[row].find("─") != std::string_view::npos) {
            divider_row = row;
            break;
        }
    }
    ASSERT_LT(divider_row, second_body_row) << visible;
    const std::size_t body_column = lines[first_body_row].find_first_not_of(' ');
    const std::size_t divider_column = lines[divider_row].find_first_not_of(' ');
    ASSERT_NE(std::string_view::npos, body_column) << visible;
    ASSERT_NE(std::string_view::npos, divider_column) << visible;
    EXPECT_EQ(body_column, divider_column) << visible;
    const std::size_t divider_end = lines[divider_row].find_last_not_of(' ');
    ASSERT_NE(std::string_view::npos, divider_end) << visible;
    EXPECT_LE(lines[divider_row].size() - divider_end - 1, 2u) << visible;
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
