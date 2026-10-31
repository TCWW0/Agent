#include "my_agent/ui/input.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>

namespace my_agent::ui {

namespace {

constexpr std::string_view kBracketedPasteStart = "\x1b[200~";
constexpr std::string_view kBracketedPasteEnd = "\x1b[201~";

[[nodiscard]]
std::size_t suffix_that_is_prefix_of(
    std::string_view text, std::string_view marker) noexcept
{
    const std::size_t maximum = std::min(text.size(), marker.size() - 1);
    for (std::size_t length = maximum; length > 0; --length) {
        if (text.substr(text.size() - length)
            == marker.substr(0, length)) {
            return length;
        }
    }
    return 0;
}

// 从前导字节读出这个 UTF-8 序列该有多长。非法前导返回 1 —— 按 1 字节消费掉，
// 保证循环一定推进而不是卡在同一个字节上。
[[nodiscard]]
std::size_t utf8_length(unsigned char lead) noexcept
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

// 控制字节到按键。返回 Text 表示「不是控制键」。
// 回车是 \r 而不是 \n：raw mode 关掉了 ICRNL。认错会让回车毫无反应。
// \n 也一起认，因为管道喂进来的输入没经过 termios。
[[nodiscard]]
Key::Kind control_kind(unsigned char byte) noexcept
{
    switch (byte) {
        case '\r':
        case '\n':
            return Key::Kind::Enter;
        case 0x7f:  // DEL
        case 0x08:  // BS，部分终端发这个
            return Key::Kind::Backspace;
        case 0x03:
            return Key::Kind::Interrupt;  // Ctrl-C：按键而非信号，见 ISIG 已关闭
        case 0x15:
            return Key::Kind::ClearInput;  // Ctrl-U：清空当前输入
        case 0x12:
            return Key::Kind::ResumeQueue;  // Ctrl-R：重试被阻塞的队列
        case 0x18:
            return Key::Kind::ClearQueue;  // Ctrl-X：清空尚未激活的排队输入
        case 0x04:
            return Key::Kind::Eof;  // Ctrl-D
        default:
            return Key::Kind::Text;
    }
}

struct EscapeResult {
    std::size_t consumed{0};
    std::optional<Key::Kind> kind;
    std::optional<MouseEvent> mouse;
};

constexpr std::size_t kMaxEscapeSequence = 256;

[[nodiscard]]
bool csi_tilde_key(
    std::string_view sequence, int code, int modifier = -1) noexcept
{
    const std::string prefix = std::to_string(code);
    if (sequence == prefix + "~") {
        return modifier == -1 || modifier == 1;
    }

    const std::string start = prefix + ";";
    if (sequence.size() <= start.size() || !sequence.starts_with(start)
        || sequence.back() != '~') {
        return false;
    }

    const std::string_view value = sequence.substr(
        start.size(), sequence.size() - start.size() - 1);
    int parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size()
        && (modifier == -1 || parsed == modifier);
}

[[nodiscard]]
std::optional<Key::Kind> escape_kind(std::string_view sequence) noexcept
{
    if (sequence == "A") {
        return Key::Kind::Up;
    }
    if (sequence == "B") {
        return Key::Kind::Down;
    }
    if (sequence == "C") {
        return Key::Kind::Right;
    }
    if (sequence == "D") {
        return Key::Kind::Left;
    }
    if (sequence == "H" || sequence == "1~" || sequence == "7~") {
        return Key::Kind::Home;
    }
    if (sequence == "F" || sequence == "4~" || sequence == "8~") {
        return Key::Kind::End;
    }
    if (csi_tilde_key(sequence, 5)) {
        return Key::Kind::PageUp;
    }
    if (csi_tilde_key(sequence, 6)) {
        return Key::Kind::PageDown;
    }
    if (sequence == "57360u") {
        return Key::Kind::Home;
    }
    if (sequence == "57361u") {
        return Key::Kind::End;
    }
    if (sequence == "57362u") {
        return Key::Kind::PageUp;
    }
    if (sequence == "57363u") {
        return Key::Kind::PageDown;
    }
    if (sequence == "1;5H" || csi_tilde_key(sequence, 1, 5)
        || sequence == "57360;5u") {
        return Key::Kind::ControlHome;
    }
    if (sequence == "1;5F" || csi_tilde_key(sequence, 4, 5)
        || sequence == "57361;5u") {
        return Key::Kind::ControlEnd;
    }
    return std::nullopt;
}

[[nodiscard]]
std::optional<MouseEvent> sgr_mouse(
    std::string_view parameters, unsigned char final_byte) noexcept
{
    // Field/button mapping follows Maya's SGR parser at revision 8da8707
    // (maya/src/terminal/input.cpp); this project owns the bounded decoder and
    // event types so it does not depend on Maya's application event loop.
    std::array<int, 3> values{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::size_t separator = parameters.find(';', start);
        const bool last = index + 1 == values.size();
        if ((!last && separator == std::string_view::npos)
            || (last && separator != std::string_view::npos)) {
            return std::nullopt;
        }

        const std::size_t end = last ? parameters.size() : separator;
        if (end == start) {
            return std::nullopt;
        }
        const std::string_view field = parameters.substr(start, end - start);
        const auto [parsed_end, error] = std::from_chars(
            field.data(), field.data() + field.size(), values[index]);
        if (error != std::errc{} || parsed_end != field.data() + field.size()) {
            return std::nullopt;
        }
        start = end + 1;
    }

    if (values[0] < 0 || values[1] < 1 || values[2] < 1) {
        return std::nullopt;
    }

    const int button_bits = values[0] & 0x43;
    MouseButton button = MouseButton::None;
    switch (button_bits) {
        case 0:  button = MouseButton::Left;       break;
        case 1:  button = MouseButton::Middle;     break;
        case 2:  button = MouseButton::Right;      break;
        case 64: button = MouseButton::ScrollUp;   break;
        case 65: button = MouseButton::ScrollDown; break;
        case 66: button = MouseButton::ScrollLeft; break;
        case 67: button = MouseButton::ScrollRight; break;
        default: break;
    }

    const MouseEventKind kind = final_byte == 'm'
        ? MouseEventKind::Release
        : (values[0] & 0x20) != 0 ? MouseEventKind::Move
                                  : MouseEventKind::Press;
    return MouseEvent{
        .button = button,
        .kind = kind,
        .x = values[1],
        .y = values[2],
        .modifiers = Modifiers{
            .ctrl = (values[0] & 0x10) != 0,
            .alt = (values[0] & 0x08) != 0,
            .shift = (values[0] & 0x04) != 0,
        },
    };
}

// Escape sequence parsing:
//
//                         ESC (0x1B)
//                              │
//                              ▼
//                           Escape
//                              │
//               ┌──────────────┼──────────────┐
//               │              │              │
//              'O'            '['           other
//               │              │              │
//               ▼              ▼              ▼
//             SS3             CSI        consume 2 bytes
//               │              │          (unsupported,TODO)
//               │              │
//               │              └── scan until final byte
//               │                  (0x40 <= byte <= 0x7E)
//               │                         │
//               │                         ▼
//               │                  consume whole sequence
//               │
//               └── consume fixed 3 bytes
//                   ESC O <key>
//
// Incomplete sequence:
//     consumed == 0 → keep buffer intact and wait for next feed().
[[nodiscard]]
EscapeResult decode_escape(std::string_view text) noexcept
{
    if (text.size() < 2) {
        return {};
    }
    // TODO(#44): the 2-byte consume is right for Alt+<ascii>, but it also eats
    // the lead byte of a multibyte character typed right after Esc.
    if (text[1] != '[' && text[1] != 'O') {
        return EscapeResult{.consumed = 2};
    }
    if (text[1] == 'O') {
        if (text.size() < 3) {
            return {};
        }
        return EscapeResult{
            .consumed = 3,
            .kind = escape_kind(text.substr(2, 1)),
        };
    }

    // SGR mouse reports use M/m as their only final bytes. Treat the
    // parameter body as opaque until that terminator so malformed numeric
    // fields such as "bad" cannot make the generic CSI scanner stop early
    // and leak the remainder as user text.
    if (text.size() >= 3 && text[2] == '<') {
        for (std::size_t index = 3; index < text.size(); ++index) {
            const auto byte = static_cast<unsigned char>(text[index]);
            if (byte != 'M' && byte != 'm') {
                continue;
            }
            const std::size_t consumed = index + 1;
            if (consumed > kMaxEscapeSequence) {
                return EscapeResult{.consumed = consumed};
            }
            const std::string_view sequence = text.substr(2, index - 1);
            return EscapeResult{
                .consumed = consumed,
                .mouse = sgr_mouse(
                    sequence.substr(1, sequence.size() - 2), byte),
            };
        }
        if (text.size() > kMaxEscapeSequence) {
            return EscapeResult{.consumed = text.size()};
        }
        return {};
    }

    for (std::size_t index = 2; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte >= 0x40 && byte <= 0x7E) {
            const std::size_t consumed = index + 1;
            if (consumed > kMaxEscapeSequence) {
                return EscapeResult{.consumed = consumed};
            }
            const std::string_view sequence = text.substr(2, index - 1);
            return EscapeResult{
                .consumed = consumed,
                .kind = escape_kind(sequence),
            };
        }
    }

