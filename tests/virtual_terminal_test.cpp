#include "virtual_terminal.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using my_agent::test::VirtualTerminal;
using my_agent::test::CellStyle;

// 这一组测的是**量具本身**，不是产品代码，所以它们必须绿。
// 存在的理由：幽灵行探针的全部结论都建立在这个虚拟终端的记账上，而「断言通过」
// 和「断言有鉴别力」是两件事 —— 上一轮的教训是自己写的验证代码没人 review。
// 期望值来自 ECMA-48 与 DEC STD 070 的规定，不是从实现反推的。

// 场景：CUP 绝对定位。
// 领域语义：渲染字节依赖 CUP 定位（raw mode 下没有 ONLCR，"\n" 只下移不回列）。
// 定位错一行，整个界面就错一行 —— 这是幽灵行表现出来的样子。
TEST(VirtualTerminalTest, CupPlacesTextOnTheAddressedRow)
{
    VirtualTerminal terminal{10, 3};
    terminal.feed("\x1b[2;1Hmid");

    const std::vector<std::string> screen = terminal.screen();
    ASSERT_EQ(3u, screen.size());
    EXPECT_EQ("", screen[0]);
    EXPECT_EQ("mid", screen[1]);
    EXPECT_EQ("", screen[2]);
    EXPECT_EQ(0, terminal.scrolls_in_alt_screen());
}

// 场景：正好填满末行，一个字符都不多。
// 领域语义：DECAWM 开时，写满末列只是置上「待换行」，**下一个**可打印字符才真正
// 换行（ECMA-48 §8.3.118）。所以「正好填满」本身无害 —— 这正是 wrap 当年用 >
// 而不是 >= 的依据。量具必须复现这个延迟，否则会把合法的填满误报成滚动。
TEST(VirtualTerminalTest, ExactlyFillingTheLastRowDoesNotScroll)
{
    VirtualTerminal terminal{5, 2};
    terminal.feed("\x1b[?1049h\x1b[2;1Habcde");

    EXPECT_EQ(0, terminal.scrolls_in_alt_screen());
    EXPECT_EQ("abcde", terminal.screen()[1]);
}

// 场景：末行多写一个字符。
// 领域语义：这是幽灵行的成因 1 在量具里的样子 —— 光标越过右边距、DECAWM 开着、
// 已经在末行，于是备用屏滚动一行。滚动之后所有 CUP 都落在错位的物理行上，
// 而屏幕上看到的就是「内容整块上移 + 顶行被顶掉」。
TEST(VirtualTerminalTest, OverrunningTheLastRowScrollsTheAltScreen)
{
    VirtualTerminal terminal{5, 2};
    terminal.feed("\x1b[?1049h\x1b[1;1Htop\x1b[2;1Habcdef");

    EXPECT_EQ(1, terminal.scrolls_in_alt_screen());
    EXPECT_EQ(1, terminal.right_margin_overruns());
    // "top" 被顶掉，末行留下溢出的那个字符 —— 这就是错位。
    EXPECT_EQ("abcde", terminal.screen()[0]);
    EXPECT_EQ("f", terminal.screen()[1]);
}

// 场景：关掉 DECAWM 之后同样多写一个字符。
// 领域语义：这是成因 1 的修法在量具里的样子，也是成因 2 为什么必须同片修的证据 ——
// DECAWM 关时光标停在第 W-1 列不前进，字符原地覆写，永不滚动。代价是末列字符被
// 覆盖，而从那个位置发 EL 会擦掉刚画的格子。
TEST(VirtualTerminalTest, WithAutowrapOffTheCursorSticksToTheLastColumn)
{
    VirtualTerminal terminal{5, 2};
    terminal.feed("\x1b[?1049h\x1b[?7l\x1b[1;1Htop\x1b[2;1Habcdef");

    EXPECT_FALSE(terminal.autowrap());
    EXPECT_EQ(0, terminal.scrolls_in_alt_screen()) << "DECAWM 关掉后不该再滚动";
    EXPECT_EQ("top", terminal.screen()[0]) << "顶行必须还在原处";
    EXPECT_EQ("abcdf", terminal.screen()[1]) << "末列被 f 覆写，这是可接受的代价";
}

