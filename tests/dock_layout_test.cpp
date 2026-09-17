#include "my_agent/ui/screen.hpp"
#include "virtual_terminal.hpp"

#include <maya/render/frame.hpp>
#include <maya/core/scroll_state.hpp>
#include <maya/dsl.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/conversation.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kColumns = 40;
constexpr int kRows = 12;
// 哨兵：composer 半截输入与 status 相位各自唯一可寻的锚点。
// 约束：verb 哨兵 ≤ 10 列 —— maya StatusBar 的 verb 槽固定 verb_width=10，
// 超长 verb 会被削成 9 列 + …（测宽降级），哨兵就找不到了；
// 且两个哨兵互不为子串 —— find_row_containing 按子串匹配，重叠会错锚。
constexpr const char* kDraft = "DRAFT7F3A";
constexpr const char* kVerb = "VERB9C31";

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
    dock.status.phase.glyph = "*";
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
    // draft 在 composer 盒内：顶框之下、底框之上（正文行是盒的一部分）。
    ASSERT_LE(composer_top, draft_row) << bytes;
    ASSERT_LE(draft_row, composer_bottom) << bytes;

    // 圆角盒必须在右 gutter 之前闭合。此前只断言最右列为空，无法区分
    // “正确留出 gutter”与“右边框整列没有画出”这两种屏幕。
    EXPECT_EQ("╮", terminal.cell_text(composer_top, kColumns - 2)) << bytes;
    EXPECT_EQ("╯", terminal.cell_text(composer_bottom, kColumns - 2)) << bytes;

    // 1. composer 紧贴 status：底框与相位行之间不允许出现空行
    //    （status 区自身可以多行 —— 顶条/chip/底条 —— 但那是它的内容，
    //    不是间隔；断言钉「无空行」，不钉 status 的内部结构）。
    ASSERT_GT(status_row, composer_bottom) << bytes;
    for (int row = composer_bottom + 1; row < status_row; ++row) {
        EXPECT_FALSE(row_is_blank(screen[row]))
            << "blank row between composer and status at row " << row;
    }

    // 2. composer 上方恰好一空行；这行之上 dock 无内容（dock 从呼吸行开始）。
    ASSERT_GT(composer_top, 0) << bytes;
    EXPECT_TRUE(row_is_blank(screen[composer_top - 1]))
        << "the row above the composer must be the breathing blank";
    for (int row = 0; row < composer_top - 1; ++row) {
        EXPECT_TRUE(row_is_blank(screen[row]))
            << "row " << row << " above the breathing blank is not blank";
    }

    // 3. status 之下不画任何行：status 区允许自身多行，但一旦出现空行，
    //    之后不得再出现内容（「空行之后有内容」= 在 status 下面画了东西）。
    bool blank_after_status = false;
    for (int row = status_row + 1; row < kRows; ++row) {
        if (row_is_blank(screen[row])) {
            blank_after_status = true;
        } else {
            EXPECT_FALSE(blank_after_status)
                << "content at row " << row
                << " appears after a blank row below status";
        }
    }

    // 4. 一列 gutter：屏幕上每个非空行，左右边缘列都是空白格。
    for (int row = 0; row < kRows; ++row) {
        if (row_is_blank(screen[row])) {
            continue;
        }
        EXPECT_TRUE(terminal.cell_blank(row, 0))
            << "left gutter missing on row " << row;
        EXPECT_TRUE(terminal.cell_blank(row, kColumns - 1))
            << "right gutter missing on row " << row;
    }
}

// 全屏 adapter 会把同一个 dock 放在一个定高、裁剪的根列中。这个父级不能改变
// dock 的水平布局契约：右 gutter 前一格仍须是 Composer 的闭合边框。
TEST(DockLayoutTest, FullscreenParentKeepsComposerRightBorderClosed)
{
    using namespace maya::dsl;

    my_agent::ui::DockConfig dock;
    dock.composer.text = kDraft;
    dock.status.phase.glyph = "*";
    dock.status.phase.verb = kVerb;

    maya::ScrollState scroll;
    maya::Conversation::Config conversation_config;
    conversation_config.fill_available_height = false;
    maya::Element conversation = (
        maya::Conversation{std::move(conversation_config)}.build()
            | width(kColumns)
    ).build();
    maya::Element transcript = maya::detail::vstack()
        .grow(1.0f)
        .shrink(1.0f)
        .min_height(maya::Dimension::fixed(0))
        .overflow(maya::Overflow::Hidden)
        (v(std::move(conversation)).build()
            | scrolly(scroll, 0)
            | grow(1.0f));

    maya::Element root = maya::detail::vstack()
        .width(maya::Dimension::fixed(kColumns))
        .height(maya::Dimension::fixed(kRows))
        .overflow(maya::Overflow::Hidden)
        (
            std::move(transcript),
            my_agent::ui::dock_element(dock) | shrink(0.0f)
        );

    maya::FrameBuffer framebuffer{kColumns, kRows};
    const std::string& bytes = framebuffer.render(root, maya::theme::dark);
    my_agent::test::VirtualTerminal terminal{kColumns, kRows};
    terminal.feed(bytes);
    ASSERT_TRUE(terminal.unhandled().empty())
        << "Unhandled sequence: " << terminal.unhandled().front();

    const auto screen = terminal.screen();
    const int composer_top = find_row_containing(screen, "╭");
    const int composer_bottom = find_row_containing(screen, "╰");
    ASSERT_GE(composer_top, 0) << bytes;
    ASSERT_GE(composer_bottom, 0) << bytes;
    EXPECT_EQ("╮", terminal.cell_text(composer_top, kColumns - 2)) << bytes;
    EXPECT_EQ("╯", terminal.cell_text(composer_bottom, kColumns - 2)) << bytes;
}
