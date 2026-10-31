#include "my_agent/ui/input.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

// 场景：敲进三个可见字符。
// 领域语义：raw mode 下 stdin 是裸字节流，前端要的是「按键」。最基本的契约是
// 可见字符原样成为文本 —— 这是后面所有按键语义的锚点。
// Red 原因：仓库尚不存在 ui::InputDecoder（编译错误）。
TEST(InputTest, DecodesPrintableCharactersAsText)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> keys = decoder.feed("abc");

    ASSERT_EQ(3u, keys.size());
    EXPECT_EQ("a", keys[0].text);
    EXPECT_EQ("c", keys[2].text);
}

// 场景：一个汉字被 read 切成两半。
// 领域语义：这是本项目最会真实发生的输入 bug —— 中文占 3 字节，而 read 只保证返回
// 「已就绪的字节」，不保证在字符边界上切。残缺序列必须留到下一次 feed 缝合。
// 逐字节当作按键的实现会把「你」吐成 3 个乱码字符，插进输入行就再也删不干净。
// Red 原因：当前实现逐字节生成按键，会得到 3 个而不是 1 个。
TEST(InputTest, StitchesAMultibyteCharacterSplitAcrossTwoReads)
{
    my_agent::ui::InputDecoder decoder;
    const std::string ni = "你";  // E4 BD A0
    ASSERT_EQ(3u, ni.size());

    const std::vector<my_agent::ui::Key> first = decoder.feed(ni.substr(0, 2));
    EXPECT_TRUE(first.empty()) << "残缺序列不能吐出来";

    const std::vector<my_agent::ui::Key> second = decoder.feed(ni.substr(2));
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ("你", second[0].text);
}

// 场景：回车、退格、Ctrl-C、Ctrl-U、Ctrl-D、Ctrl-R、Ctrl-X。
// 领域语义：这四个是控制字节而不是文本 —— 把它们当文本会往输入行里插入不可见字符。
// 回车在 raw mode 下是 \r 而不是 \n（关掉了 ICRNL），认错会让回车毫无反应，
// 这是 raw mode 下最典型的失误。Ctrl-C 必须作为按键到达而不是杀进程，
// 否则备用屏来不及还原，用户的终端留在 raw mode。
// Red 原因：当前实现把所有字节都当文本。
TEST(InputTest, DecodesControlBytesAsKeysRatherThanText)
{
    my_agent::ui::InputDecoder decoder;

    // \r 而非 \n：raw mode 关掉了 ICRNL，回车原样是 \r。
    const std::vector<my_agent::ui::Key> keys = decoder.feed(
        "\r\x7f\x03\x15\x04\x12\x18"
    );

    ASSERT_EQ(7u, keys.size());
    EXPECT_EQ(my_agent::ui::Key::Kind::Enter, keys[0].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Backspace, keys[1].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Interrupt, keys[2].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ClearInput, keys[3].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Eof, keys[4].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ResumeQueue, keys[5].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ClearQueue, keys[6].kind);
    for (const my_agent::ui::Key& key : keys) {
        EXPECT_TRUE(key.text.empty()) << "控制键不该带文本";
    }
}

TEST(InputTest, DecodesBracketedPasteAsOneTextEvent)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::InputEvent> events = decoder.feed_events(
        "\x1b[200~first line\nsecond line\r\nthird line\x1b[201~"
    );

    ASSERT_EQ(1u, events.size());
    const auto* paste = std::get_if<my_agent::ui::PasteEvent>(&events.front());
    ASSERT_NE(nullptr, paste);
    EXPECT_EQ("first line\nsecond line\r\nthird line", paste->text);
}

