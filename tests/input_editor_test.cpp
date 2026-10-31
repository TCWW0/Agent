#include "my_agent/ui/input_editor.hpp"
#include "my_agent/ui/composer_paste.hpp"

#include <gtest/gtest.h>

namespace {

using my_agent::ui::edit_input;
using my_agent::ui::InputEditorState;
using my_agent::ui::Key;

TEST(InputEditorTest, InsertsAndMovesAcrossUtf8CharacterBoundaries)
{
    InputEditorState state = edit_input(
        "a你b", std::string::npos,
        Key{.kind = Key::Kind::Text, .text = "!"}
    );
    EXPECT_EQ("a你b!", state.buffer);

    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Left}
    );
    EXPECT_EQ("a你b!", state.buffer);
    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Right}
    );
    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Backspace}
    );
    EXPECT_EQ("a你b", state.buffer);

    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Left}
    );
    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Left}
    );
    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Right}
    );
    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Text, .text = "X"}
    );
    EXPECT_EQ("a你Xb", state.buffer);
}

TEST(InputEditorTest, HomeAndEndStayOnTheCurrentLogicalLine)
{
    InputEditorState state = edit_input(
        "first\nsecond", 8,
        Key{.kind = Key::Kind::Home}
    );
    EXPECT_EQ("first\nsecond", state.buffer);
    EXPECT_EQ(6u, state.cursor);

    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::End}
    );
    EXPECT_EQ(12u, state.cursor);
}

TEST(InputEditorTest, BackspaceAtTheStartAndEndAreNoOps)
{
    InputEditorState state = edit_input(
        "你好", 0,
        Key{.kind = Key::Kind::Backspace}
    );
    EXPECT_EQ("你好", state.buffer);
    EXPECT_EQ(0u, state.cursor);

    state = edit_input(
        state.buffer, std::string::npos,
        Key{.kind = Key::Kind::Right}
    );
    EXPECT_EQ("你好", state.buffer);
    EXPECT_EQ(6u, state.cursor);
}

TEST(InputEditorTest, TreatsPastePlaceholderAsOneEditableUnit)
{
    const std::string placeholder =
        my_agent::ui::make_paste_placeholder(0);
    const std::string buffer = "before" + placeholder + "after";

    auto state = edit_input(
        buffer, 6,
        Key{.kind = Key::Kind::Right}
    );
    EXPECT_EQ(6u + placeholder.size(), state.cursor);

    state = edit_input(
        state.buffer, state.cursor,
        Key{.kind = Key::Kind::Backspace}
    );
    EXPECT_EQ("beforeafter", state.buffer);
    EXPECT_EQ(6u, state.cursor);
}

}  // namespace