    if (text.size() > kMaxEscapeSequence) {
        return EscapeResult{.consumed = text.size()};
    }
    return {};
}

}  // namespace

std::vector<Key> InputDecoder::feed(std::string_view bytes)
{
    std::vector<Key> keys;
    for (const InputEvent& event : feed_events(bytes)) {
        if (const auto* key = std::get_if<Key>(&event)) {
            keys.push_back(*key);
        }
    }
    return keys;
}

std::vector<InputEvent> InputDecoder::feed_events(std::string_view bytes)
{
    buffer_.append(bytes);

    std::vector<InputEvent> events;
    std::size_t offset = 0;
    while (offset < buffer_.size()) {
        if (in_bracketed_paste_) {
            const std::size_t end = buffer_.find(
                kBracketedPasteEnd, offset);
            if (end != std::string::npos) {
                paste_buffer_.append(buffer_, offset, end - offset);
                events.push_back(InputEvent{
                    PasteEvent{.text = std::move(paste_buffer_)},
                });
                paste_buffer_.clear();
                in_bracketed_paste_ = false;
                offset = end + kBracketedPasteEnd.size();
                continue;
            }

            const std::size_t available = buffer_.size() - offset;
            const std::size_t suffix = suffix_that_is_prefix_of(
                std::string_view{buffer_}.substr(offset),
                kBracketedPasteEnd);
            const std::size_t complete = available - suffix;
            paste_buffer_.append(buffer_, offset, complete);
            offset += complete;
            break;
        }

        if (std::string_view{buffer_}.substr(offset).starts_with(
                kBracketedPasteStart)) {
            in_bracketed_paste_ = true;
            offset += kBracketedPasteStart.size();
            continue;
        }

        const auto lead = static_cast<unsigned char>(buffer_[offset]);

        if (lead == 0x1b) {  // ESC：转义序列的开头
            const EscapeResult escape = decode_escape(
                std::string_view{buffer_}.substr(offset)
            );
            if (escape.consumed == 0) {
                break;  // 序列还没到齐，留到下一次 feed
            }
            if (escape.kind) {
                events.push_back(InputEvent{Key{.kind = *escape.kind}});
            }
            if (escape.mouse) {
                events.push_back(InputEvent{*escape.mouse});
            }
            offset += escape.consumed;
            continue;  // 本切片不支持方向键等，整段丢弃而不是当文本显示
        }

        if (const Key::Kind kind = control_kind(lead); kind != Key::Kind::Text) {
            events.push_back(InputEvent{Key{.kind = kind}});
            offset += 1;
            continue;
        }

        const std::size_t length = utf8_length(lead);

        // 序列还没到齐：留在缓冲里等下一次 feed。read 不保证在字符边界上切，
        // 现在吐出去就是半个汉字 —— 插进输入行之后再也删不干净。
        if (offset + length > buffer_.size()) {
            break;
        }

        events.push_back(InputEvent{Key{
            .kind = Key::Kind::Text,
            .text = buffer_.substr(offset, length),
        }});
        offset += length;
    }

    buffer_.erase(0, offset);
    return events;
}

}  // namespace my_agent::ui
