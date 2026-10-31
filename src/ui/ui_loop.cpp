#include "my_agent/ui/ui_loop.hpp"

#include "my_agent/ui/input_editor.hpp"
#include "my_agent/ui/repaint_clock.hpp"
#include "my_agent/ui/resize_watch.hpp"
#include "my_agent/ui/screen.hpp"
#include "my_agent/ui/transcript_guidance.hpp"
#include "my_agent/runtime/permission_projection.hpp"

#include <maya/core/motion.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
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

struct AnimationHostContext {
    RepaintClock* clock;
};

void request_animation_frame(void* opaque) noexcept
{
    auto* context = static_cast<AnimationHostContext*>(opaque);
    if (context == nullptr || context->clock == nullptr) {
        return;
    }
    context->clock->request_frame(RepaintClock::Clock::now());
}

void request_animation_frame_after(void* opaque, std::int64_t delay_ms) noexcept
{
    auto* context = static_cast<AnimationHostContext*>(opaque);
    if (context == nullptr || context->clock == nullptr) {
        return;
    }
    context->clock->request_frame_after(
        RepaintClock::Clock::now(), std::chrono::milliseconds{delay_ms});
}

[[nodiscard]]
std::string normalize_paste_text(std::string text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            normalized.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

[[nodiscard]]
std::size_t paste_line_count(std::string_view text) noexcept
{
    if (text.empty()) {
        return 0;
    }
    std::size_t count = 1;
    for (const char byte : text) {
        if (byte == '\n') {
            ++count;
        }
    }
    if (text.back() == '\n') {
        --count;
    }
    return count;
}

}  // namespace

KeyOutcome apply_paste(std::string text, UiState& ui, const Model& model)
{
    if (std::holds_alternative<AwaitingPermission>(model.session.phase)) {
        return {};
    }

    text = normalize_paste_text(std::move(text));
    if (text.empty()) {
        return {};
    }

    InputEditorState normalized = edit_input(
        std::move(ui.input), ui.cursor,
        Key{.kind = Key::Kind::Text, .text = {}});
    ui.input = std::move(normalized.buffer);
    ui.cursor = normalized.cursor;

    const std::size_t index = ui.pastes.size();
    ui.pastes.push_back(PendingPaste{
        .body = text,
        .line_count = paste_line_count(text),
        .byte_count = text.size(),
    });
    const std::string placeholder = make_paste_placeholder(index);
    ui.input.insert(ui.cursor, placeholder);
    ui.cursor += placeholder.size();
    return {};
}

