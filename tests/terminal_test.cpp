#include "my_agent/ui/terminal.hpp"

#include "virtual_terminal.hpp"

#include <cerrno>
#include <cstddef>
#include <csignal>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

struct PipeFds {
    int read_fd = -1;
    int write_fd = -1;

    PipeFds() = default;

    PipeFds(const PipeFds&) = delete;
    PipeFds& operator=(const PipeFds&) = delete;

    ~PipeFds()
    {
        close_read();
        close_write();
    }

    [[nodiscard]]
    bool open() noexcept
    {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            return false;
        }
        read_fd = fds[0];
        write_fd = fds[1];
        return true;
    }

    void close_read() noexcept
    {
        if (read_fd != -1) {
            ::close(read_fd);
            read_fd = -1;
        }
    }

    void close_write() noexcept
    {
        if (write_fd != -1) {
            ::close(write_fd);
            write_fd = -1;
        }
    }
};

[[nodiscard]]
std::string read_available(int fd)
{
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            bytes.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return bytes;
        }
        return bytes;
    }
}

[[nodiscard]]
bool make_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

[[nodiscard]]
bool fill_until_would_block(int fd)
{
    const std::string chunk(4096, 'x');
    for (;;) {
        const ssize_t count = ::write(fd, chunk.data(), chunk.size());
        if (count > 0) {
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
}

// 场景：进入与退出全屏的字节序列。
// 领域语义：退出序列必须是进入序列的**精确逆操作，且顺序相反**。进入是
// 「切备用屏 → 藏光标」，退出必须是「显光标 → 回主屏」。顺序反了会把光标显示在
// 备用屏上、再切回主屏 —— 主屏的光标状态取决于终端实现，可能留下隐藏的光标，
// 用户的 shell 从此看不见自己在打什么。这是崩溃恢复要恢复的东西，必须先钉死顺序。
// Red 原因：仓库尚不存在 enter_bytes / leave_bytes（编译错误）。
TEST(TerminalTest, LeaveSequenceReversesEnterSequenceInOppositeOrder)
{
    const std::string enter{my_agent::ui::enter_bytes()};
    const std::string leave{my_agent::ui::leave_bytes()};

    const std::string alt_screen_on = "\x1b[?1049h";
    const std::string alt_screen_off = "\x1b[?1049l";
    const std::string cursor_hide = "\x1b[?25l";
    const std::string cursor_show = "\x1b[?25h";
    // DECAWM（自动换行）：进入关、退出开。#14 加进来的纵深防御，若不在这里断言其
    // 对称，未来有人把 leave 的 ?7h 删掉或改序，这条「退出精确逆转进入」的不变量
    // 会静默破掉 —— 破坏验证已证实此前没有任何测试守着它。
    const std::string autowrap_off = "\x1b[?7l";
    const std::string autowrap_on = "\x1b[?7h";

    ASSERT_NE(std::string::npos, enter.find(alt_screen_on));
    ASSERT_NE(std::string::npos, enter.find(autowrap_off));
    ASSERT_NE(std::string::npos, enter.find(cursor_hide));
    ASSERT_NE(std::string::npos, leave.find(cursor_show));
    ASSERT_NE(std::string::npos, leave.find(autowrap_on));
    ASSERT_NE(std::string::npos, leave.find(alt_screen_off));

    // 进入：切备用屏 → 关 autowrap → 藏光标。
    EXPECT_LT(enter.find(alt_screen_on), enter.find(autowrap_off));
    EXPECT_LT(enter.find(autowrap_off), enter.find(cursor_hide));
    // 退出：显光标 → 开 autowrap → 回主屏。逐段都是进入的镜像。
    EXPECT_LT(leave.find(cursor_show), leave.find(autowrap_on));
    EXPECT_LT(leave.find(autowrap_on), leave.find(alt_screen_off));
}

// 场景：在非 tty 上构造驱动（CI、管道、`my_agent | tee`）。
// 领域语义：非 tty 不是错误，是一种正常的运行方式 —— 但 termios 和转义序列在那里
// 都没有意义。驱动必须能构造成功且自报 `!is_tty()`，让前端据此回退到行式输出。
// 反过来如果构造失败或照样吐转义序列，重定向到文件的输出里就会混进一堆乱码。
// Red 原因：仓库尚不存在 TerminalDriver（编译错误）。
TEST(TerminalTest, ReportsNonTtyInsteadOfFailingWhenOutputIsNotATerminal)
{
    const int devnull = ::open("/dev/null", O_WRONLY);
    ASSERT_LE(0, devnull);

    {
        my_agent::ui::TerminalDriver driver{devnull, devnull};
        EXPECT_FALSE(driver.is_tty());
    }

    ::close(devnull);
}

// 场景：在真 pty 上进入 raw mode 然后析构。
// 领域语义：这是整个 TUI 里唯一会**损坏用户环境**的失效。raw mode 关掉了 ECHO 和
// ICANON，没还原的话退出后 shell 不回显、退格不工作，用户得敲 `reset` 才能救回来。
// 所以驱动必须真的进 raw mode（否则读不到单键），并且析构后 termios 必须逐字段
// 等于进入前的样子。前面几条测试都在断言字节串，这条断言的是**系统状态**。
// Red 原因：驱动虽已实现，但这是第一条真正验证还原的测试 —— 它保护的不变量
// 无法从字节串断言，必须要一个真 tty。
TEST(TerminalTest, RestoresTerminalAttributesExactlyOnDestruction)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    termios before{};
    ASSERT_EQ(0, ::tcgetattr(replica, &before));

    termios inside{};
    {
        my_agent::ui::TerminalDriver driver{replica, replica};
        ASSERT_TRUE(driver.is_tty());
        ASSERT_EQ(0, ::tcgetattr(replica, &inside));
        // 真的进了 raw mode：回显与行缓冲都关掉了，否则读不到单个按键。
        EXPECT_EQ(0u, inside.c_lflag & static_cast<tcflag_t>(ECHO));
        EXPECT_EQ(0u, inside.c_lflag & static_cast<tcflag_t>(ICANON));
    }

    termios after{};
    ASSERT_EQ(0, ::tcgetattr(replica, &after));
    EXPECT_EQ(before.c_lflag, after.c_lflag);
    EXPECT_EQ(before.c_iflag, after.c_iflag);
    EXPECT_EQ(before.c_oflag, after.c_oflag);
    EXPECT_EQ(before.c_cc[VMIN], after.c_cc[VMIN]);
    EXPECT_EQ(before.c_cc[VTIME], after.c_cc[VTIME]);

    ::close(replica);
    ::close(primary);
}

// 场景：进入 raw mode 之后进程被 SIGSEGV 杀掉。
// 领域语义：析构函数在 SIGSEGV 下不执行，所以 RAII 保护不了这条路径 —— 而这正是
// 唯一会把用户的 shell 弄坏的路径（raw mode 没还原 = 不回显、退格不工作，
// 得敲 reset）。所以必须有一条不依赖栈展开的还原路径。
// 用 fork：子进程进 raw mode 后故意 SIGSEGV，父进程检查共享的 pty 是否已还原。
// 这是唯一能真正验证「崩溃后终端可用」的做法 —— 在同进程里断言只能测到函数本身，
// 测不到它确实被信号触发。
// Red 原因：仓库尚不存在 install_crash_handler（编译错误）。
TEST(TerminalTest, RestoresTerminalWhenTheProcessCrashesWithoutUnwinding)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    termios before{};
    ASSERT_EQ(0, ::tcgetattr(replica, &before));

    const pid_t child = ::fork();
    ASSERT_LE(0, child);
    if (child == 0) {
        my_agent::ui::TerminalDriver driver{replica, replica};
        driver.install_crash_handler();
        // 故意崩溃。析构函数不会跑，只有信号处理器有机会还原。
        std::raise(SIGSEGV);
        ::_exit(0);  // 到不了这里
    }

    int status = 0;
    ASSERT_EQ(child, ::waitpid(child, &status, 0));

    termios after{};
    ASSERT_EQ(0, ::tcgetattr(replica, &after));
    EXPECT_EQ(before.c_lflag, after.c_lflag);
    EXPECT_EQ(before.c_iflag, after.c_iflag);
    EXPECT_EQ(before.c_oflag, after.c_oflag);

    ::close(replica);
    ::close(primary);
}

