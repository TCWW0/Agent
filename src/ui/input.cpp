#include "my_agent/ui/input.hpp"

#include <cstddef>
#include <optional>

namespace my_agent::ui {

namespace {

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
        case 0x04:
            return Key::Kind::Eof;  // Ctrl-D
        default:
            return Key::Kind::Text;
    }
}

struct EscapeResult {
    std::size_t consumed{0};
    std::optional<Key::Kind> kind;
};

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
    return std::nullopt;
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
    for (std::size_t index = 2; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte >= 0x40 && byte <= 0x7E) {
            return EscapeResult{
                .consumed = index + 1,
                .kind = escape_kind(text.substr(2, index - 1)),
            };
        }
    }
    return {};
}

}  // namespace

std::vector<Key> InputDecoder::feed(std::string_view bytes)
{
    buffer_.append(bytes);

    std::vector<Key> keys;
    std::size_t offset = 0;
    while (offset < buffer_.size()) {
        const auto lead = static_cast<unsigned char>(buffer_[offset]);

        if (lead == 0x1b) {  // ESC：转义序列的开头
            const EscapeResult escape = decode_escape(
                std::string_view{buffer_}.substr(offset)
            );
            if (escape.consumed == 0) {
                break;  // 序列还没到齐，留到下一次 feed
            }
            if (escape.kind) {
                keys.push_back(Key{.kind = *escape.kind});
            }
            offset += escape.consumed;
            continue;  // 本切片不支持方向键等，整段丢弃而不是当文本显示
        }

        if (const Key::Kind kind = control_kind(lead); kind != Key::Kind::Text) {
            keys.push_back(Key{.kind = kind});
            offset += 1;
            continue;
        }

        const std::size_t length = utf8_length(lead);

        // 序列还没到齐：留在缓冲里等下一次 feed。read 不保证在字符边界上切，
        // 现在吐出去就是半个汉字 —— 插进输入行之后再也删不干净。
        if (offset + length > buffer_.size()) {
            break;
        }

        keys.push_back(Key{
            .kind = Key::Kind::Text,
            .text = buffer_.substr(offset, length),
        });
        offset += length;
    }

    buffer_.erase(0, offset);
    return keys;
}

}  // namespace my_agent::ui
