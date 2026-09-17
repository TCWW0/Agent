#include "my_agent/ui/screen.hpp"
#include "virtual_terminal.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/core/motion.hpp>
#include <maya/render/frame.hpp>
#include <maya/style/theme.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr int kWideColumns = 40;
constexpr int kNarrowColumns = 18;
constexpr int kRows = 12;
constexpr int kCursorOffset = 2;  // "dr|aft"：caret 覆盖 a 所在格，但不能挤动它。
constexpr int kLegacyBlinkPeriodMs = 530;
constexpr int kHalfBlinkPeriodMs = 265;
constexpr std::string_view kBlockCaret = "\xe2\x96\x88";  // █ U+2588

// 只用于把旧实现放到远离相位边界的确定性起点，避免真实墙钟碰巧跨界污染对照。
// Green 不需要知道这个周期：solid caret 对任何动画时刻都应给出同一格。
void align_legacy_blink_to_visible_phase()
{
    constexpr int kTargetPhaseMs = 20;
    const std::int64_t now_ms = maya::anim::default_clock().now_ms();
    const std::int64_t phase = now_ms % kLegacyBlinkPeriodMs;
    const std::int64_t advance =
        (kTargetPhaseMs - phase + kLegacyBlinkPeriodMs)
        % kLegacyBlinkPeriodMs;
    maya::testing::advance_anim_clock_ms(advance);
}

// 渲染一帧。应用每帧都会重新投影，所以测试也重走完整的公开接缝：
// project_screen → dock_element → FrameBuffer::render。
[[nodiscard]]
std::string render_dock_frame(
    const my_agent::Model& model,
    const my_agent::ui::UiState& ui,
    const maya::Theme& theme,
    maya::FrameBuffer& framebuffer)
{
    const my_agent::ui::ScreenConfig screen =
        my_agent::ui::project_screen(model, ui, theme);
    return std::string{
        framebuffer.render(my_agent::ui::dock_element(screen.dock), theme)};
}

[[nodiscard]]
bool find_glyph(
    const my_agent::test::VirtualTerminal& terminal,
    std::string_view glyph,
    int columns,
    int& row,
    int& column)
{
    for (int candidate_row = 0; candidate_row < kRows; ++candidate_row) {
        for (int candidate_column = 0; candidate_column < columns;
             ++candidate_column) {
            if (terminal.cell_text(candidate_row, candidate_column) == glyph) {
                row = candidate_row;
                column = candidate_column;
                return true;
            }
        }
    }
    return false;
}

// caret 不得参与正文排版。它可以给 "a" 加矩形样式，也可以在这一帧用块字形
// 替换 "a"；但后面的 "ft" 必须仍在原列，不能因为插入了额外字形整体右移。
void expect_draft_geometry_is_unchanged(
    const my_agent::test::VirtualTerminal& terminal,
    int row,
    int first_column)
{
    constexpr std::string_view kDraft = "draft";
    for (std::size_t index = 0; index < kDraft.size(); ++index) {
        const std::string actual = terminal.cell_text(
            row, first_column + static_cast<int>(index));
        if (index == static_cast<std::size_t>(kCursorOffset)) {
            EXPECT_TRUE(actual == "a" || actual == kBlockCaret)
                << "caret 格既没有保留当前字形，也不是块字形";
            continue;
        }
        EXPECT_EQ(
            std::string(1, kDraft[index]),
            actual)
            << "caret 改变了正文布局：draft 的第 " << index
            << " 个字形不在原本的格子里";
    }
}

// “矩形 caret”是格子事实，不是某个固定 ANSI 写法：反显或改变背景色都可以。
// 另一条合法路径是用正文的高对比样式画一个 full block；只把 full block 暗成
// 另一种前景色不算可见 caret。
[[nodiscard]]
bool has_rectangular_caret_style(
    const my_agent::test::CellStyle& text_style,
    const my_agent::test::CellStyle& caret_style,
    std::string_view caret_text)
{
    return caret_style.inverse || caret_style.bg != text_style.bg
        || (caret_text == kBlockCaret && caret_style == text_style);
}

}  // namespace

