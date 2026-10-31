#pragma once

#include "my_agent/ui/size.hpp"

#include <string_view>

#include <termios.h>

namespace my_agent::ui {

[[nodiscard]]
std::string_view enter_bytes() noexcept;

[[nodiscard]]
std::string_view leave_bytes() noexcept;

// Owns the real terminal session. It is the only UI object that changes
// termios, enters/leaves the alternate screen, queries the terminal size, or
// writes bytes to the terminal.
class TerminalSession {
public:
    TerminalSession(int input_fd, int output_fd);
    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    [[nodiscard]] bool is_tty() const noexcept;
    [[nodiscard]] int input_fd() const noexcept;
    [[nodiscard]] Size size() const noexcept;

    [[nodiscard]] bool write_all(std::string_view bytes) const noexcept;

    void install_crash_handler() noexcept;
    static void restore_on_crash() noexcept;

private:
    int input_fd_;
    int output_fd_;
    bool is_tty_;
    termios saved_termios_{};
};

}  // namespace my_agent::ui