// 场景：往一个已经写不进去的 fd 上渲染。
// 领域语义：`render()` 必须报告是否**完整**写完，因为调用方要据此决定是否
// commit 已渲染状态。写失败却报成功，front 就会声称屏幕上有实际没写进去的内容，
// 之后每一次差分都建立在假前提上，而且这种残留不会自愈。
// 用管道模拟：读端关掉后写入立即失败。
// Red 原因：仓库尚不存在 render()（编译错误）。
TEST(TerminalTest, RenderReportsFailureSoTheCallerDoesNotCommitAnUnwrittenFrame)
{
    PipeFds pipe;
    ASSERT_TRUE(pipe.open());
    pipe.close_read();  // 读端关掉：写入会拿到 EPIPE

    // SIGPIPE 默认会杀掉进程，这里要的是 write 返回 -1。
    const auto previous = std::signal(SIGPIPE, SIG_IGN);

    my_agent::ui::TerminalDriver driver{pipe.write_fd, pipe.write_fd};
    const my_agent::ui::Frame frame{.lines = {{.text = "content"}}};

    EXPECT_FALSE(driver.render(frame));

    std::signal(SIGPIPE, previous);
}

// 场景：同一帧连续两次成功渲染。
// 领域语义：TerminalDriver::render 是 Maya FrameBuffer 的提交边界。第一次写成功后必须
// commit，front_ 才代表终端真实画面；第二次渲染相同 Frame 时 Maya diff 不应重画正文。
// Red 原因：旧自有字节生成路径每次都全帧输出，因此第二次仍会包含哨兵正文。
TEST(TerminalTest, RenderCommitsSuccessfulMayaFrameSoIdenticalFrameDoesNotRedrawContent)
{
    constexpr const char* kSentinel = "TERMINALMAYACOMMIT7F3A";

    PipeFds pipe;
    ASSERT_TRUE(pipe.open());
    ASSERT_TRUE(make_nonblocking(pipe.read_fd));

    my_agent::ui::TerminalDriver driver{pipe.write_fd, pipe.write_fd};
    const my_agent::ui::Frame frame{.lines = {{.text = kSentinel}}};

    ASSERT_TRUE(driver.render(frame));
    const std::string first = read_available(pipe.read_fd);
    ASSERT_NE(std::string::npos, first.find(kSentinel)) << first;

    ASSERT_TRUE(driver.render(frame));
    const std::string second = read_available(pipe.read_fd);
    EXPECT_EQ(std::string::npos, second.find(kSentinel)) << second;
}