// issue #26 红 3：一个行为贯穿四次成功提交。
//
//   1. 首帧：caret 是应用画在 "a" 那一格上的高对比矩形；这一格可以保留 "a"
//      或用 full block 临时替换，但后缀仍在原列，caret 没有挤动 "draft"。
//   2. 控制帧：不改输入、宽度或动画时刻，立即再次 render + commit；同一格不变，
//      排除“commit 或跨帧缓存本身让 caret 消失”。
//   3. 时间实验：只推进旧实现的半个闪烁周期；同一格仍须不变，证明首版 caret
//      是 solid，而不是碰巧截到亮相位。
//   4. resize 实验：不再推进动画，只在**同一个** FrameBuffer 上改变宽度；新的物理
//      网格上仍满足同一契约，证明 resize 没有把 caret 留在旧布局或旧 front。
//
// 测试只观察 VirtualTerminal 的格子，不读取 Frame::cursor，也不要求某种 Maya
// 内部实现。当前实现会失败，因为 Composer 把 U+2588 当额外字形插进正文，并在
// idle 状态随动画钟切换前景样式：它既改变布局，也不是稳定的矩形格。
TEST(CaretSeamTest, PaintedCaretPreservesTextAndStaysSolidAcrossCommitsAndResize)
{
    my_agent::Model model;
    my_agent::ui::UiState ui;
    ui.input = "draft";
    ui.cursor = kCursorOffset;

    const maya::Theme theme = maya::theme::dark;
    align_legacy_blink_to_visible_phase();
    maya::FrameBuffer framebuffer{kWideColumns, kRows};
    my_agent::test::VirtualTerminal first_screen{kWideColumns, kRows};

    first_screen.feed(render_dock_frame(model, ui, theme, framebuffer));
    framebuffer.commit();
    ASSERT_TRUE(first_screen.unhandled().empty())
        << "首帧包含 VirtualTerminal 尚未建模的序列";

    int first_row = -1;
    int first_column = -1;
    ASSERT_TRUE(find_glyph(
        first_screen, "d", kWideColumns, first_row, first_column))
        << "正文没有经过 FrameBuffer 画进终端网格";

    expect_draft_geometry_is_unchanged(first_screen, first_row, first_column);
    const int first_caret_column = first_column + kCursorOffset;
    const my_agent::test::CellStyle first_text_style =
        first_screen.cell_style(first_row, first_column);
    const std::string first_caret_text =
        first_screen.cell_text(first_row, first_caret_column);
    const my_agent::test::CellStyle first_caret_style =
        first_screen.cell_style(first_row, first_caret_column);
    EXPECT_TRUE(has_rectangular_caret_style(
        first_text_style,
        first_caret_style,
        first_caret_text))
        << "插入格既非反显/背景矩形，也非正文色 full block";

    // 控制组：什么都不改变，只走第二次成功 render + commit。
    first_screen.feed(render_dock_frame(model, ui, theme, framebuffer));
    framebuffer.commit();
    ASSERT_TRUE(first_screen.unhandled().empty())
        << "控制帧包含 VirtualTerminal 尚未建模的序列";
    EXPECT_EQ(first_caret_text,
              first_screen.cell_text(first_row, first_caret_column))
        << "仅仅再次 commit 就改变了 caret 格内容";
    EXPECT_EQ(first_caret_style,
              first_screen.cell_style(first_row, first_caret_column))
        << "仅仅再次 commit 就改变了 caret 格样式";

    // 实验组：其余条件不变，只改变动画时间。
    maya::testing::advance_anim_clock_ms(kHalfBlinkPeriodMs);
    first_screen.feed(render_dock_frame(model, ui, theme, framebuffer));
    framebuffer.commit();
    ASSERT_TRUE(first_screen.unhandled().empty())
        << "时间实验帧包含 VirtualTerminal 尚未建模的序列";

    EXPECT_EQ(first_caret_text,
              first_screen.cell_text(first_row, first_caret_column))
        << "只推进动画时间后，caret 格的内容改变";
    EXPECT_EQ(first_caret_style,
              first_screen.cell_style(first_row, first_caret_column))
        << "半个闪烁周期后 caret 样式改变：它不是 solid";

    // 第二个实验组：时间不再变化，只改变 FrameBuffer 宽度。
    framebuffer.resize(kNarrowColumns, kRows);
    my_agent::test::VirtualTerminal resized_screen{kNarrowColumns, kRows};
    resized_screen.feed(render_dock_frame(model, ui, theme, framebuffer));
    framebuffer.commit();
    ASSERT_TRUE(resized_screen.unhandled().empty())
        << "resize 后的帧包含 VirtualTerminal 尚未建模的序列";

    int resized_row = -1;
    int resized_column = -1;
    ASSERT_TRUE(find_glyph(
        resized_screen, "d", kNarrowColumns, resized_row, resized_column))
        << "resize 后正文没有画进新的终端网格";

    expect_draft_geometry_is_unchanged(
        resized_screen, resized_row, resized_column);
    const int resized_caret_column = resized_column + kCursorOffset;
    const my_agent::test::CellStyle resized_text_style =
        resized_screen.cell_style(resized_row, resized_column);
    const my_agent::test::CellStyle resized_caret_style =
        resized_screen.cell_style(resized_row, resized_caret_column);
    EXPECT_EQ(first_caret_text,
              resized_screen.cell_text(resized_row, resized_caret_column))
        << "resize 后 caret 格的内容改变";
    EXPECT_TRUE(has_rectangular_caret_style(
        resized_text_style,
        resized_caret_style,
        resized_screen.cell_text(resized_row, resized_caret_column)))
        << "resize 后插入格不再是应用画出的矩形 caret";
    EXPECT_EQ(first_caret_style, resized_caret_style)
        << "resize 改变了 solid caret 的格子样式";
}
