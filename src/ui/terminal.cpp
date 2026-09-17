#include "my_agent/ui/terminal.hpp"

#include "my_agent/ui/maya_projection.hpp"
#include "my_agent/ui/screen.hpp"

#include <maya/dsl.hpp>
#include <maya/core/scroll_state.hpp>
#include <maya/element/builder.hpp>
#include <maya/render/frame.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/conversation.hpp>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <memory>

#include <sys/ioctl.h>
#include <unistd.h>

namespace my_agent::ui {

namespace {

// 循环写完，EINTR 当重试而不是失败。返回是否全部写出 —— 调用方据此决定是否
// commit 已渲染状态，谎报成功会让后续差分建立在假前提上。
[[nodiscard]]
bool write_all(int fd, std::string_view bytes) noexcept
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

// 崩溃还原用的全局状态。信号处理器不能访问对象（this 可能已损坏）、不能加锁
// （可能已持有）、不能分配（可能正崩在 malloc 里），所以只能靠这几个平坦的全局量。
std::atomic<bool> g_crash_armed{false};
std::atomic<int> g_crash_fd{-1};
std::atomic<int> g_crash_out_fd{-1};
// 非 atomic：termios 是聚合体，没有无锁的原子版本。armed 标志的 release/acquire
// 保证处理器读到它时内容已写完，而它在 armed 之后不再改动。
termios g_crash_termios{};

extern "C" void crash_signal_handler(int signal_number)
{
    TerminalDriver::restore_on_crash();
    // 重新发一次。SA_RESETHAND 已把处理器复位成默认动作，所以这次会真正终止进程，
    // 留下正确的退出状态和 core —— 而不是让崩溃被静默吞掉。
    std::raise(signal_number);
}

}  // namespace

// 1049 是「切备用屏并存光标位置」的组合，比老的 47 + 独立存光标少一次往返。
// 备用屏而非 inline：inline 要精确记账滚出屏幕的物理行数，那是正确性问题；
// 备用屏的代价（退出后历史消失）只是体验问题。
//
// ?7l 关 DECAWM（自动换行）：这是纵深防御，不针对任何当前已知缺陷。已知的两条幽灵行
// 成因都已在 #13/#14 修掉（宽度表补齐 + 填满行跳过 EL），此时输入行按真实宽度裁剪，
// 够不到右边距，DECAWM 无从咬起。这条防的是**未来**：宽度表跟不上 Unicode 新分配时，
// 漏网字符会被欠算、把行推过右边距 —— DECAWM 关掉后光标钉在末列原地覆写而非滚屏，
// 幽灵行退化成末格被覆盖这种局部瑕疵，而不是整屏错位。Maya 的序列化层负责 cell
// diff、EL 保护与宽度处理；本驱动只在会话边界管理终端模式。
std::string_view enter_bytes() noexcept
{
    return "\x1b[?1049h\x1b[?7l\x1b[?25l";
}

// 精确逆转 enter_bytes，且顺序相反。先显光标、再开 DECAWM、最后切回主屏 —— 反过来
// 可能让主屏留着隐藏的光标或关着的 autowrap，用户的 shell 从此看不见自己在打什么、
// 或长命令不换行。
std::string_view leave_bytes() noexcept
{
    return "\x1b[?25h\x1b[?7h\x1b[?1049l";
}

TerminalDriver::TerminalDriver(int input_fd, int output_fd)
    : input_fd_{input_fd},
      output_fd_{output_fd},
      // 两端都必须是 tty。只有输出是 tty 时（`cat file | my_agent`）改不了输入的
      // termios，读键盘的那套逻辑无从工作，只能整体回退。
      is_tty_{::isatty(input_fd) == 1 && ::isatty(output_fd) == 1},
      framebuffer_{std::make_unique<maya::FrameBuffer>()},
      transcript_scroll_{std::make_unique<maya::ScrollState>()}
{
    if (!is_tty_) {
        return;
    }
    if (::tcgetattr(input_fd_, &saved_termios_) != 0) {
        is_tty_ = false;  // 拿不到原始状态就不敢改 —— 改了就还不回去
        return;
    }

    termios raw = saved_termios_;
    // 关回显与行缓冲：逐键处理的前提。关 ISIG 让 Ctrl-C 作为字节到达，
    // 由前端决定含义（中断请求而不是杀进程），否则备用屏来不及还原。
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
    // 关 IXON 让 Ctrl-S/Ctrl-Q 不被终端吞掉；关 ICRNL 让回车保持 \r。
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | INLCR | ISTRIP));
    // 关 OPOST：输出不再自动 \n -> \r\n，这正是行定位必须显式 CUP 的原因。
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    // VMIN=0/VTIME=0 让 read 立即返回 —— 阻塞由 poll 负责，read 只负责取走
    // 已经就绪的字节。VMIN=1 会让 read 在 poll 之后再次阻塞，UI 线程卡死。
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(input_fd_, TCSAFLUSH, &raw) != 0) {
        is_tty_ = false;
        return;
    }

    // 写不出去也继续：termios 已经设好，能读键盘。屏幕的事下一帧 render 会再试，
    // 而它的返回值调用方看得到 —— 这里没人能处理。
    static_cast<void>(write_all(output_fd_, enter_bytes()));
}