// 场景：宽字符的记账。
// 领域语义：一个 2 列的字占两格，第二格不产出字节。量具必须按列记账 ——
// 按字符记账会让「4 个汉字 = 4 列」，探针于是量不出任何溢出。
TEST(VirtualTerminalTest, WideCharactersOccupyTwoCells)
{
    VirtualTerminal terminal{6, 1};
    terminal.feed("\x1b[1;1H你好世");

    EXPECT_EQ("你好世", terminal.screen()[0]);
    EXPECT_FALSE(terminal.cell_blank(0, 1))
        << "宽字形的第二列没有文本，但仍是被该字形占用的格子";
    EXPECT_EQ(0, terminal.scrolls_in_alt_screen());
    EXPECT_TRUE(terminal.unhandled().empty());
}

// 场景：SGR 改变后续格子的样式，reset 只影响 reset 之后画的格子。
// 领域语义：caret seam 通过 cell_style 判断「一格被画成什么样」，所以量具必须
// 证明它既不会漏掉样式，也不会在 reset 后把旧样式串到下一格。期望值直接来自
// SGR 字面量：97=亮白前景、44=蓝背景、1=粗体、7=反显、0=全部重置。
TEST(VirtualTerminalTest, SgrStyleIsRecordedPerCellAndResetForLaterCells)
{
    VirtualTerminal terminal{4, 1};
    terminal.feed("\x1b[97;44;1;7mA\x1b[0mB");

    const CellStyle styled = terminal.cell_style(0, 0);
    EXPECT_EQ(15, styled.fg);
    EXPECT_EQ(4, styled.bg);
    EXPECT_TRUE(styled.bold);
    EXPECT_TRUE(styled.inverse);

    EXPECT_EQ(CellStyle{}, terminal.cell_style(0, 1));
    EXPECT_TRUE(terminal.unhandled().empty());
}

// 场景：EL 擦到行尾。
// 领域语义：EL 会抹掉上一帧更长的行留下的尾巴。量具必须实现它，否则屏幕内容会
// 残留旧字符，探针把残留误报成幽灵行。
TEST(VirtualTerminalTest, EraseInLineClearsTheTailButKeepsWhatWasJustDrawn)
{
    VirtualTerminal terminal{8, 1};
    terminal.feed("\x1b[1;1HLONGLINE");  // 填满
    terminal.feed("\x1b[1;1Hab\x1b[K");  // 重画短行 + EL

    EXPECT_EQ("ab", terminal.screen()[0]);
}

// 场景：判据不认识的码点。
// 领域语义：这是量具唯一不能「兜底」的地方。猜 1 列会让真实溢出被算成合规 ——
// 探针在最需要它的时候失去鉴别力。所以未知码点必须记账，让断言直接失败。
TEST(VirtualTerminalTest, UnknownCodePointsAreRecordedRatherThanGuessed)
{
    VirtualTerminal terminal{10, 1};
    terminal.feed("\x1b[1;1H\xE0\xA4\x95");  // U+0915 天城文，判据表里没有

    ASSERT_EQ(1u, terminal.unhandled().size());
    EXPECT_EQ("unknown code point U+915", terminal.unhandled()[0]);
}

// 场景：未实现的转义序列。
// 领域语义：同上一条 —— 静默忽略会让探针在实现改动后悄悄不再鉴别。
// 比如日后有人加了 SGR 颜色或 DECSTBM 滚动区，量具必须说「我没记这个」。
TEST(VirtualTerminalTest, UnimplementedSequencesAreRecorded)
{
    VirtualTerminal terminal{10, 2};
    terminal.feed("\x1b[1;2r");  // DECSTBM 设滚动区，量具没实现

    ASSERT_EQ(1u, terminal.unhandled().size());
    EXPECT_EQ("CSI 1;2r", terminal.unhandled()[0]);
}

}  // namespace
