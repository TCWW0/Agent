#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace my_agent::ui {

// 一次按键。text 非空表示可见字符（可能是多字节的 UTF-8），否则看 kind。
struct Key {
    enum class Kind {
        Text,
        Enter,
        Backspace,
        ClearInput,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        ControlHome,
        ControlEnd,
        Interrupt,  // Ctrl-C
        ResumeQueue, // Ctrl-R: retry a blocked queue
        ClearQueue,  // Ctrl-X: drop every prompt still waiting in the queue
        Eof,        // Ctrl-D
    };

    Kind kind{Kind::Text};
    std::string text;
};

struct Modifiers {
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
};

enum class MouseButton {
    Left,
    Middle,
    Right,
    ScrollUp,
    ScrollDown,
    ScrollLeft,
    ScrollRight,
    None,
};

enum class MouseEventKind {
    Press,
    Release,
    Move,
};

// SGR coordinates stay 1-based, matching the terminal protocol. The
// viewport adapter converts them to the renderer's 0-based cell rectangle.
struct MouseEvent {
    MouseButton button{MouseButton::None};
    MouseEventKind kind{MouseEventKind::Press};
    int x{1};
    int y{1};
    Modifiers modifiers;
};

// Bracketed paste is a semantic input event, not a sequence of Enter keys.
// Its body is kept verbatim here; the composer decides how to render and
// submit it.
struct PasteEvent {
    std::string text;
};

using InputEvent = std::variant<Key, MouseEvent, PasteEvent>;

// 字节流 -> 按键。有状态：一个 UTF-8 字符可能跨两次 read 到达（中文占 3 字节，
// 而 read 只保证返回「已就绪的字节」），残缺序列必须留到下一次 feed 缝合，
// 否则会把半个汉字当成非法字节吐出去。
class InputDecoder {
public:
    [[nodiscard]]
    std::vector<Key> feed(std::string_view bytes);

    [[nodiscard]]
    std::vector<InputEvent> feed_events(std::string_view bytes);

private:
    std::string buffer_;
    bool in_bracketed_paste_{false};
    std::string paste_buffer_;
};

}  // namespace my_agent::ui
