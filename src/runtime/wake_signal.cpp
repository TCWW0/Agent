#include "my_agent/runtime/wake_signal.hpp"

#include <cstdint>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace my_agent {

namespace {

// O_NONBLOCK 不是可选项：signal() 在后台 worker 上跑，如果写端满了会阻塞在
// drain 之外的地方，把 worker 拖住；wait() 侧读空时阻塞则会绕过 poll 的判断。
// 两端都必须非阻塞，超时/等待一律交给 poll。
[[nodiscard]]
bool make_nonblocking_cloexec(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}

}  // namespace

WakeSignal::WakeSignal()
{
    // EFD_SEMAPHORE 不设：默认语义是「一次读走全部计数并清零」，正是我们要的合并。
    const int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd >= 0) {
        read_fd_ = event_fd;
        write_fd_ = event_fd;
        return;
    }

    // eventfd 可能被 seccomp 沙箱过滤掉（容器里很常见），所以必须有自管道回退。
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) == 0 && make_nonblocking_cloexec(pipe_fds[0])
        && make_nonblocking_cloexec(pipe_fds[1])) {
        read_fd_ = pipe_fds[0];
        write_fd_ = pipe_fds[1];
        return;
    }
    if (pipe_fds[0] >= 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
    }
    // 两条路都失败：留在 -1，成为惰性哨兵。signal/wait 变成空操作，调用方靠
    // wait_for 的超时轮询继续跑 —— 降级运行胜过启动失败。
}

WakeSignal::~WakeSignal()
{
    if (read_fd_ >= 0) {
        ::close(read_fd_);
    }
    if (write_fd_ >= 0 && write_fd_ != read_fd_) {
        ::close(write_fd_);
    }
}

void WakeSignal::signal()
{
    if (write_fd_ < 0) {
        return;  // 惰性哨兵
    }
    const std::uint64_t one = 1;
    // 写满（eventfd 计数溢出 / 管道缓冲满）说明已经有大量未消费的唤醒，
    // 再加一次没有意义 —— 合并语义下丢掉这次写不会丢事件。
    static_cast<void>(::write(write_fd_, &one, sizeof(one)));
}

void WakeSignal::wait()
{
    while (!wait_for(std::chrono::milliseconds{-1})) {
        // poll 被信号打断（EINTR）时重试。SIGWINCH 会频繁打断这里。
    }
}

bool WakeSignal::wait_for(std::chrono::milliseconds timeout)
{
    if (read_fd_ < 0) {
        return false;  // 惰性哨兵：调用方退回超时轮询
    }

    pollfd probe{.fd = read_fd_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&probe, 1, static_cast<int>(timeout.count()));
    if (ready <= 0) {
        return false;  // 超时或 EINTR。都不消费标志。
    }

    // 一次读走全部计数：这就是合并语义。eventfd 读 8 字节，管道读到空为止。
    std::uint64_t counter = 0;
    if (read_fd_ == write_fd_) {
        static_cast<void>(::read(read_fd_, &counter, sizeof(counter)));
    } else {
        char scratch[256];
        while (::read(read_fd_, scratch, sizeof(scratch)) > 0) {
        }
    }
    return true;
}

int WakeSignal::fd() const noexcept
{
    return read_fd_;
}

}  // namespace my_agent
