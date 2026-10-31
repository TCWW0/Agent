#include "my_agent/ui/input_editor.hpp"

#include "my_agent/ui/composer_paste.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace my_agent::ui {

namespace {

[[nodiscard]]
bool is_utf8_continuation(unsigned char byte) noexcept
{
    return (byte & 0xc0u) == 0x80u;
}

[[nodiscard]]
std::size_t normalize_cursor(std::string_view buffer, std::size_t cursor) noexcept
{
    if (cursor == std::string::npos || cursor > buffer.size()) {
        return buffer.size();
    }
    for (std::size_t position = 0; position < cursor;) {
        const std::size_t length = paste_placeholder_len_at(buffer, position);
        if (length != 0) {
            if (cursor < position + length) {
                return position;
            }
            position += length;
            continue;
        }
        ++position;
    }
    while (cursor > 0 && cursor < buffer.size()
           && is_utf8_continuation(static_cast<unsigned char>(buffer[cursor]))) {
        --cursor;
    }
    return cursor;
}

[[nodiscard]]
std::size_t previous_boundary(std::string_view buffer, std::size_t cursor) noexcept
{
    if (cursor == 0) {
        return 0;
    }
    if (const std::size_t length = paste_placeholder_len_ending_at(
            buffer, cursor); length != 0) {
        return cursor - length;
    }
    --cursor;
    while (cursor > 0
           && is_utf8_continuation(static_cast<unsigned char>(buffer[cursor]))) {
        --cursor;
    }
    return cursor;
}

[[nodiscard]]
std::size_t next_boundary(std::string_view buffer, std::size_t cursor) noexcept
{
    if (cursor >= buffer.size()) {
        return buffer.size();
    }
    if (const std::size_t length = paste_placeholder_len_at(
            buffer, cursor); length != 0) {
        return cursor + length;
    }
    ++cursor;
    while (cursor < buffer.size()
           && is_utf8_continuation(static_cast<unsigned char>(buffer[cursor]))) {
        ++cursor;
    }
    return cursor;
}

[[nodiscard]]
std::size_t line_start(std::string_view buffer, std::size_t cursor) noexcept
{
    if (cursor == 0) {
        return 0;
    }
    const std::size_t newline = buffer.rfind('\n', cursor - 1);
    return newline == std::string_view::npos ? 0 : newline + 1;
}

[[nodiscard]]
std::size_t line_end(std::string_view buffer, std::size_t cursor) noexcept
{
    const std::size_t newline = buffer.find('\n', cursor);
    return newline == std::string_view::npos ? buffer.size() : newline;
}

[[nodiscard]]
std::size_t character_count(
    std::string_view buffer,
    std::size_t begin,
    std::size_t end
) noexcept
{
    std::size_t count = 0;
    for (std::size_t cursor = begin; cursor < end;
         cursor = next_boundary(buffer, cursor)) {
        ++count;
    }
    return count;
}

[[nodiscard]]
std::size_t boundary_after_characters(
    std::string_view buffer,
    std::size_t begin,
    std::size_t end,
    std::size_t count
) noexcept
{
    std::size_t cursor = begin;
    while (cursor < end && count > 0) {
        cursor = next_boundary(buffer, cursor);
        --count;
    }
    return cursor;
}

[[nodiscard]]
std::size_t move_vertical(
    std::string_view buffer,
    std::size_t cursor,
    bool down
) noexcept
{
    const std::size_t current_start = line_start(buffer, cursor);
    const std::size_t current_end = line_end(buffer, cursor);
    const std::size_t column = character_count(buffer, current_start, cursor);

    if (down) {
        if (current_end == buffer.size()) {
            return cursor;
        }
        const std::size_t next_start = current_end + 1;
        const std::size_t next_end = line_end(buffer, next_start);
        return boundary_after_characters(
            buffer,
            next_start,
            next_end,
            std::min(column, character_count(buffer, next_start, next_end))
        );
    }

    if (current_start == 0) {
        return cursor;
    }
    const std::size_t previous_end = current_start - 1;
    const std::size_t previous_start = line_start(buffer, previous_end);
    return boundary_after_characters(
        buffer,
        previous_start,
        previous_end,
        std::min(column, character_count(buffer, previous_start, previous_end))
    );
}

}  // namespace

InputEditorState edit_input(
    std::string buffer,
    std::size_t cursor,
    const Key& key
)
{
    cursor = normalize_cursor(buffer, cursor);

    switch (key.kind) {
        case Key::Kind::Text:
            buffer.insert(cursor, key.text);
            cursor += key.text.size();
            break;

        case Key::Kind::Backspace:
            if (cursor > 0) {
                const std::size_t previous = previous_boundary(buffer, cursor);
                buffer.erase(previous, cursor - previous);
                cursor = previous;
            }
            break;

        case Key::Kind::Left:
            cursor = previous_boundary(buffer, cursor);
            break;

        case Key::Kind::Right:
            cursor = next_boundary(buffer, cursor);
            break;

        case Key::Kind::Home:
            cursor = line_start(buffer, cursor);
            break;

        case Key::Kind::End:
            cursor = line_end(buffer, cursor);
            break;

        case Key::Kind::Up:
            cursor = move_vertical(buffer, cursor, false);
            break;

        case Key::Kind::Down:
            cursor = move_vertical(buffer, cursor, true);
            break;

        case Key::Kind::Enter:
        case Key::Kind::ResumeQueue:
        case Key::Kind::ClearQueue:
        case Key::Kind::Interrupt:
        case Key::Kind::Eof:
            break;
    }

    return InputEditorState{
        .buffer = std::move(buffer),
        .cursor = cursor,
    };
}

}  // namespace my_agent::ui