KeyOutcome apply_key(const Key& key, UiState& ui, const Model& model)
{
    if (tool_activity_projection(model).kind
        == ToolActivityProjection::Kind::Invalid) {
        switch (key.kind) {
            case Key::Kind::Interrupt:
                return KeyOutcome{.msg = Msg{CancelActive{}}};
            case Key::Kind::Eof:
                return KeyOutcome{.quit = true};
            case Key::Kind::PageUp:
                return KeyOutcome{
                    .transcript_navigation = TranscriptNavigation::PageUp};
            case Key::Kind::PageDown:
                return KeyOutcome{
                    .transcript_navigation = TranscriptNavigation::PageDown};
            case Key::Kind::ControlHome:
                return KeyOutcome{
                    .transcript_navigation = TranscriptNavigation::Top};
            case Key::Kind::ControlEnd:
                return KeyOutcome{
                    .transcript_navigation = TranscriptNavigation::Bottom};
            default:
                return {};
        }
    }

    // 审批期间按键改变含义：这是 apply_key 需要看 model 的唯一理由。
    // id 取自 pending_permission —— 审批的对象是某一次具体调用，用错 id 会批准
    // 另一个工具。没有 pending id 时按键落回普通文本，而不是发一条无主的批准。
    if (std::holds_alternative<AwaitingPermission>(model.session.phase)
        && key.kind == Key::Kind::Text) {
        if (pending_permission_call(model) == nullptr) {
            return {};
        }
        const std::string& id = model.session.pending_permission->id;
        if (key.text == "y" || key.text == "Y") {
            return KeyOutcome{.msg = Msg{PermissionApprove{
                .turn_id = model.session.pending_permission->turn_id,
                .tool_exec_id = model.session.pending_permission->tool_exec_id,
                .id = id,
            }}};
        }
        if (key.text == "n" || key.text == "N") {
            return KeyOutcome{.msg = Msg{PermissionReject{
                .turn_id = model.session.pending_permission->turn_id,
                .tool_exec_id = model.session.pending_permission->tool_exec_id,
                .id = id,
            }}};
        }
        return {};  // 其他键在审批期间无意义，不该插进输入行
    }

    switch (key.kind) {
        case Key::Kind::PageUp:
            return KeyOutcome{
                .transcript_navigation = TranscriptNavigation::PageUp};

        case Key::Kind::PageDown:
            return KeyOutcome{
                .transcript_navigation = TranscriptNavigation::PageDown};

        case Key::Kind::ControlHome:
            return KeyOutcome{
                .transcript_navigation = TranscriptNavigation::Top};

        case Key::Kind::ControlEnd:
            return KeyOutcome{
                .transcript_navigation = TranscriptNavigation::Bottom};

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
            ui.input.clear();
            ui.cursor = 0;
            ui.pastes.clear();
            return {};

        case Key::Kind::ResumeQueue:
            if (std::holds_alternative<Blocked>(model.session.phase)
                && !model.session.queued_messages.empty()) {
                return KeyOutcome{.msg = Msg{ResumeQueue{}}};
            }
            return {};

        case Key::Kind::ClearQueue:
            if (!model.session.queued_messages.empty()) {
                return KeyOutcome{.msg = Msg{ClearQueue{}}};
            }
            return {};

        case Key::Kind::Interrupt:
            if (ui.input.empty()
                && (std::holds_alternative<Streaming>(model.session.phase)
                    || std::holds_alternative<Compacting>(model.session.phase)
                    || std::holds_alternative<ExecutingTool>(model.session.phase)
                    || std::holds_alternative<AwaitingPermission>(model.session.phase))) {
                return KeyOutcome{.msg = Msg{CancelActive{}}};
            }
            ui.input.clear();
            ui.cursor = 0;
            ui.pastes.clear();
            return {};

        case Key::Kind::Enter: {
            // 空行不发 Submit：模型会收到一条空用户消息，白跑一次往返，
            // 有些 provider 还会直接报错。
            if (ui.input.empty()) {
                return {};
            }
            const std::string text = expand_paste_placeholders(
                ui.input, ui.pastes);
            Msg msg = text == "/compact"
                ? Msg{RequestCompaction{}}
                : Msg{Submit{.text = text}};
            ui.input.clear();  // 不清会让下一句带着上一句的残留
            ui.cursor = 0;
            ui.pastes.clear();
            return KeyOutcome{
                .msg = std::move(msg),
                .transcript_navigation = TranscriptNavigation::Bottom,
            };
        }

        case Key::Kind::Eof:
            // 唯一的退出路径。不认这个键，用户只能 SIGKILL —— 那条路上备用屏和
            // termios 都还不回去。
            return KeyOutcome{.quit = true};
    }
    return {};
}

