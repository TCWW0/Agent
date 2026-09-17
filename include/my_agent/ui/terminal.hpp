#pragma once

#include "my_agent/ui/screen.hpp"

#include <memory>
#include <string>
#include <string_view>

#include <termios.h>

namespace maya {
class FrameBuffer;
struct ScrollState;
}  // namespace maya

namespace my_agent::ui {

// 进入/退出全屏的字节。做成常量而非藏在驱动内部，是为了让「退出必须精确逆转进入」
// 这条不变量可以被断言 —— 顺序错了会让用户的 shell 丢掉光标。
[[nodiscard]]
std::string_view enter_bytes() noexcept;

[[nodiscard]]
std::string_view leave_bytes() noexcept;

// 唯一改 termios、唯一写终端字节的地方。RAII：析构一定还原，包括异常路径。
// 非 tty（管道、CI、重定向）不是错误 —— 构造照样成功，只是 is_tty() 为假，
// 前端据此回退到行式输出，而不是往文件里吐转义序列。
class TerminalDriver {
public:
    TerminalDriver(int input_fd, int output_fd);
    ~TerminalDriver();

    TerminalDriver(const TerminalDriver&) = delete;
    TerminalDriver& operator=(const TerminalDriver&) = delete;

    [[nodiscard]]
    bool is_tty() const noexcept;

    // 把这个驱动登记为崩溃时的还原目标，并挂上 SIGSEGV/SIGABRT/SIGBUS 等处理器。
    // 析构函数在这些信号下**不会**执行，而 raw mode 没还原意味着用户的 shell
    // 从此不回显 —— 得敲 reset 才能救回来。所以这条路径不能依赖 RAII。
    // 不放在构造函数里：登记全局状态与挂信号是进程级副作用，测试里要能构造
    // 驱动而不动进程的信号处理器。
    void install_crash_handler() noexcept;

    // 只用 async-signal-safe 的调用（write / tcsetattr / _exit）。
    // 供信号处理器调用，也可单测。
    static void restore_on_crash() noexcept;

    // 把一帧写到终端。返回是否**完整**写完 —— 调用方据此决定是否 commit。
    // 部分写时不 commit，front 才继续反映屏幕的真实内容，下一次差分才是完整的
    // 而不是空的（这个耦合是 maya 的 frame.hpp:109 记下的教训）。
    [[nodiscard]]
    bool render(const Frame& frame) noexcept;

    // 新语义屏幕的运行时入口。ScreenConfig 已经把 transcript 与 dock 分开；
    // 驱动只负责整屏组合、终端尺寸以及 write/commit 边界。
    [[nodiscard]]
    bool render(const ScreenConfig& screen) noexcept;

    // 查询终端尺寸。失败时返回 80x24 —— 管道里没有尺寸，但仍需要一个宽度来折行。
    [[nodiscard]]
    Size size() const noexcept;

    // 键盘所在的 fd，供调用方放进 poll 集合。必须由驱动给出而不是让循环假定
    // STDIN_FILENO —— 假定会让驱动持有的 fd 被忽略，测试也无从在 pty 上验证。
    [[nodiscard]]
    int input_fd() const noexcept;

private:
    int input_fd_;
    int output_fd_;
    bool is_tty_;
    // 进入 raw mode 之前的 termios，析构时原样写回。
    termios saved_termios_{};
    std::unique_ptr<maya::FrameBuffer> framebuffer_;
    std::unique_ptr<maya::ScrollState> transcript_scroll_;
};

}  // namespace my_agent::ui