TerminalDriver::~TerminalDriver()
{
    if (!is_tty_) {
        return;
    }
    // 顺序：先还原屏幕再还原 termios。反过来的话，还原 termios 之后 OPOST 又开着，
    // 后面那串转义序列会被终端加工（\n -> \r\n），可能被截断。
    static_cast<void>(write_all(output_fd_, leave_bytes()));
    // 即使屏幕没还原成功也要还 termios：不回显的 shell 比留在备用屏上更难恢复。
    ::tcsetattr(input_fd_, TCSAFLUSH, &saved_termios_);
}

bool TerminalDriver::is_tty() const noexcept
{
    return is_tty_;
}

int TerminalDriver::input_fd() const noexcept
{
    return input_fd_;
}

bool TerminalDriver::render(const Frame& frame) noexcept
{
    const Size current_size = size();
    if (framebuffer_->width() != current_size.columns
        || framebuffer_->height() != current_size.rows) {
        framebuffer_->resize(current_size.columns, current_size.rows);
    }

    const int max_column = std::max(0, current_size.columns - 1);
    const int max_row = std::max(0, current_size.rows - 1);
    if (frame.cursor) {
        framebuffer_->set_cursor(maya::Position{
            maya::Columns{std::clamp(frame.cursor->column, 0, max_column)},
            maya::Rows{std::clamp(frame.cursor->row, 0, max_row)},
        });
        framebuffer_->set_cursor_visible(true);
    } else {
        framebuffer_->set_cursor_visible(false);
    }

    const maya::Theme& theme = maya::theme::dark;
    const std::string& bytes =
        framebuffer_->render(to_maya_element(frame, theme), theme);
    if (!write_all(output_fd_, bytes)) {
        // Maya render() does not swap buffers. Skipping commit on write failure
        // keeps front_ aligned with the terminal's last successful frame, so
        // the next render produces a complete diff instead of losing content.
        return false;
    }
    framebuffer_->commit();
    return true;
}