TEST(InputTest, StitchesBracketedPasteMarkersAcrossReads)
{
    my_agent::ui::InputDecoder decoder;

    EXPECT_TRUE(decoder.feed_events("\x1b[200~first\nsec").empty());
    const std::vector<my_agent::ui::InputEvent> events = decoder.feed_events(
        "ond\x1b[201~"
    );

    ASSERT_EQ(1u, events.size());
    const auto* paste = std::get_if<my_agent::ui::PasteEvent>(&events.front());
    ASSERT_NE(nullptr, paste);
    EXPECT_EQ("first\nsecond", paste->text);
}

// 场景：方向键等未支持的转义序列。
// 领域语义：本切片不支持方向键，但 stdin 里照样会出现 ESC [ A。原样当文本会把
// "\x1b[A" 三个可见字符插进输入行，用户看到的是 "^[[A" 一样的垃圾。所以未识别的
// 转义序列必须被**丢弃**而不是当文本 —— 不支持一个功能和显示乱码是两件事。
// Red 原因：当前实现会把 ESC 和 [ A 都当成文本吐出来。
TEST(InputTest, DiscardsUnsupportedEscapeSequencesInsteadOfShowingGarbage)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> keys = decoder.feed("a\x1b[1;2Ab");

    ASSERT_EQ(2u, keys.size());
    EXPECT_EQ("a", keys[0].text);
    EXPECT_EQ("b", keys[1].text);
}

// 场景：转义序列被 read 切开。
// 领域语义：和 UTF-8 同一个问题的另一面 —— 转义序列也会跨 read 到达。如果只在
// 单次 feed 内部识别边界，切开的那半段会被当成文本，用户看到 "[A" 插进输入行。
// 缓冲必须跨 feed 保持，直到终结符到齐。
// 这是 characterization 测试：探查现有行为后固定下来，防止日后把缓冲逻辑
// "简化"成单次 feed 内处理。
TEST(InputTest, KeepsAnIncompleteEscapeSequenceAcrossReadBoundaries)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> first = decoder.feed("a\x1b[1;2");
    ASSERT_EQ(1u, first.size());
    EXPECT_EQ("a", first[0].text);  // ESC [ 留在缓冲里，没当成文本

    const std::vector<my_agent::ui::Key> second = decoder.feed("Ab");
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ("b", second[0].text);  // 序列到齐后整段丢弃，只剩 b
}

TEST(InputTest, DecodesCursorMovementEscapeSequences)
{
    my_agent::ui::InputDecoder decoder;
    const std::vector<my_agent::ui::Key> keys = decoder.feed(
        "\x1b[D\x1b[C\x1b[A\x1b[B\x1b[H\x1b[F\x1b[1~\x1b[4~"
    );

    ASSERT_EQ(8u, keys.size());
    EXPECT_EQ(my_agent::ui::Key::Kind::Left, keys[0].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Right, keys[1].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Up, keys[2].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Down, keys[3].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Home, keys[4].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::End, keys[5].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Home, keys[6].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::End, keys[7].kind);
}

// 场景：在 transcript 上翻页或跳到首尾。
// 领域语义：PageUp/PageDown 属于 transcript 导航，而 Ctrl+Home/Ctrl+End
// 是回到最旧/最新内容的明确手势；裸 Home/End 仍然交给 composer 编辑器。
// Red 原因：当前 decoder 只识别裸 Home/End，导航序列会被整段丢弃。
TEST(InputTest, DecodesTranscriptNavigationWithoutChangingBareComposerKeys)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> keys = decoder.feed(
        "\x1b[5~\x1b[6~\x1b[1;5H\x1b[1;5F\x1b[H\x1b[F"
    );

    ASSERT_EQ(6u, keys.size());
    EXPECT_EQ(my_agent::ui::Key::Kind::PageUp, keys[0].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::PageDown, keys[1].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ControlHome, keys[2].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ControlEnd, keys[3].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::Home, keys[4].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::End, keys[5].kind);
}

TEST(InputTest, StitchesTranscriptNavigationSequencesAcrossReads)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> first = decoder.feed("\x1b[1;");
    EXPECT_TRUE(first.empty());

    const std::vector<my_agent::ui::Key> second = decoder.feed("5H");
    ASSERT_EQ(1u, second.size());
    EXPECT_EQ(my_agent::ui::Key::Kind::ControlHome, second[0].kind);
}

