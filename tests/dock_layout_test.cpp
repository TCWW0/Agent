#include "my_agent/ui/screen.hpp"
#include "virtual_terminal.hpp"

#include <maya/render/frame.hpp>
#include <maya/style/theme.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kColumns = 40;
constexpr int kRows = 12;
// 哨兵：composer 半截输入与 status 相位各自唯一可寻的锚点。
constexpr const char* kDraft = "DRAFT7F3A";
constexpr const char* kVerb = "thinking7F3A";

[[nodiscard]]
int find_row_containing(
    const std::vector<std::string>& screen, std::string_view needle)
{
    for (int row = 0; row < static_cast<int>(screen.size()); ++row) {
        if (screen[static_cast<std::size_t>(row)].find(needle)
            != std::string::npos) {
            return row;
        }
    }
    return -1;
}

[[nodiscard]]
bool row_is_blank(const std::string& row)
{
    return row.find_first_not_of(' ') == std::string::npos;
}

}  // namespace

// 场景：无权限请求、composer 有半截输入、status 有相位的 dock，
// 经 dock_element → FrameBuffer::render → VirtualTerminal 网格。
// 领域语义（issue #26 验收 2 的布局不变量）：
//   1. composer 紧贴 status 上方 —— dock 的底是 status 本身，两者间无空行；
//   2. composer 上方恰好一空行 —— transcript 与 dock 之间的呼吸行，
//      这一行之上 dock 不再画任何内容；
//   3. status 之下不画任何行；
//   4. 左右各一列 gutter —— dock 内容不贴终端边缘。
// 布局语义只在渲染之后存在（Config 字段答不了「在哪一行」），所以断言
// 落在网格层 —— 这也是 issue 评论里「framebuffer bytes 钉投影接缝」的第一现场。
// Red 原因：dock_element 是桩（默认构造 Element，渲染零内容），
// 四个锚点行（╭ / ╰ / 正文 / 相位）全部找不到，首个 ASSERT 即挂。
TEST(DockLayoutTest, ComposerSitsDirectlyAboveStatusInsideAGutter)
{
    my_agent::ui::DockConfig dock;
    dock.composer.text = kDraft;
    dock.status.phase.verb = kVerb;

    maya::FrameBuffer framebuffer{kColumns, kRows};
    const std::string& bytes = framebuffer.render(
        my_agent::ui::dock_element(dock), maya::theme::dark);

    my_agent::test::VirtualTerminal terminal{kColumns, kRows};
    terminal.feed(bytes);
    ASSERT_TRUE(terminal.unhandled().empty())
        << "Unhandled sequence: " << terminal.unhandled().front();

    const std::vector<std::string> screen = terminal.screen();

    // 锚点：composer 盒的顶框 / 底框 / 正文行 / status 相位行。
    const int composer_top = find_row_containing(screen, "╭");
    const int composer_bottom = find_row_containing(screen, "╰");
    const int draft_row = find_row_containing(screen, kDraft);
    const int status_row = find_row_containing(screen, kVerb);
    ASSERT_GE(composer_top, 0) << bytes;
    ASSERT_GE(composer_bottom, 0) << bytes;
    ASSERT_GE(draft_row, 0) << bytes;
    ASSERT_GE(status_row, 0) << bytes;
    ASSERT_LT(composer_top, composer_bottom) << bytes;
    ASSERT_LE(composer_bottom, draft_row) << bytes;

    // 1. composer 紧贴 status：底框的下一行就是相位行，中间没有空行。
    EXPECT_EQ(status_row, composer_bottom + 1);

    // 2. composer 上方恰好一空行；这行之上 dock 无内容（dock 从呼吸行开始）。
    ASSERT_GT(composer_top, 0) << bytes;
    EXPECT_TRUE(row_is_blank(screen[composer_top - 1]))
        << "the row above the composer must be the breathing blank";
    for (int row = 0; row < composer_top - 1; ++row) {
        EXPECT_TRUE(row_is_blank(screen[row]))
            << "row " << row << " above the breathing blank is not blank";
    }

    // 3. status 之下不画任何行：以下全是空白。
    for (int row = status_row + 1; row < kRows; ++row) {
        EXPECT_TRUE(row_is_blank(screen[row]))
            << "row " << row << " below status must be blank";
    }

    // 4. 一列 gutter：dock 的每个非空行，左右边缘列都是空白格。
    for (int row = composer_top; row <= status_row; ++row) {
        EXPECT_TRUE(terminal.cell_blank(row, 0))
            << "left gutter missing on row " << row;
        EXPECT_TRUE(terminal.cell_blank(row, kColumns - 1))
            << "right gutter missing on row " << row;
    }
}
