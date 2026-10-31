#include "my_agent/ui/terminal_session.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>

#include <sys/ioctl.h>
#include <unistd.h>

namespace my_agent::ui {

namespace {

std::atomic<bool> g_crash_armed{false};
std::atomic<int> g_crash_fd{-1};
std::atomic<int> g_crash_out_fd{-1};
termios g_crash_termios{};

[[nodiscard]]
bool write_all_fd(int fd, std::string_view bytes) noexcept
{
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

extern "C" void crash_signal_handler(int signal_number)
{
    TerminalSession::restore_on_crash();
    std::raise(signal_number);
}

}  // namespace

std::string_view enter_bytes() noexcept
{
    return "\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[?2004h";
}

std::string_view leave_bytes() noexcept
{
    return "\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?7h\x1b[?1049l";
}

TerminalSession::TerminalSession(int input_fd, int output_fd)
    : input_fd_{input_fd},
      output_fd_{output_fd},
      is_tty_{::isatty(input_fd) == 1 && ::isatty(output_fd) == 1}
{
    if (!is_tty_) {
        return;
    }
    if (::tcgetattr(input_fd_, &saved_termios_) != 0) {
        is_tty_ = false;
        return;
    }

    termios raw = saved_termios_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | INLCR | ISTRIP));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(input_fd_, TCSAFLUSH, &raw) != 0) {
        is_tty_ = false;
        return;
    }

    static_cast<void>(write_all_fd(output_fd_, enter_bytes()));
}

TerminalSession::~TerminalSession()
{
    if (!is_tty_) {
        return;
    }
    static_cast<void>(write_all_fd(output_fd_, leave_bytes()));
    ::tcsetattr(input_fd_, TCSAFLUSH, &saved_termios_);
    g_crash_armed.store(false, std::memory_order_release);
}

bool TerminalSession::is_tty() const noexcept
{
    return is_tty_;
}

int TerminalSession::input_fd() const noexcept
{
    return input_fd_;
}

Size TerminalSession::size() const noexcept
{
    winsize window{};
    if (::ioctl(output_fd_, TIOCGWINSZ, &window) == 0
        && window.ws_col > 0 && window.ws_row > 0) {
        return Size{
            .columns = static_cast<int>(window.ws_col),
            .rows = static_cast<int>(window.ws_row),
        };
    }
    return Size{.columns = 80, .rows = 24};
}

bool TerminalSession::write_all(std::string_view bytes) const noexcept
{
    return write_all_fd(output_fd_, bytes);
}

void TerminalSession::install_crash_handler() noexcept
{
    if (!is_tty_) {
        return;
    }
    g_crash_fd.store(input_fd_, std::memory_order_relaxed);
    g_crash_out_fd.store(output_fd_, std::memory_order_relaxed);
    g_crash_termios = saved_termios_;
    g_crash_armed.store(true, std::memory_order_release);

    struct sigaction action{};
    action.sa_handler = &crash_signal_handler;
    action.sa_flags = SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);
    for (const int signal_number : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        ::sigaction(signal_number, &action, nullptr);
    }
}

void TerminalSession::restore_on_crash() noexcept
{
    if (!g_crash_armed.load(std::memory_order_acquire)) {
        return;
    }
    const std::string_view leave = leave_bytes();
    ::write(g_crash_out_fd.load(std::memory_order_relaxed), leave.data(), leave.size());
    ::tcsetattr(
        g_crash_fd.load(std::memory_order_relaxed), TCSAFLUSH, &g_crash_termios
    );
}

}  // namespace my_agent::ui