TEST(TerminalTest, RendersTheFrameCursorAtTheInputPosition)
{
    PipeFds pipe;
    ASSERT_TRUE(pipe.open());
    ASSERT_TRUE(make_nonblocking(pipe.read_fd));

    my_agent::ui::TerminalDriver driver{pipe.write_fd, pipe.write_fd};
    const my_agent::ui::Frame frame{
        .lines = {{.text = "input"}, {.text = "tail"}},
        .cursor = my_agent::ui::CursorPosition{.row = 1, .column = 2},
    };

    ASSERT_TRUE(driver.render(frame));
    const std::string bytes = read_available(pipe.read_fd);
    EXPECT_NE(std::string::npos, bytes.find("\x1b[?25h")) << bytes;
    EXPECT_NE(std::string::npos, bytes.find("\x1b[2;3H")) << bytes;
}

// 场景：Maya 已经算出这一帧，但输出 fd 当时写不进去。
// 领域语义：write 失败时必须跳过 commit。否则 front_ 会前进到终端从没显示过的内容，
// 清空管道后重试同一帧就会变成空 diff，哨兵永久丢失。
TEST(TerminalTest, RenderSkipsMayaCommitWhenTheWriteFails)
{
    constexpr const char* kSentinel = "TERMINALMAYARETRY7F3A";

    PipeFds pipe;
    ASSERT_TRUE(pipe.open());
    ASSERT_TRUE(make_nonblocking(pipe.read_fd));
    ASSERT_TRUE(make_nonblocking(pipe.write_fd));
    ASSERT_TRUE(fill_until_would_block(pipe.write_fd));

    my_agent::ui::TerminalDriver driver{pipe.write_fd, pipe.write_fd};
    const my_agent::ui::Frame frame{.lines = {{.text = kSentinel}}};

    ASSERT_FALSE(driver.render(frame));
    static_cast<void>(read_available(pipe.read_fd));

    ASSERT_TRUE(driver.render(frame));
    const std::string retry = read_available(pipe.read_fd);
    EXPECT_NE(std::string::npos, retry.find(kSentinel)) << retry;

    ASSERT_TRUE(driver.render(frame));
    const std::string settled = read_available(pipe.read_fd);
    EXPECT_EQ(std::string::npos, settled.find(kSentinel)) << settled;
}