TEST(InputTest, KeepsKittyUnmodifiedHomeAndEndAsComposerKeys)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::Key> keys = decoder.feed(
        "\x1b[57360u\x1b[57361u\x1b[57362u\x1b[57363u"
        "\x1b[57360;5u\x1b[57361;5u"
    );

    ASSERT_EQ(6u, keys.size());
    EXPECT_EQ(my_agent::ui::Key::Kind::Home, keys[0].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::End, keys[1].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::PageUp, keys[2].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::PageDown, keys[3].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ControlHome, keys[4].kind);
    EXPECT_EQ(my_agent::ui::Key::Kind::ControlEnd, keys[5].kind);
}

TEST(InputTest, DecodesSgrMouseButtonsCoordinatesAndModifiers)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::InputEvent> events = decoder.feed_events(
        "\x1b[<93;12;7M\x1b[<0;3;4m\x1b[<32;8;9M"
    );

    ASSERT_EQ(3u, events.size());
    const auto& wheel = std::get<my_agent::ui::MouseEvent>(events[0]);
    EXPECT_EQ(my_agent::ui::MouseButton::ScrollDown, wheel.button);
    EXPECT_EQ(my_agent::ui::MouseEventKind::Press, wheel.kind);
    EXPECT_EQ(12, wheel.x);
    EXPECT_EQ(7, wheel.y);
    EXPECT_TRUE(wheel.modifiers.shift);
    EXPECT_TRUE(wheel.modifiers.alt);
    EXPECT_TRUE(wheel.modifiers.ctrl);

    const auto& release = std::get<my_agent::ui::MouseEvent>(events[1]);
    EXPECT_EQ(my_agent::ui::MouseButton::Left, release.button);
    EXPECT_EQ(my_agent::ui::MouseEventKind::Release, release.kind);

    const auto& move = std::get<my_agent::ui::MouseEvent>(events[2]);
    EXPECT_EQ(my_agent::ui::MouseButton::Left, move.button);
    EXPECT_EQ(my_agent::ui::MouseEventKind::Move, move.kind);
}

TEST(InputTest, StitchesSgrMouseReportsAcrossReadsAndKeepsFollowingText)
{
    my_agent::ui::InputDecoder decoder;

    const std::vector<my_agent::ui::InputEvent> first = decoder.feed_events(
        "\x1b[<64;12;"
    );
    EXPECT_TRUE(first.empty());

    const std::vector<my_agent::ui::InputEvent> second = decoder.feed_events(
        "7Mtail"
    );
    ASSERT_EQ(5u, second.size());
    EXPECT_EQ(
        my_agent::ui::MouseButton::ScrollUp,
        std::get<my_agent::ui::MouseEvent>(second[0]).button);
    EXPECT_EQ("t", std::get<my_agent::ui::Key>(second[1]).text);
    EXPECT_EQ("l", std::get<my_agent::ui::Key>(second[4]).text);
}

TEST(InputTest, RejectsMalformedAndOversizedSgrReportsWithoutLosingFollowingText)
{
    my_agent::ui::InputDecoder decoder;

    std::string oversized{"\x1b[<64;"};
    oversized.append(300, '1');
    oversized += ";7;8Mtail";

    const std::vector<my_agent::ui::InputEvent> events = decoder.feed_events(
        "\x1b[<64;bad;7Mok" + oversized
    );

    std::string text;
    for (const auto& event : events) {
        if (const auto* key = std::get_if<my_agent::ui::Key>(&event)) {
            text += key->text;
        }
    }
    EXPECT_EQ("oktail", text);
    for (const auto& event : events) {
        EXPECT_FALSE(std::holds_alternative<my_agent::ui::MouseEvent>(event));
    }
}

}  // namespace
