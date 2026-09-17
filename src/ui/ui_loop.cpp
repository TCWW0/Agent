#include "my_agent/ui/ui_loop.hpp"

#include "my_agent/ui/input_editor.hpp"
#include "my_agent/ui/repaint_clock.hpp"
#include "my_agent/ui/resize_watch.hpp"
#include "my_agent/ui/screen.hpp"

#include <maya/style/theme.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <variant>

#include <poll.h>
#include <unistd.h>

namespace my_agent::ui {

namespace {

// 20fps：人眼分辨不出更高的刷新，而 Ollama 一秒能吐几十个 token。
constexpr int kFramesPerSecond = 20;

// 唤醒机制降级成惰性哨兵时的轮询周期。此时没有 fd 可等，只能定期 drain ——
// 比永久挂死好，比忙轮询省。
constexpr int kSentinelPollMs = 50;

}  // namespace

KeyOutcome apply_key(const Key& key, UiState& ui, const Model& model)
{
    // 审批期间按键改变含义：这是 apply_key 需要看 model 的唯一理由。
    // id 取自 pending_permission —— 审批的对象是某一次具体调用，用错 id 会批准
    // 另一个工具。没有 pending id 时按键落回普通文本，而不是发一条无主的批准。
    if (std::holds_alternative<AwaitingPermission>(model.phase) && model.pending_permission
        && key.kind == Key::Kind::Text) {
        const std::string& id = model.pending_permission->id;
        if (key.text == "y" || key.text == "Y") {
            return KeyOutcome{.msg = Msg{PermissionApprove{.id = id}}};
        }
        if (key.text == "n" || key.text == "N") {
            return KeyOutcome{.msg = Msg{PermissionReject{.id = id}}};
        }
        return {};  // 其他键在审批期间无意义，不该插进输入行
    }

    switch (key.kind) {
        case Key::Kind::Text:
        case Key::Kind::Backspace:
        case Key::Kind::Left:
        case Key::Kind::Right:
        case Key::Kind::Up:
        case Key::Kind::Down:
        case Key::Kind::Home:
        case Key::Kind::End: {
            InputEditorState state = edit_input(
                std::move(ui.input), ui.cursor, key
            );
            ui.input = std::move(state.buffer);
            ui.cursor = state.cursor;
            return {};
        }

        case Key::Kind::ClearInput:
        case Key::Kind::Interrupt:
            ui.input.clear();
            ui.cursor = 0;
            return {};

        case Key::Kind::Enter: {
            // 空行不发 Submit：模型会收到一条空用户消息，白跑一次往返，
            // 有些 provider 还会直接报错。
            if (ui.input.empty()) {
                return {};
            }
            Msg msg = Submit{.text = ui.input};
            ui.input.clear();  // 不清会让下一句带着上一句的残留
            ui.cursor = 0;
            return KeyOutcome{.msg = std::move(msg)};
        }

        case Key::Kind::Eof:
            // 唯一的退出路径。不认这个键，用户只能 SIGKILL —— 那条路上备用屏和
            // termios 都还不回去。
            return KeyOutcome{.quit = true};
    }
    return {};
}

bool run_ui(AsyncHost& host, TerminalDriver& terminal)
{
    return run_ui(host, terminal, {});
}

bool run_ui(
    AsyncHost& host,
    TerminalDriver& terminal,
    StatusProvider status_provider
)
{
    if (!terminal.is_tty()) {
        return false;  // 调用方回退到行式 REPL
    }

    UiState ui;
    InputDecoder decoder;
    RepaintClock clock{kFramesPerSecond};
    // 作用域绑在循环上：处理器随 run_ui 返回而摘掉，不给非 tty 回退路径留残留。
    ResizeWatch resize;

    const auto refresh_status = [&ui, &status_provider] {
        if (status_provider) {
            ui.status = status_provider();
        }
    };

    // 开局先画一帧：否则屏幕在用户敲第一个键之前是空的。
    static_cast<void>(clock.should_paint(RepaintClock::Clock::now()));
    refresh_status();
    static_cast<void>(terminal.render(
        project_screen(host.model(), ui, maya::theme::dark)
    ));

    const int wake_fd = host.wake_fd();
    // 键盘 fd 由驱动给出，不能假定 STDIN_FILENO —— 假定会让驱动持有的 fd 被彻底忽略，
    // 于是按键永远读不到（在 pty 上实测过：只画出提示符，敲进去的字节没人取）。
    const int keyboard_fd = terminal.input_fd();
    bool running = true;
    while (running) {
        // drain -> paint -> poll。
        // drain 必须在 poll 之前：唤醒是电平合并的，若不先消费掉已到达的消息，
        // 「消息已在 inbox 但唤醒已被消费」会让 poll 白等一个超时周期。
        // paint 在 drain 之后画的是刚 drain 出来的状态，事件于是在**本轮**上屏；
        // 这是延迟而非正确性 —— 实测把两者换序，流式测试依然通过，因为跳过的
        // 重绘会置上 pending，poll 因此拿到有限超时并在一个帧预算内补画。
        // 兜底在 RepaintClock，顺序只决定落后一帧还是两帧。
        host.drain_inbox();

        if (clock.should_paint(RepaintClock::Clock::now())) {
            refresh_status();
            static_cast<void>(terminal.render(
                project_screen(host.model(), ui, maya::theme::dark)
            ));
        }

        // 键盘固定在 [0]。唤醒与窗口变化按可用性依次追加 —— 哨兵（fd 为 -1）
        // 不能放进 poll，Linux 会把负 fd 当"跳过"，但索引就此错位，于是得记下
        // 每一路落在哪一格，而不是假定固定下标。
        pollfd fds[3] = {
            {.fd = keyboard_fd, .events = POLLIN, .revents = 0},
        };
        int count = 1;
        const int wake_slot = wake_fd >= 0 ? count : -1;
        if (wake_slot >= 0) {
            fds[count++] = {.fd = wake_fd, .events = POLLIN, .revents = 0};
        }
        const int resize_slot = resize.fd() >= 0 ? count : -1;
        if (resize_slot >= 0) {
            fds[count++] = {.fd = resize.fd(), .events = POLLIN, .revents = 0};
        }

        const bool has_wake = wake_slot >= 0;
        const std::chrono::milliseconds pending =
            clock.time_until_next_paint(RepaintClock::Clock::now());
        // 唤醒降级成惰性哨兵时（eventfd 与自管道都失败）没有 fd 可等，只能强制一个
        // 有限超时，靠轮询 drain_inbox 继续推进而不是永久挂死。
        const int timeout = has_wake ? static_cast<int>(pending.count())
                                     : static_cast<int>(kSentinelPollMs);

        const int ready = ::poll(fds, static_cast<nfds_t>(count), timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;  // 信号打断（SIGWINCH 不带 SA_RESTART），重来
            }
            break;
        }

        // 窗口变化：只取走处理器写进管道的字节，重绘交给下一轮开头那一次 ——
        // view() 每帧都重新读 terminal.size()，被帧率跳过的那一帧由 pending +
        // poll 超时补上，和别的事件走同一条路。（原本这里额外强制 render 一次，
        // 实测去掉后重排测试依然通过，说明那次重绘是多余的。）
        // 字节必须取走：管道是电平的，不读就一直可读，poll 每次立刻返回。
        if (resize_slot >= 0 && (fds[resize_slot].revents & POLLIN) != 0) {
            static_cast<void>(resize.drain());
        }

        // 唤醒是电平的：不取走那个计数，POLLIN 就一直亮着，poll 每次立刻返回，
        // 循环退化成忙转（实测 eventfd 不清时空转烧 CPU），而且 poll 的超时从此
        // 失效 —— 帧率补画那条路径被永久短路。0ms 只清信号、不阻塞。
        if (wake_slot >= 0 && (fds[wake_slot].revents & POLLIN) != 0) {
            static_cast<void>(host.wait_wake(std::chrono::milliseconds{0}));
        }

        if ((fds[0].revents & POLLIN) != 0) {
            char bytes[512];
            const ssize_t count = ::read(keyboard_fd, bytes, sizeof(bytes));
            if (count > 0) {
                for (const Key& key :
                     decoder.feed(std::string_view{bytes, static_cast<std::size_t>(count)})) {
                    const KeyOutcome outcome = apply_key(key, ui, host.model());
                    if (outcome.quit) {
                        running = false;
                        break;
                    }
                    if (outcome.msg) {
                        host.dispatch(*outcome.msg);
                    }
                }
            } else if (count == 0) {
                running = false;  // stdin 关闭
            }
        }

    }

    return true;
}

}  // namespace my_agent::ui