// 场景：备用屏里终端从 20 列加宽到 40 列，之后渲染同一块语义屏。
// 领域语义：差分的全部前提是「front == 终端像素」。尺寸变化恰好摧毁它——驱动
// 把 framebuffer 换到新尺寸时 front 被抹成空白，diff 于是判定「新帧是空格、front
// 也是空格、没变、跳过」，而物理屏上那些格子还留着旧宽度的内容（旧右边框、旧
// 列块）：跳过即永久残留，这就是幽灵单元格。更糟的是连行尾 EL 都不发——trailing
// EL 只在「front 在行尾有内容」时触发，抹白后的 front 恒为空白，need_clear 恒假。
// 所以尺寸一变，驱动与终端之间唯一成立的事实是「屏上有旧内容、范围未知」，
// 必须放弃差分改走全量序列化（逐行重画 + 每行擦尾），写成功后才重新拥有可差分
// 的 front。
// 断言是「脏屏收敛」：同一份帧字节分别喂给带着旧内容的终端模型和空白终端模型，
// 最终网格必须一致——全量序列化是唯一能让脏屏收敛的输出形状，任何残留都会让
// 两个网格分叉。终端模型用保留式 resize（驱动关 DECAWM，真实终端 resize 不重排
// 只截断/补白），「旧内容还在」是被建模的事实而非假设。
// Red 原因：render() 在尺寸变化时把 framebuffer resize 后照常差分，diff 在结构上
// 无法表达擦除——脏屏不收敛，旧宽度的边框留在屏上。
TEST(TerminalTest, WideningTheTerminalLeavesNoGhostCellsFromTheOldWidth)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    winsize narrow{};
    narrow.ws_col = 20;
    narrow.ws_row = 8;
    ASSERT_EQ(0, ::ioctl(replica, TIOCSWINSZ, &narrow));
    ASSERT_TRUE(make_nonblocking(primary));

    const my_agent::ui::ScreenConfig screen{};
    my_agent::ui::TerminalDriver driver{replica, replica};
    ASSERT_TRUE(driver.render(screen));
    const std::string first_frame = read_available(primary);

    my_agent::test::VirtualTerminal terminal{20, 8};
    terminal.feed(first_frame);
    ASSERT_TRUE(terminal.unhandled().empty())
        << "首帧包含 VirtualTerminal 尚未建模的序列";

    // 前置一：找旧帧实际画到的最右内容列（dock 留左右 gutter，末列本身不画）。
    // 右边框、右对齐 chip 这类「随宽度换列块」的内容就停在这一列附近——加宽后
    // 它们该挪去新列块，这一列在新帧里该是空白。若旧帧没用到半宽以上，本测试
    // 对「新帧留空被跳过」就没有可残留的东西，会在实现空转时悄悄通过。
    int stale_row = -1;
    int stale_col = -1;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 20; ++col) {
            if (!terminal.cell_blank(row, col) && col > stale_col) {
                stale_col = col;
                stale_row = row;
            }
        }
    }
    ASSERT_GE(stale_col, 10) << "旧帧没有用到半宽以上：前置不成立，红测试失去鉴别力";

    winsize wide{};
    wide.ws_col = 40;
    wide.ws_row = 8;
    ASSERT_EQ(0, ::ioctl(replica, TIOCSWINSZ, &wide));
    terminal.resize(40, 8);

    // 前置二：保留式 resize 后旧内容仍在。否则脏屏被量具自身抹掉，收敛断言
    // 空转——这正是 caret_seam 的 resize 场景新建空白 VT 抓不到幽灵的原因。
    ASSERT_FALSE(terminal.cell_blank(stale_row, stale_col))
        << "保留式 resize 应留下旧内容：量具自身不成立";

    ASSERT_TRUE(driver.render(screen));
    const std::string second_frame = read_available(primary);
    ASSERT_FALSE(second_frame.empty()) << "加宽后的重绘一个字节都没发";

    my_agent::test::VirtualTerminal clean{40, 8};
    clean.feed(my_agent::ui::enter_bytes());
    clean.feed(second_frame);
    ASSERT_TRUE(clean.unhandled().empty())
        << "同一份帧字节在空白终端上有未建模的序列";

    // 前置三：场景里真的存在「旧帧有内容、新帧为空白」的格子——幽灵候选。
    // 没有它，任何实现都能让收敛断言通过（空转）。此刻 terminal 还没喂第二帧，
    // 读到的就是旧屏内容。
    int candidate_row = -1;
    for (int row = 0; row < 8 && candidate_row < 0; ++row) {
        for (int col = 0; col < 20; ++col) {
            if (!terminal.cell_blank(row, col) && clean.cell_blank(row, col)) {
                candidate_row = row;
                break;
            }
        }
    }
    ASSERT_GE(candidate_row, 0) << "没有旧内容在新帧留空的格子：场景无幽灵候选";

    terminal.feed(second_frame);
    ASSERT_TRUE(terminal.unhandled().empty())
        << "加宽后的帧包含 VirtualTerminal 尚未建模的序列";

    EXPECT_EQ(clean.screen(), terminal.screen())
        << "加宽后的重绘没有覆盖旧宽度的内容——幽灵单元格留在屏上";
}

// 场景：非 tty 上查询尺寸。
// 领域语义：管道没有尺寸，但折行仍然需要一个宽度 —— 返回 0 列会让 wrap 什么都不吐，
// 整个界面空白。所以失败时必须退回一个可用的默认值（80x24 是 VT100 以来的惯例）。
// Red 原因：仓库尚不存在 size()（编译错误）。
TEST(TerminalTest, FallsBackToAUsableSizeWhenTheTerminalHasNone)
{
    const int devnull = ::open("/dev/null", O_WRONLY);
    ASSERT_LE(0, devnull);

    const my_agent::ui::TerminalDriver driver{devnull, devnull};
    const my_agent::ui::Size size = driver.size();

    EXPECT_LT(0, size.columns);
    EXPECT_LT(0, size.rows);

    ::close(devnull);
}

}  // namespace