bool run_ui(
    AsyncHost& host,
    TerminalSession& terminal,
    Painter& painter,
    StatusProvider status_provider,
    UiConfig config
)
{
    if (!terminal.is_tty()) {
        return false;  // 调用方回退到行式 REPL
    }

    UiState ui;
    InputDecoder decoder;
    RepaintClock clock{kFramesPerSecond};
    AnimationHostContext animation_context{.clock = &clock};
    // Install only after the tty gate. The scope covers the full UI loop and
    // restores Maya's previous host before run_ui returns.
    maya::anim::AnimationHostScope animation_host{
        &animation_context,
        &request_animation_frame,
        &request_animation_frame_after,
    };
    // 作用域绑在循环上：处理器随 run_ui 返回而摘掉，不给非 tty 回退路径留残留。
    ResizeWatch resize;
    TranscriptRevisionTracker transcript_revision;
    TranscriptGuidance transcript_guidance;

    const auto refresh_status = [&ui, &host, &status_provider] {
        const bool streaming = std::holds_alternative<Streaming>(
            host.model().session.phase);
        const LiveTokenRate::Snapshot live = ui.live_token_rate.observe(
            host.model().session.stream_telemetry.output_bytes,
            host.model().session.stream_telemetry.generation,
            streaming,
            RepaintClock::Clock::now());

        if (live.sample) {
            // Keep provider generations and UI sample generations in separate
            // namespaces so a final provider generation cannot suppress the
            // first live sample of the next turn.
            ui.token_rate_history.observe(
                live.sample->rate,
                live.sample->generation | (std::uint64_t{1} << 63));
        }

        StatusBarInput next;
        if (status_provider) {
            next = status_provider();
        }

        if (live.active) {
            next.tokens_per_second = live.rate;
            next.tokens_generated = live.tokens_generated;
            next.telemetry_generation.reset();
        } else {
            ui.token_rate_history.observe(
                next.tokens_per_second,
                next.telemetry_generation);
        }
        ui.status = std::move(next);
    };

    const auto sync_transcript_guidance = [&] {
        const std::uint64_t revision = transcript_revision.observe(host.model());
        const TranscriptViewport::Snapshot snapshot =
            painter.transcript_viewport_snapshot();
        transcript_guidance.reconcile(snapshot, revision);
        ui.transcript_hint = transcript_guidance.hint(snapshot, revision);
    };

    // 开局先画一帧：否则屏幕在用户敲第一个键之前是空的。
    const auto initial_now = RepaintClock::Clock::now();
    clock.request_frame(initial_now);
    static_cast<void>(clock.should_paint(initial_now));
    refresh_status();
    sync_transcript_guidance();
    const WriteSink writer = [&terminal](std::string_view bytes) {
        return terminal.write_all(bytes);
    };
    static_cast<void>(painter.render(
        project_screen(host.model(), ui, painter.theme()),
        terminal.size(),
        writer
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
        if (host.drain_inbox()) {
            clock.request_frame(RepaintClock::Clock::now());
        }

        if (clock.should_paint(RepaintClock::Clock::now())) {
            refresh_status();
            sync_transcript_guidance();
            static_cast<void>(painter.render(
                project_screen(host.model(), ui, painter.theme()),
                terminal.size(),
                writer
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
        const auto pending_timeout = pending.count() >= 0
            ? std::min<std::int64_t>(pending.count(), kSentinelPollMs)
            : static_cast<std::int64_t>(kSentinelPollMs);
        const int timeout = has_wake ? static_cast<int>(pending.count())
                                     : static_cast<int>(pending_timeout);

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
            clock.request_frame(RepaintClock::Clock::now());
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
                for (const InputEvent& event : decoder.feed_events(
                         std::string_view{bytes, static_cast<std::size_t>(count)})) {
                    if (const auto* key = std::get_if<Key>(&event)) {
                        const KeyOutcome outcome = apply_key(*key, ui, host.model());
                        if (outcome.quit) {
                            running = false;
                            break;
                        }
                        if (outcome.transcript_navigation) {
                            painter.apply_transcript_navigation(
                                *outcome.transcript_navigation);
                        }
                    if (outcome.msg) {
                        // Dispatch the complete runtime message variant, including
                        // local maintenance events introduced by the async host.
                        host.dispatch(*outcome.msg);
                    }
                    } else if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
                        static_cast<void>(painter.apply_transcript_mouse(
                            *mouse, config.mouse_scroll_rows));
                    } else if (const auto* paste = std::get_if<PasteEvent>(
                                   &event)) {
                        static_cast<void>(apply_paste(
                            paste->text, ui, host.model()));
                    }
                }
                if (running) {
                    clock.request_frame(RepaintClock::Clock::now());
                }
            } else if (count == 0) {
                running = false;  // stdin 关闭
            }
        }

    }

    return true;
}

}  // namespace my_agent::ui