bool TerminalDriver::render(const ScreenConfig& screen) noexcept
{
    const Size current_size = size();
    if (framebuffer_->width() != current_size.columns
        || framebuffer_->height() != current_size.rows) {
        framebuffer_->resize(current_size.columns, current_size.rows);
    }

    // Composer 使用应用绘制的 SolidCell caret；硬件光标继续隐藏，避免同屏双光标。
    framebuffer_->set_cursor_visible(false);

    const bool follow_tail = transcript_scroll_->at_bottom();
    const auto build_screen = [&]() {
        using namespace maya::dsl;

        // agentty 的 Conversation 在 Inline 模式下靠自然高度进入终端 scrollback；
        // 本项目保留备用屏，因此对应物是一个固定 viewport：Conversation 在 scrolly
        // 内按自然高度布局，pane 吸收 dock 之外的剩余高度，dock 则禁止收缩。
        maya::Element conversation = (
            maya::Conversation{screen.transcript}.build()
                | width(current_size.columns)
        ).build();
        maya::Element scroll_content = v(std::move(conversation)).build();
        maya::Element transcript = maya::detail::vstack()
            .grow(1.0f)
            .shrink(1.0f)
            .min_height(maya::Dimension::fixed(0))
            .overflow(maya::Overflow::Hidden)
            (std::move(scroll_content)
                | scrolly(*transcript_scroll_, 0)
                | grow(1.0f));

        return maya::detail::vstack()
            .width(maya::Dimension::fixed(current_size.columns))
            .height(maya::Dimension::fixed(current_size.rows))
            .overflow(maya::Overflow::Hidden)
            (
                std::move(transcript),
                // 与 agentty 的 AppLayout 一样，让父级 cross-axis stretch
                // 分配可用宽度。根节点已经给出整屏宽度，dock 只声明自己的
                // 左右 gutter；这里不重复制造第二份宽度边界。
                dock_element(screen.dock)
                    | shrink(0.0f)
            );
    };

    const maya::Theme& theme = maya::theme::dark;
    maya::Element element = build_screen();
    const std::string* bytes = &framebuffer_->render(element, theme);

    // 第一次 paint 会把新的 max_y 写回 ScrollState。用户原本位于末尾时，内容增长
    // 后立即跟随到新末尾并重建一次树；否则尊重未来的手动回看位置。
    if (follow_tail && !transcript_scroll_->at_bottom()) {
        transcript_scroll_->scroll_to_bottom();
        element = build_screen();
        bytes = &framebuffer_->render(element, theme);
    }

    if (!write_all(output_fd_, *bytes)) {
        return false;
    }
    framebuffer_->commit();
    return true;
}

Size TerminalDriver::size() const noexcept
{
    winsize window{};
    if (::ioctl(output_fd_, TIOCGWINSZ, &window) == 0 && window.ws_col > 0
        && window.ws_row > 0) {
        return Size{
            .columns = static_cast<int>(window.ws_col),
            .rows = static_cast<int>(window.ws_row),
        };
    }
    // 管道没有尺寸，但折行仍然需要一个宽度 —— 0 列会让 wrap 什么都不吐，界面全空。
    return Size{.columns = 80, .rows = 24};
}

void TerminalDriver::install_crash_handler() noexcept
{
    if (!is_tty_) {
        return;
    }
    // 信号处理器只能碰这几个 volatile 全局量：它不能加锁（可能已持有），
    // 不能分配（可能崩在 malloc 里），也不能访问对象（this 可能已损坏）。
    g_crash_fd.store(input_fd_, std::memory_order_relaxed);
    g_crash_out_fd.store(output_fd_, std::memory_order_relaxed);
    g_crash_termios = saved_termios_;
    g_crash_armed.store(true, std::memory_order_release);

    struct sigaction action{};
    action.sa_handler = &crash_signal_handler;
    // SA_RESETHAND：处理器只跑一次，之后恢复默认。还原完再重发信号时才会
    // 真正终止进程并留下正确的退出状态/core，而不是重入处理器死循环。
    action.sa_flags = SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);
    for (const int signal_number : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        ::sigaction(signal_number, &action, nullptr);
    }
}

void TerminalDriver::restore_on_crash() noexcept
{
    if (!g_crash_armed.load(std::memory_order_acquire)) {
        return;
    }
    const std::string_view leave = leave_bytes();
    // 直接 write 而不走 write_all：短写时宁可少写几个字节，也不要在崩溃路径上
    // 循环。write 与 tcsetattr 都是 async-signal-safe。
    ::write(g_crash_out_fd.load(std::memory_order_relaxed), leave.data(), leave.size());
    ::tcsetattr(
        g_crash_fd.load(std::memory_order_relaxed), TCSAFLUSH, &g_crash_termios
    );
}

}  // namespace my_agent::ui
