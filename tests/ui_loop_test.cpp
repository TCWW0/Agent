#include "my_agent/ui/ui_loop.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "virtual_terminal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

// 断言必须落在「解码后的屏幕」上，而不是原始字节流上。
//
// 差分渲染器只重写发生变化的格 —— 屏幕内容完全正确的帧里，一个词的相邻
// 字符也可能被 CUP 定位切开。批准工具调用时屏幕从 "[pending]" 变为
// "[done]"，而 old 'n' == new 'n'，那一格被整段跳过：字节流里根本不存在
// 连续的 "[done]" 子串。直接对字节流 find() 锚定的是渲染器的编码细节，
// 不是它在屏幕上画出了什么。喂进会建模屏幕的 VirtualTerminal 再断言屏幕
// 文本，才是这条不变量真正想说的东西。
[[nodiscard]]
std::string decoded_screen(std::string_view bytes, int columns, int rows)
{
    my_agent::test::VirtualTerminal terminal(columns, rows);
    terminal.feed(bytes);
    std::string text;
    for (const std::string& line : terminal.screen()) {
        text += line;
        text += '\n';
    }
    return text;
}

using my_agent::ui::apply_key;
using my_agent::ui::apply_paste;
using my_agent::ui::Key;
using my_agent::ui::KeyOutcome;
using my_agent::ui::UiState;

std::shared_ptr<const my_agent::tool::ToolRuntime> read_tool_runtime(my_agent::tool::ToolHandler handler)
{
    return std::make_shared<const my_agent::tool::ToolRuntime>(
        std::vector<my_agent::tool::RegisteredTool>{
            my_agent::tool::RegisteredTool{
                .descriptor = my_agent::tool::ToolDescriptor{
                    .name = my_agent::tool::ToolName{.value = "read"},
                    .description = "read a file",
                    .input_schema = nlohmann::json{
                        {"type", "object"},
                        {"properties", nlohmann::json{
                            {"path", nlohmann::json{{"type", "string"}}},
                        }},
                    },
                    .effects = my_agent::tool::EffectSet{
                        my_agent::tool::Effect::ReadFs,
                    },
                },
                .handler = std::move(handler),
            },
        });
}

std::shared_ptr<const my_agent::tool::ToolRuntime> empty_tool_runtime()
{
    return std::make_shared<const my_agent::tool::ToolRuntime>(
        std::vector<my_agent::tool::RegisteredTool>{});
}

class TestHost : public my_agent::AsyncHost {
public:
    explicit TestHost(my_agent::StreamEffect stream)
        : my_agent::AsyncHost(std::move(stream), empty_tool_runtime()) {}

    TestHost(my_agent::StreamEffect stream,
             std::shared_ptr<const my_agent::tool::ToolRuntime> runtime)
        : my_agent::AsyncHost(
              std::move(stream), std::move(runtime)) {}
};

// 场景：Idle 时敲字然后回车。
// 领域语义：输入行的编辑完全属于前端 —— 只有回车才产生一条领域事件。这是
// 「UiState 不进 Model」这个分层决定的可执行版本：敲了 5 个字符只有第 6 次
// 按键（回车）跨越边界，而不是 6 次都进 update()。回车后输入行必须清空，
// 否则下一句会带着上一句的残留。
// Red 原因：仓库尚不存在 ui::apply_key（编译错误）。
TEST(UiLoopTest, OnlyEnterCrossesIntoTheDomainAndItClearsTheInputLine)
{
    UiState ui;
    my_agent::Model model;

    for (const char letter : std::string{"hello"}) {
        const KeyOutcome outcome =
            apply_key(Key{.kind = Key::Kind::Text, .text = std::string(1, letter)}, ui, model);
        EXPECT_FALSE(outcome.msg.has_value()) << "编辑不该产生领域事件";
    }
    ASSERT_EQ("hello", ui.input);

    const KeyOutcome outcome = apply_key(Key{.kind = Key::Kind::Enter}, ui, model);
    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(ui.input.empty()) << "回车后必须清空，否则下一句带着残留";
}

TEST(UiLoopTest, PasteStaysInComposerUntilTheUserSubmits)
{
    UiState ui;
    my_agent::Model model;

    const KeyOutcome pasted = apply_paste(
        "first line\nsecond line\r\nthird line", ui, model);
    EXPECT_FALSE(pasted.msg.has_value());
    ASSERT_EQ(1u, ui.pastes.size());
    EXPECT_EQ("first line\nsecond line\nthird line", ui.pastes.front().body);
    EXPECT_EQ(std::string::npos, ui.input.find("first line"));

    const KeyOutcome submitted = apply_key(
        Key{.kind = Key::Kind::Enter}, ui, model);
    ASSERT_TRUE(submitted.msg.has_value());
    const auto* submit = std::get_if<my_agent::Submit>(&*submitted.msg);
    ASSERT_NE(nullptr, submit);
    EXPECT_EQ("first line\nsecond line\nthird line", submit->text);
    EXPECT_TRUE(ui.input.empty());
    EXPECT_TRUE(ui.pastes.empty());
}

// 场景：AwaitingPermission 时按 y / n。
// 领域语义：同一个按键在不同 phase 下含义不同 —— 这正是 apply_key 需要看 model 的
// 唯一理由。审批期间 'y' 是批准而不是往输入行插入字母 'y'。批准/拒绝必须带上
// pending_permission 的 id：审批的对象是某一次具体调用，用错 id 会批准另一个工具。
// Red 原因：当前实现无视 phase，'y' 会被当成文本插进输入行。
TEST(UiLoopTest, InterpretsYesAndNoAsApprovalOnlyWhileAwaitingPermission)
{
    my_agent::Model model;
    model.thread.messages.push_back({
        .role = my_agent::Role::Assistant,
        .text = "",
        .tool_calls = {{
            .id = "call_7f3a",
            .name = "custom_writer",
            .args = nlohmann::json::object(),
            .status = my_agent::ToolCall::Pending{
                .binding = my_agent::tool::ToolBinding{
                    .ref = my_agent::tool::ToolRef{7},
                    .effects = {my_agent::tool::Effect::WriteFs},
                }},
            .turn_id = my_agent::TurnId{3},
            .tool_exec_id = my_agent::ToolExecId{9},
        }},
        .id = my_agent::MessageId{11},
        .turn_id = my_agent::TurnId{3},
    });
    model.phase = my_agent::AwaitingPermission{
        .turn_id = my_agent::TurnId{3},
        .tool_exec_id = my_agent::ToolExecId{9},
        .id = "call_7f3a",
        .assistant_message_id = my_agent::MessageId{11},
    };
    model.pending_permission = my_agent::PendingPermission{
        .id = "call_7f3a",
        .turn_id = my_agent::TurnId{3},
        .tool_exec_id = my_agent::ToolExecId{9},
    };

    UiState approve_ui;
    const KeyOutcome approved =
        apply_key(Key{.kind = Key::Kind::Text, .text = "y"}, approve_ui, model);
    ASSERT_TRUE(approved.msg.has_value());
    ASSERT_TRUE(std::holds_alternative<my_agent::PermissionApprove>(*approved.msg));
    EXPECT_EQ("call_7f3a", std::get<my_agent::PermissionApprove>(*approved.msg).id);
    EXPECT_EQ(my_agent::TurnId{3},
              std::get<my_agent::PermissionApprove>(*approved.msg).turn_id);
    EXPECT_EQ(my_agent::ToolExecId{9},
              std::get<my_agent::PermissionApprove>(*approved.msg).tool_exec_id);
    EXPECT_TRUE(approve_ui.input.empty()) << "审批期间的按键不该进输入行";

    UiState reject_ui;
    const KeyOutcome rejected =
        apply_key(Key{.kind = Key::Kind::Text, .text = "n"}, reject_ui, model);
    ASSERT_TRUE(rejected.msg.has_value());
    ASSERT_TRUE(std::holds_alternative<my_agent::PermissionReject>(*rejected.msg));
    EXPECT_EQ("call_7f3a", std::get<my_agent::PermissionReject>(*rejected.msg).id);

    model.pending_permission->tool_exec_id = my_agent::ToolExecId{10};
    UiState invalid_ui;
    EXPECT_FALSE(apply_key(
        Key{.kind = Key::Kind::Text, .text = "y"}, invalid_ui, model).msg.has_value());
    const KeyOutcome cancel = apply_key(
        Key{.kind = Key::Kind::Interrupt}, invalid_ui, model);
    ASSERT_TRUE(cancel.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::CancelActive>(*cancel.msg));

    // Idle 时同一个键仍然是普通文本。
    UiState idle_ui;
    const my_agent::Model idle;
    const KeyOutcome typed =
        apply_key(Key{.kind = Key::Kind::Text, .text = "y"}, idle_ui, idle);
    EXPECT_FALSE(typed.msg.has_value());
    EXPECT_EQ("y", idle_ui.input);
}

TEST(UiLoopTest, InvalidToolActivityAllowsOnlyCancellationAndTranscriptNavigation)
{
    my_agent::Model model;
    model.phase = my_agent::ExecutingTool{
        .call = my_agent::ToolCallRef{
            .assistant_message_id = my_agent::MessageId{11},
            .tool_exec_id = my_agent::ToolExecId{9},
        },
    };
    UiState ui;
    ui.input = "keep";

    EXPECT_FALSE(apply_key(
        Key{.kind = Key::Kind::Text, .text = "x"}, ui, model).msg.has_value());
    EXPECT_EQ("keep", ui.input);
    EXPECT_TRUE(apply_key(Key{.kind = Key::Kind::PageUp}, ui, model)
                    .transcript_navigation.has_value());
    EXPECT_FALSE(apply_paste("not accepted", ui, model).msg.has_value());
    EXPECT_TRUE(ui.pastes.empty());
    EXPECT_EQ("keep", ui.input);

    const KeyOutcome cancel =
        apply_key(Key{.kind = Key::Kind::Interrupt}, ui, model);
    ASSERT_TRUE(cancel.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::CancelActive>(*cancel.msg));
}

TEST(UiLoopTest, EmptyInterruptCancelsAnActiveTurn)
{
    UiState ui;
    my_agent::Model model;
    model.phase = my_agent::Streaming{.ref = my_agent::StreamRef{.turn_id = my_agent::TurnId{1}, .stream_id = my_agent::StreamId{2}}, .target_message_id = my_agent::MessageId{3}};

    const KeyOutcome outcome =
        apply_key(Key{.kind = Key::Kind::Interrupt}, ui, model);

    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::CancelActive>(*outcome.msg));
    EXPECT_TRUE(ui.input.empty());
}

TEST(UiLoopTest, EmptyInterruptCancelsCompaction)
{
    UiState ui;
    my_agent::Model model;
    model.phase = my_agent::Compacting{};

    const KeyOutcome outcome =
        apply_key(Key{.kind = Key::Kind::Interrupt}, ui, model);

    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::CancelActive>(*outcome.msg));
    EXPECT_TRUE(ui.input.empty());
}

// 场景：删掉一个汉字。
// 领域语义：退格删的是**字符**而不是字节。按字节删会在输入行里留下 2 个残缺字节，
// 屏幕显示成乱码方块，而且再按一次退格也删不干净 —— 这是中文输入下必然遇到的 bug。
// Red 原因：当前实现完全没处理 Backspace。
TEST(UiLoopTest, BackspaceDeletesAWholeCharacterNotAByte)
{
    UiState ui{.input = "你好"};
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(Key{.kind = Key::Kind::Backspace}, ui, model);

    EXPECT_FALSE(outcome.msg.has_value());
    EXPECT_EQ("你", ui.input);  // 而不是 "你" 加两个残缺字节
}

// 场景：输入行为空时按退格。
// 领域语义：没有正确答案，只有可接受的失败方式 —— 不能越界。空串上 pop_back
// 是未定义行为。characterization：固定成无操作。
TEST(UiLoopTest, BackspaceOnAnEmptyInputLineIsANoOp)
{
    UiState ui;
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(Key{.kind = Key::Kind::Backspace}, ui, model);

    EXPECT_FALSE(outcome.msg.has_value());
    EXPECT_TRUE(ui.input.empty());
}

TEST(UiLoopTest, CursorKeysEditTheInputWithoutCrossingIntoTheDomain)
{
    UiState ui{.input = "hello", .cursor = 2};
    const my_agent::Model model;

    const KeyOutcome moved_right = apply_key(
        Key{.kind = Key::Kind::Right}, ui, model
    );
    EXPECT_FALSE(moved_right.msg.has_value());
    EXPECT_EQ(3u, ui.cursor);

    const KeyOutcome moved_home = apply_key(
        Key{.kind = Key::Kind::Home}, ui, model
    );
    EXPECT_FALSE(moved_home.msg.has_value());
    EXPECT_EQ(0u, ui.cursor);

    const KeyOutcome moved_end = apply_key(
        Key{.kind = Key::Kind::End}, ui, model
    );
    EXPECT_FALSE(moved_end.msg.has_value());
    EXPECT_EQ(5u, ui.cursor);
}

TEST(UiLoopTest, TranscriptNavigationKeysProduceUiIntentsNotDomainMessages)
{
    UiState ui{.input = "draft", .cursor = 2};
    const my_agent::Model model;

    const KeyOutcome page_up = apply_key(
        Key{.kind = Key::Kind::PageUp}, ui, model
    );
    ASSERT_TRUE(page_up.transcript_navigation.has_value());
    EXPECT_EQ(
        my_agent::ui::TranscriptNavigation::PageUp,
        *page_up.transcript_navigation);
    EXPECT_FALSE(page_up.msg.has_value());
    EXPECT_EQ("draft", ui.input);
    EXPECT_EQ(2u, ui.cursor);

    const KeyOutcome page_down = apply_key(
        Key{.kind = Key::Kind::PageDown}, ui, model
    );
    ASSERT_TRUE(page_down.transcript_navigation.has_value());
    EXPECT_EQ(
        my_agent::ui::TranscriptNavigation::PageDown,
        *page_down.transcript_navigation);
    EXPECT_FALSE(page_down.msg.has_value());

    const KeyOutcome to_top = apply_key(
        Key{.kind = Key::Kind::ControlHome}, ui, model
    );
    ASSERT_TRUE(to_top.transcript_navigation.has_value());
    EXPECT_EQ(
        my_agent::ui::TranscriptNavigation::Top,
        *to_top.transcript_navigation);
    EXPECT_FALSE(to_top.msg.has_value());

    const KeyOutcome to_bottom = apply_key(
        Key{.kind = Key::Kind::ControlEnd}, ui, model
    );
    ASSERT_TRUE(to_bottom.transcript_navigation.has_value());
    EXPECT_EQ(
        my_agent::ui::TranscriptNavigation::Bottom,
        *to_bottom.transcript_navigation);
    EXPECT_FALSE(to_bottom.msg.has_value());
}

TEST(UiLoopTest, NonEmptySubmitRequestsBottomBeforeDispatch)
{
    UiState ui{.input = "send this", .cursor = 4};
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(
        Key{.kind = Key::Kind::Enter}, ui, model
    );

    ASSERT_TRUE(outcome.msg.has_value());
    ASSERT_TRUE(outcome.transcript_navigation.has_value());
    EXPECT_EQ(
        my_agent::ui::TranscriptNavigation::Bottom,
        *outcome.transcript_navigation);
}

TEST(UiLoopTest, ExactCompactCommandBecomesACompactionRequest)
{
    UiState ui{.input = "/compact", .cursor = 8};
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(
        Key{.kind = Key::Kind::Enter}, ui, model);

    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::RequestCompaction>(
        *outcome.msg));
    EXPECT_FALSE(std::holds_alternative<my_agent::Submit>(*outcome.msg));
    EXPECT_TRUE(ui.input.empty());

    UiState regular{.input = "/compact now", .cursor = 12};
    const KeyOutcome ordinary = apply_key(
        Key{.kind = Key::Kind::Enter}, regular, model);
    ASSERT_TRUE(ordinary.msg.has_value());
    const auto* submit = std::get_if<my_agent::Submit>(&*ordinary.msg);
    ASSERT_NE(nullptr, submit);
    EXPECT_EQ("/compact now", submit->text);
}

// 场景：空输入行上按回车。
// 领域语义：不该发一条空的 Submit —— 那会让模型收到一条空用户消息，浪费一次
// 往返，而且有些 provider 会直接报错。回车在空行上应该什么都不做。
// Red 原因：当前实现无条件发 Submit。
TEST(UiLoopTest, EnterOnAnEmptyLineDoesNotSubmit)
{
    UiState ui;
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(Key{.kind = Key::Kind::Enter}, ui, model);

    EXPECT_FALSE(outcome.msg.has_value());
}

TEST(UiLoopTest, CtrlRResumesAQueuedBlockedSession)
{
    UiState ui;
    my_agent::Model model;
    model.phase = my_agent::Blocked{
        .reason = my_agent::BlockReason::ContextLimit,
        .message = "request exceeds the available context budget",
    };
    model.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{1},
        .message_id = my_agent::MessageId{1},
        .text = "waiting prompt",
    });

    const KeyOutcome outcome = apply_key(
        Key{.kind = Key::Kind::ResumeQueue}, ui, model);

    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::ResumeQueue>(*outcome.msg));
    EXPECT_TRUE(ui.input.empty());
}

TEST(UiLoopTest, CtrlXClearsQueuedPromptsWithoutTouchingTheDraft)
{
    UiState ui{.input = "draft", .cursor = 3};
    my_agent::Model model;
    model.queued_messages.push_back(my_agent::QueuedMessage{
        .turn_id = my_agent::TurnId{1},
        .message_id = my_agent::MessageId{1},
        .text = "waiting prompt",
    });

    const KeyOutcome outcome = apply_key(
        Key{.kind = Key::Kind::ClearQueue}, ui, model);

    ASSERT_TRUE(outcome.msg.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::ClearQueue>(*outcome.msg));
    EXPECT_EQ("draft", ui.input);
    EXPECT_EQ(3u, ui.cursor);

    model.queued_messages.clear();
    EXPECT_FALSE(apply_key(
        Key{.kind = Key::Kind::ClearQueue}, ui, model).msg.has_value());
}

TEST(UiLoopTest, CtrlCAndCtrlUClearTheDraftWithoutSubmittingOrQuitting)
{
    UiState ctrl_c_ui{.input = "draft", .cursor = 2};
    const my_agent::Model model;

    const KeyOutcome ctrl_c = apply_key(
        Key{.kind = Key::Kind::Interrupt}, ctrl_c_ui, model
    );
    EXPECT_FALSE(ctrl_c.msg.has_value());
    EXPECT_FALSE(ctrl_c.quit);
    EXPECT_TRUE(ctrl_c_ui.input.empty());
    EXPECT_EQ(0u, ctrl_c_ui.cursor);

    UiState ctrl_u_ui{.input = "draft", .cursor = 2};
    const KeyOutcome ctrl_u = apply_key(
        Key{.kind = Key::Kind::ClearInput}, ctrl_u_ui, model
    );
    EXPECT_FALSE(ctrl_u.msg.has_value());
    EXPECT_FALSE(ctrl_u.quit);
    EXPECT_TRUE(ctrl_u_ui.input.empty());
    EXPECT_EQ(0u, ctrl_u_ui.cursor);
}

TEST(UiLoopTest, CtrlCOnAnEmptyDraftDoesNotQuit)
{
    UiState ui;
    const my_agent::Model model;

    const KeyOutcome outcome = apply_key(
        Key{.kind = Key::Kind::Interrupt}, ui, model
    );
    EXPECT_FALSE(outcome.msg.has_value());
    EXPECT_FALSE(outcome.quit);
}

TEST(UiLoopTest, CtrlDRemainsTheExplicitQuitKey)
{
    UiState ui;
    const my_agent::Model model;

    EXPECT_TRUE(apply_key(Key{.kind = Key::Kind::Eof}, ui, model).quit);
}

// 场景：非 tty 上启动 UI。
// 领域语义：非 tty（CI、管道、重定向）必须让调用方知道要回退，而不是往文件里吐
// 转义序列。返回 false 就是那个信号。
TEST(UiLoopTest, DeclinesToRunWithoutATtySoTheCallerCanFallBack)
{
    const int devnull = ::open("/dev/null", O_RDWR);
    ASSERT_LE(0, devnull);

    my_agent::ui::TerminalSession terminal{devnull, devnull};
    my_agent::ui::Painter painter;
    TestHost host{[](my_agent::Request, my_agent::ProviderEventSink) {}};

    EXPECT_FALSE(my_agent::ui::run_ui(host, terminal, painter));

    ::close(devnull);
}

// issue #26 红 4：真实事件循环绘制语义 Screen。
// 一条 tracer bullet 同时穿过 run_ui → project_screen → Painter → pty：
// 初始帧先看到新 Composer 的占位文案；提交后再看到 Conversation 的 speaker chrome
// 和助手正文。旧路径虽然也能吐出消息文本，却没有 "type a message"、"you"、
// "agent" 这些 Maya widget 拥有的语义外观，因此不能靠正文碰巧出现蒙混过关。
TEST(UiLoopTest, RunsTheSemanticTranscriptAndDockOnARealTerminal)
{
    constexpr std::string_view kUser = "RUNTIME_SCREEN_USER_7F3A";
    constexpr std::string_view kAssistant = "RUNTIME_SCREEN_AGENT_9C2E";

    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    TestHost host{[kAssistant](
        my_agent::Request, my_agent::ProviderEventSink sink) {
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
            .text = std::string{kAssistant},
        }});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_until = [&seen, primary](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
            if (::poll(&probe, 1, 200) > 0) {
                char buffer[4096];
                const ssize_t count = ::read(primary, buffer, sizeof(buffer));
                if (count > 0) {
                    seen.append(buffer, static_cast<std::size_t>(count));
                }
            }
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }
    if (!read_until("type a message")) {
        ADD_FAILURE() << "initial frame did not contain the semantic composer";
        stop();
        return;
    }

    const std::string typed = std::string{kUser} + "\r";
    EXPECT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    EXPECT_TRUE(read_until(kAssistant))
        << "assistant reply did not pass through the semantic transcript";
    EXPECT_NE(std::string::npos, seen.find(kUser));
    EXPECT_NE(std::string::npos, seen.find("you"));
    EXPECT_NE(std::string::npos, seen.find("agent"));

    stop();
}

TEST(UiLoopTest, RoutesBracketedPasteAsOneComposerSubmission)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic_bool called{false};
    std::string submitted;
    TestHost host{[&](my_agent::Request request,
                                 my_agent::ProviderEventSink sink) {
        for (const my_agent::Message& message : request.messages) {
            if (message.role == my_agent::Role::User) {
                submitted = message.text;
            }
        }
        called.store(true, std::memory_order_release);
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
            .text = "PASTE_ACCEPTED_7F3A",
        }});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};
    host.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{my_agent::ContextManagementConfig{
            .configured_limit = 32768,
            .safety_margin = 64,
            .normal_output_ratio = 0.0078125,
            .normal_output_ceiling = 256,
        }, {}, "model-a"}}});
    host.set_system_prompt_provider([] {
        return std::string{"dynamic startup prompt"};
    });

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_until = [&seen, primary](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
            if (::poll(&probe, 1, 200) > 0) {
                char buffer[4096];
                const ssize_t count = ::read(primary, buffer, sizeof(buffer));
                if (count > 0) {
                    seen.append(buffer, static_cast<std::size_t>(count));
                }
            }
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("type a message")) {
        ADD_FAILURE() << "initial frame did not contain the semantic composer";
        stop();
        return;
    }

    const std::string pasted_body =
        "first line\nsecond line\r\nthird line\n"
        + std::string(6000, 'x');
    const std::string pasted =
        "\x1b[200~" + pasted_body + "\x1b[201~";
    ASSERT_EQ(static_cast<ssize_t>(pasted.size()),
              ::write(primary, pasted.data(), pasted.size()));
    ASSERT_TRUE(read_until("Pasted text"));
    EXPECT_FALSE(called.load(std::memory_order_acquire));

    const char enter = '\r';
    ASSERT_EQ(1, ::write(primary, &enter, 1));
    ASSERT_TRUE(read_until("PASTE_ACCEPTED_7F3A"));
    ASSERT_TRUE(called.load(std::memory_order_acquire));
    EXPECT_EQ("first line\nsecond line\nthird line\n"
              + std::string(6000, 'x'), submitted);

    stop();
}

TEST(UiLoopTest, ShowsQueuedPreviewThenPromotesItWithoutDuplication)
{
    std::atomic<bool> release_first{false};
    std::atomic_size_t stream_calls{0};

    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    TestHost host{
        [&release_first, &stream_calls](my_agent::Request,
                                         my_agent::ProviderEventSink sink) {
            if (stream_calls.fetch_add(1) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = "FIRST_OUTPUT_7F3A",
                }});
                while (!release_first.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
                sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
                return;
            }

            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                .text = "SECOND_OUTPUT_9C2E",
            }});
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_chunk = [&] {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    };
    const auto screen = [&] {
        return decoded_screen(seen, 80, 24);
    };
    const auto read_until = [&read_chunk, &seen](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            read_chunk();
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto read_screen_until = [&read_chunk, &screen](
        std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && screen().find(needle) == std::string::npos) {
            read_chunk();
        }
        return screen().find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        release_first.store(true, std::memory_order_release);
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }

    ASSERT_EQ(6, ::write(primary, "first\r", 6));
    ASSERT_TRUE(read_until("FIRST_OUTPUT_7F3A"));

    const std::string queued_paste =
        "\x1b[200~queued prompt\x1b[201~\r";
    ASSERT_EQ(static_cast<ssize_t>(queued_paste.size()),
              ::write(primary, queued_paste.data(), queued_paste.size()));
    ASSERT_TRUE(read_screen_until("queued 1 / 1"));
    EXPECT_NE(std::string::npos, screen().find("queued prompt"));

    release_first.store(true, std::memory_order_release);
    ASSERT_TRUE(read_until("SECOND_OUTPUT_9C2E"));
    ASSERT_TRUE(read_screen_until("SECOND_OUTPUT_9C2E"));
    const std::string final_screen = screen();
    EXPECT_EQ(std::string::npos, final_screen.find("queued 1 / 1"));
    const std::size_t queued_start = final_screen.find("queued prompt");
    ASSERT_NE(std::string::npos, queued_start);
    EXPECT_EQ(
        std::string::npos,
        final_screen.find("queued prompt", queued_start + 1));

    stop();
}

TEST(UiLoopTest, OversizedPasteShowsContextLimitAndCtrlXClearsQueue)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic_size_t stream_calls{0};
    TestHost host{[&stream_calls](
        my_agent::Request, my_agent::ProviderEventSink) {
        ++stream_calls;
    }};
    host.dispatch(my_agent::Msg{my_agent::SetRequestPolicy{.policy = my_agent::RequestPolicy{my_agent::ContextManagementConfig{
            .configured_limit = 512,
            .safety_margin = 32,
            .normal_output_ratio = 0.125,
            .normal_output_ceiling = 64,
            .compaction_output_ratio = 0.0625,
            .compaction_output_ceiling = 32,
        }, {}, "test-model"}}});

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_chunk = [&] {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    };
    const auto screen = [&] {
        return decoded_screen(seen, 80, 24);
    };
    const auto read_screen_until = [&read_chunk, &screen](
        std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && screen().find(needle) == std::string::npos) {
            read_chunk();
        }
        return screen().find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_screen_until("Ready")) {
        ADD_FAILURE() << "driver did not paint its initial frame";
        stop();
        return;
    }

    const std::string paste = "\x1b[200~" + std::string(1000, 'x')
        + "\x1b[201~\r";
    if (::write(primary, paste.data(), paste.size())
        != static_cast<ssize_t>(paste.size())) {
        ADD_FAILURE() << "failed to write oversized bracketed paste";
        stop();
        return;
    }
    if (!read_screen_until("Ctx limit")) {
        ADD_FAILURE() << "context-limit phase was not visible\n" << screen();
        stop();
        return;
    }
    EXPECT_NE(std::string::npos, screen().find("Ctrl-X clear"));
    EXPECT_EQ(std::size_t{0}, stream_calls.load());

    const char clear_queue = 0x18;
    if (::write(primary, &clear_queue, 1) != 1) {
        ADD_FAILURE() << "failed to write Ctrl-X";
        stop();
        return;
    }
    if (!read_screen_until("Ready")) {
        ADD_FAILURE() << "cleared context-limit queue did not return to Ready\n"
                      << screen();
        stop();
        return;
    }
    EXPECT_EQ(std::string::npos, screen().find("queued 1 / 1"));

    stop();
    EXPECT_EQ(std::size_t{0}, stream_calls.load());
    EXPECT_TRUE(host.model().queued_messages.empty());
    EXPECT_TRUE(std::holds_alternative<my_agent::Idle>(host.model().phase));
}

TEST(UiLoopTest, RoutesPageUpBytesToTheTranscriptViewport)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::string transcript;
    for (int index = 0; index < 90; ++index) {
        transcript += "SCROLL_ROW_" + std::to_string(index) + "\n";
    }
    TestHost host{[transcript](my_agent::Request, my_agent::ProviderEventSink sink) {
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = transcript}});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_chunk = [&] {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    };
    const auto read_until = [&read_chunk, &seen](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            read_chunk();
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }

    const std::string typed = "scroll\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    ASSERT_TRUE(read_until("SCROLL_ROW_89"));
    EXPECT_EQ(
        std::string::npos,
        decoded_screen(seen, 80, 24).find("SCROLL_ROW_0"));

    std::string page_ups;
    for (int count = 0; count < 40; ++count) {
        page_ups += "\x1b[5~";
    }
    ASSERT_EQ(static_cast<ssize_t>(page_ups.size()),
              ::write(primary, page_ups.data(), page_ups.size()));
    const auto page_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < page_deadline
           && decoded_screen(seen, 80, 24).find("SCROLL_ROW_0")
               == std::string::npos) {
        read_chunk();
    }
    EXPECT_NE(
        std::string::npos,
        decoded_screen(seen, 80, 24).find("SCROLL_ROW_0"));

    stop();
}

TEST(UiLoopTest, ShowsUnseenTranscriptOutputWithoutMovingAFrozenViewport)
{
    constexpr std::string_view kTail = "S3_NEW_OUTPUT_TAIL_7F3A";

    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::string transcript;
    for (int index = 0; index < 90; ++index) {
        transcript += "S3_ROW_" + std::to_string(index) + "\n";
    }
    std::atomic<bool> release_tail{false};
    TestHost host{
        [&transcript, &release_tail, kTail](
            my_agent::Request, my_agent::ProviderEventSink sink) {
            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = transcript}});
            while (!release_tail.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                .text = std::string{kTail},
            }});
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_chunk = [&] {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    };
    const auto read_until = [&read_chunk, &seen](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            read_chunk();
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto screen = [&] {
        return decoded_screen(seen, 80, 24);
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }

    const std::string typed = "s3\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    ASSERT_TRUE(read_until("S3_ROW_89"));

    std::string page_ups;
    for (int count = 0; count < 40; ++count) {
        page_ups += "\x1b[5~";
    }
    ASSERT_EQ(static_cast<ssize_t>(page_ups.size()),
              ::write(primary, page_ups.data(), page_ups.size()));
    const auto top_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < top_deadline
           && screen().find("S3_ROW_0") == std::string::npos) {
        read_chunk();
    }
    const std::string frozen = screen();
    ASSERT_NE(std::string::npos, frozen.find("S3_ROW_0"));
    EXPECT_NE(std::string::npos, frozen.find("Ctrl+End"));
    EXPECT_EQ(std::string::npos, frozen.find("new output"));

    release_tail.store(true, std::memory_order_release);
    const auto output_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < output_deadline
           && screen().find("new output") == std::string::npos) {
        read_chunk();
    }
    const std::string unseen = screen();
    EXPECT_NE(std::string::npos, unseen.find("S3_ROW_0"));
    EXPECT_NE(std::string::npos, unseen.find("new output"));
    EXPECT_NE(std::string::npos, unseen.find("End")) << unseen;
    EXPECT_EQ(std::string::npos, unseen.find(kTail));

    const std::string to_bottom = "\x1b[1;5F";
    ASSERT_EQ(static_cast<ssize_t>(to_bottom.size()),
              ::write(primary, to_bottom.data(), to_bottom.size()));
    const auto bottom_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < bottom_deadline
           && screen().find(kTail) == std::string::npos) {
        read_chunk();
    }
    const std::string followed = screen();
    EXPECT_NE(std::string::npos, followed.find(kTail));
    EXPECT_EQ(std::string::npos, followed.find("new output"));

    stop();
}

TEST(UiLoopTest, RoutesVerticalWheelOnlyWhenItsCoordinateHitsTheTranscript)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::string transcript;
    for (int index = 0; index < 90; ++index) {
        transcript += "WHEEL_ROW_" + std::to_string(index) + "\n";
    }
    TestHost host{[transcript](my_agent::Request, my_agent::ProviderEventSink sink) {
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = transcript}});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(
            host, terminal, painter, {},
            my_agent::ui::UiConfig{.mouse_scroll_rows = 3}));
    }};

    std::string seen;
    const auto read_chunk = [&] {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    };
    const auto read_until = [&read_chunk, &seen](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            read_chunk();
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }

    const std::string typed = "wheel\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    ASSERT_TRUE(read_until("WHEEL_ROW_89"));
    const std::string at_bottom = decoded_screen(seen, 80, 24);
    ASSERT_EQ(std::string::npos, at_bottom.find("WHEEL_ROW_0"));

    const std::string wheel_up = "\x1b[<64;10;1M";
    ASSERT_EQ(static_cast<ssize_t>(wheel_up.size()),
              ::write(primary, wheel_up.data(), wheel_up.size()));
    const auto inside_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < inside_deadline
           && decoded_screen(seen, 80, 24) == at_bottom) {
        read_chunk();
    }
    const std::string after_inside = decoded_screen(seen, 80, 24);
    EXPECT_NE(at_bottom, after_inside)
        << "滚轮命中 transcript 后必须改变可见滚动几何";

    const std::string outside_wheel = "\x1b[<64;10;24M";
    ASSERT_EQ(static_cast<ssize_t>(outside_wheel.size()),
              ::write(primary, outside_wheel.data(), outside_wheel.size()));
    const auto outside_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{300};
    while (std::chrono::steady_clock::now() < outside_deadline) {
        read_chunk();
    }
    EXPECT_EQ(after_inside, decoded_screen(seen, 80, 24))
        << "滚轮命中 dock/status 区域不得改变 transcript";

    stop();
}

TEST(UiLoopTest, KeepsThinkingAliveWithoutAnExternalEvent)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic<bool> release{false};
    TestHost host{
        [&release](my_agent::Request, my_agent::ProviderEventSink) {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "thinking-test"}});

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto ready_by = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < ready_by
           && seen.find("\x1b[?1049h") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    ASSERT_NE(std::string::npos, seen.find("\x1b[?1049h"))
        << "驱动没能进入全屏";

    // Drain bytes already belonging to the initial paint before measuring the
    // no-event animation window. Keep a short quiet period so a split pty
    // write cannot be mistaken for an animation repaint.
    auto quiet_until = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{25};
    while (std::chrono::steady_clock::now() < quiet_until) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 5) <= 0) {
            continue;
        }
        char buffer[4096];
        const ssize_t count = ::read(primary, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        seen.append(buffer, static_cast<std::size_t>(count));
        quiet_until = std::chrono::steady_clock::now()
            + std::chrono::milliseconds{25};
    }

    const std::size_t initial_bytes = seen.size();
    const auto animation_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < animation_deadline
           && seen.size() == initial_bytes) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }

    EXPECT_GT(seen.size(), initial_bytes)
        << "Thinking 的 ActivityIndicator 应在没有 token、键盘或 resize 时继续请求重绘";

    release.store(true, std::memory_order_release);
    const char eof = 0x04;
    static_cast<void>(::write(primary, &eof, 1));
    ui_thread.join();
    host.shutdown();

    ::close(replica);
    ::close(primary);
}

TEST(UiLoopTest, ShowsLiveTokenRateWithoutAnExternalEvent)
{
    int primary = -1;
    int replica = -1;
    winsize initial{
        .ws_row = 24,
        .ws_col = 80,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    if (::openpty(&primary, &replica, nullptr, nullptr, &initial) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic<bool> second_delta_sent{false};
    std::atomic<bool> release{false};
    TestHost host{
        [&second_delta_sent, &release](my_agent::Request, my_agent::ProviderEventSink sink) {
            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = "ABCD"}});
            std::this_thread::sleep_for(std::chrono::milliseconds{350});
            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                .text = std::string(396, 'x'),
            }});
            second_delta_sent.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
    host.dispatch(my_agent::Msg{my_agent::Submit{.text = "rate-test"}});

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{5};
    bool observed_nonzero_rate = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
        if (second_delta_sent.load(std::memory_order_acquire)) {
            const std::string screen = decoded_screen(seen, 80, 24);
            if (screen.find("t/s") != std::string::npos
                && screen.find("0.0 t/s") == std::string::npos) {
                observed_nonzero_rate = true;
                break;
            }
        }
    }

    EXPECT_TRUE(observed_nonzero_rate)
        << "流式 delta 到达后，状态栏必须在没有外部事件时显示非零实时速率\n"
        << decoded_screen(seen, 80, 24);

    release.store(true, std::memory_order_release);
    const char eof = 0x04;
    static_cast<void>(::write(primary, &eof, 1));
    ui_thread.join();
    host.shutdown();

    ::close(replica);
    ::close(primary);
}

// 场景：流式 token 在**回合结束之前**就出现在屏幕上。
// 领域语义：这是整个 issue #10 存在的理由。行式 REPL 实测的缺陷是「敲完回车之后
// 19 秒死寂，然后整段一次性出现」—— 因为 run_until_quiescent 在 phase 是 Streaming
// 时不返回。这条测试断言的正是那个缺陷已被修掉：stream effect 故意**不结束**
// （只发 delta，不发 StreamFinished），所以 run_until_quiescent 永远不会返回；
// 如果屏幕上能看到 delta 的内容，说明重绘发生在流进行中而不是回合结束后。
//
// 用真 pty：这不是能靠字节串断言的东西 —— 要证明的是「在那个时刻已经写到终端上」。
TEST(UiLoopTest, PaintsStreamingTextBeforeTheTurnEnds)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    // 故意不发 StreamFinished：回合永不结束，只有流中重绘才能让内容上屏。
    // 用 release 而不是 sleep 一个固定时长：断言做完才放 worker 走，既保证
    // 「观察发生在回合结束之前」，又不让测试为一个猜的时长白等。
    std::atomic<bool> release{false};
    TestHost host{
        [&release](my_agent::Request, my_agent::ProviderEventSink sink) {
            sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = "STREAMED"}});
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    // 必须等驱动进了 raw mode 才能敲键：tcsetattr 用的是 TCSAFLUSH，会丢弃此前
    // 已排队的输入。切备用屏的字节是在 tcsetattr **之后**写的，所以看到它就等于
    // 那次 flush 已经过去了 —— 用它同步，而不是 sleep 一个猜的时长。
    std::string seen;
    const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < ready_by
           && seen.find("\x1b[?1049h") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    ASSERT_NE(std::string::npos, seen.find("\x1b[?1049h")) << "驱动没能进入全屏";

    // 敲 "hi" + 回车，让 host 进入 Streaming。
    const std::string typed = "hi\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));

    // 读 pty 直到看到 delta 的内容，或超时。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline
           && seen.find("STREAMED") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }

    EXPECT_NE(std::string::npos, seen.find("STREAMED"))
        << "流式内容必须在回合结束前就上屏";

    // 断言已经做完，现在才放 worker 走。送 Ctrl-D 让循环退出，然后拆掉 host。
    release.store(true, std::memory_order_release);
    const char eof = 0x04;
    static_cast<void>(::write(primary, &eof, 1));
    ui_thread.join();
    host.shutdown();

    ::close(replica);
    ::close(primary);
}

// 场景：回合已经结束，最后一段文本必须上屏。
// 领域语义：这是上一条测试**不**覆盖的那一半。流进行中每个 delta 都带来一次唤醒，
// 所以哪怕重绘被帧率跳过，下一次唤醒也会把它补上。但 StreamFinished 之后不再有
// 唤醒 —— 如果那一刻的重绘正好落在帧预算内被跳过，就没有任何后续事件来补画，
// 最后一段回答永远停在 inbox 里，屏幕上少一截。RepaintClock 的 pending +
// time_until_next_paint 就是为这个而存在：跳过的重绘必须换来一个有限的 poll 超时。
//
// 用真 pty，并且**故意**让 delta 紧跟 Submit 到达（同一个帧预算内）以逼出那个跳过。
TEST(UiLoopTest, PaintsTheFinalChunkEvenThoughNoWakeFollowsTheEndOfTheStream)
{
    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    // 正常终结的流：发完 delta 立刻 StreamFinished，此后不再有任何唤醒。
    TestHost host{[](my_agent::Request, my_agent::ProviderEventSink sink) {
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = "LASTCHUNK"}});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < ready_by
           && seen.find("\x1b[?1049h") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    ASSERT_NE(std::string::npos, seen.find("\x1b[?1049h")) << "驱动没能进入全屏";

    const std::string typed = "hi\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline
           && seen.find("LASTCHUNK") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }

    EXPECT_NE(std::string::npos, seen.find("LASTCHUNK"))
        << "回合结束后不再有唤醒，被帧率跳过的那一帧必须靠 poll 超时补画";

    const char eof = 0x04;
    static_cast<void>(::write(primary, &eof, 1));
    ui_thread.join();
    host.shutdown();

    ::close(replica);
    ::close(primary);
}

// 场景：拖动窗口改变终端宽度，不碰键盘。
// 领域语义：折行宽度来自终端，宽度变了屏幕上的每一行都算错了 —— 变窄时文字被
// 截掉，变宽时留着一堆无用的换行。此时**没有任何键盘或后台事件**，poll 正阻塞着，
// 所以必须由 SIGWINCH 自己把循环叫醒。实测过当前实现：TIOCSWINSZ 之后 pty 上
// 一个字节都没有，屏幕一直停在旧宽度，直到用户碰巧按了别的键才刷新。
// Red 原因：仓库尚未安装 SIGWINCH 处理器（默认是忽略），循环收不到通知。
TEST(UiLoopTest, ReflowsOnTerminalResizeWithoutWaitingForAKeypress)
{
    int primary = -1;
    int replica = -1;
    winsize initial{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    if (::openpty(&primary, &replica, nullptr, nullptr, &initial) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    // 40 个 A：80 列时一行放得下，20 列时必须折成多行。宽度是否被重新读取，
    // 看的就是屏幕上一行到底有多少个 A。
    const std::string wide_text(40, 'A');
    TestHost host{[wide_text](my_agent::Request, my_agent::ProviderEventSink sink) {
        sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{.text = wide_text}});
        sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
    }};

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < ready_by
           && seen.find("\x1b[?1049h") == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    ASSERT_NE(std::string::npos, seen.find("\x1b[?1049h")) << "驱动没能进入全屏";

    // 先确认 80 列下整段在一行里 —— 否则后面「变窄了」的断言无从对比。
    const std::string typed = "hi\r";
    ASSERT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < until
           && seen.find(wide_text) == std::string::npos) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                seen.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }
    ASSERT_NE(std::string::npos, seen.find(wide_text))
        << "80 列下这段本该在一行里: " << seen;

    // 变窄，然后只发 SIGWINCH —— 不碰键盘。内核只把这个信号发给 pty 的前台进程组，
    // 测试进程不在其中，所以由测试自己 raise：要证明的是「信号到达后屏幕重排」，
    // 谁投递的信号不属于这条不变量。
    winsize narrow{.ws_row = 24, .ws_col = 20, .ws_xpixel = 0, .ws_ypixel = 0};
    ASSERT_EQ(0, ::ioctl(primary, TIOCSWINSZ, &narrow));
    std::string after;
    ASSERT_EQ(0, ::raise(SIGWINCH));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline
           && static_cast<std::size_t>(std::count(after.begin(), after.end(), 'A'))
               < wide_text.size()) {
        pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
        if (::poll(&probe, 1, 200) > 0) {
            char buffer[4096];
            const ssize_t count = ::read(primary, buffer, sizeof(buffer));
            if (count > 0) {
                after.append(buffer, static_cast<std::size_t>(count));
            }
        }
    }

    EXPECT_EQ(
        wide_text.size(),
        static_cast<std::size_t>(std::count(after.begin(), after.end(), 'A'))
    ) << "变窄后必须完整重排全部内容，而不是横向裁掉或等待下一次按键";
    EXPECT_EQ(std::string::npos, after.find(wide_text))
        << "旧宽度的整行不该再出现在重排后的帧里";

    const char eof = 0x04;
    static_cast<void>(::write(primary, &eof, 1));
    ui_thread.join();
    host.shutdown();

    ::close(replica);
    ::close(primary);
}

TEST(UiLoopTest, ApprovesARealToolCallThroughThePtyAndShowsItsDetails)
{
    constexpr std::string_view kArgumentSentinel = "PTY_APPROVAL_ARG_7F3A";
    constexpr std::string_view kContinuationSentinel = "PTY_APPROVAL_CONT_9B2E";

    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic<int> stream_calls{0};
    std::atomic<bool> received_expected_args{false};
    my_agent::StreamEffect fake_stream =
        [&stream_calls, kArgumentSentinel, kContinuationSentinel](
            my_agent::Request, my_agent::ProviderEventSink sink
        ) {
            if (stream_calls.fetch_add(1, std::memory_order_acq_rel) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "pty-approval-call",
                    .name = "read",
                    .args = nlohmann::json{
                        {"path", std::string{kArgumentSentinel}},
                    },
                }});
            } else {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = std::string{kContinuationSentinel},
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        };
    my_agent::tool::ToolHandler fake_tool =
        [&received_expected_args, kArgumentSentinel](
            const nlohmann::json& args,
            const my_agent::tool::ToolExecutionContext& context
        ) -> my_agent::tool::ExecResult {
            static_cast<void>(context);
            if (args.value("path", std::string{})
                    == std::string{kArgumentSentinel}) {
                received_expected_args.store(true, std::memory_order_release);
            }
            return my_agent::tool::ToolOutput{
                .text = "PTY_APPROVAL_OUTPUT_4C8D",
            };
        };

    TestHost host{
        std::move(fake_stream), read_tool_runtime(std::move(fake_tool))};
    host.dispatch(my_agent::Msg{
        my_agent::SetProfile{.profile = my_agent::Profile::Minimal},
    });

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_until = [&seen, primary](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
            if (::poll(&probe, 1, 200) > 0) {
                char buffer[4096];
                const ssize_t count = ::read(primary, buffer, sizeof(buffer));
                if (count > 0) {
                    seen.append(buffer, static_cast<std::size_t>(count));
                }
            }
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }
    EXPECT_EQ(std::string::npos, seen.find(kArgumentSentinel));
    EXPECT_EQ(std::string::npos, seen.find(kContinuationSentinel));

    const std::string typed = "request approval\r";
    EXPECT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    EXPECT_TRUE(read_until("effect=read_fs"));
    EXPECT_NE(std::string::npos, seen.find("Permission Required"));
    EXPECT_NE(std::string::npos, seen.find("effect=read_fs"));
    EXPECT_NE(std::string::npos, seen.find(kArgumentSentinel));

    const char approve = 'y';
    EXPECT_EQ(1, ::write(primary, &approve, 1));
    EXPECT_TRUE(read_until(kContinuationSentinel));
    const std::string screen = decoded_screen(seen, 80, 24);
    EXPECT_NE(std::string::npos, screen.find("[done]"))
        << "批准后屏幕上应是被标记为完成的工具调用:\n" << screen;
    EXPECT_TRUE(received_expected_args.load(std::memory_order_acquire));

    stop();
}

TEST(UiLoopTest, RejectsARealToolCallThroughThePtyWithoutExecutingIt)
{
    constexpr std::string_view kArgumentSentinel = "PTY_REJECTION_ARG_6D1E";
    constexpr std::string_view kContinuationSentinel = "PTY_REJECTION_CONT_8A4C";

    int primary = -1;
    int replica = -1;
    if (::openpty(&primary, &replica, nullptr, nullptr, nullptr) != 0) {
        GTEST_SKIP() << "openpty unavailable in this environment";
    }

    std::atomic<int> stream_calls{0};
    std::atomic<bool> tool_executed{false};
    my_agent::StreamEffect fake_stream =
        [&stream_calls, kArgumentSentinel, kContinuationSentinel](
            my_agent::Request, my_agent::ProviderEventSink sink
        ) {
            if (stream_calls.fetch_add(1, std::memory_order_acq_rel) == 0) {
                sink(my_agent::ProviderEvent{my_agent::StreamToolCall{
                    .id = "pty-rejection-call",
                    .name = "read",
                    .args = nlohmann::json{
                        {"path", std::string{kArgumentSentinel}},
                    },
                }});
            } else {
                sink(my_agent::ProviderEvent{my_agent::StreamTextDelta{
                    .text = std::string{kContinuationSentinel},
                }});
            }
            sink(my_agent::ProviderEvent{my_agent::StreamFinished{}});
        };
    my_agent::tool::ToolHandler fake_tool =
        [&tool_executed](
            const nlohmann::json&,
            const my_agent::tool::ToolExecutionContext& context)
            -> my_agent::tool::ExecResult {
            static_cast<void>(context);
            tool_executed.store(true, std::memory_order_release);
            return my_agent::tool::ToolOutput{
                .text = "PTY_REJECTION_OUTPUT_5E7B",
            };
        };

    TestHost host{
        std::move(fake_stream), read_tool_runtime(std::move(fake_tool))};
    host.dispatch(my_agent::Msg{
        my_agent::SetProfile{.profile = my_agent::Profile::Minimal},
    });

    std::thread ui_thread{[&host, replica] {
        my_agent::ui::TerminalSession terminal{replica, replica};
        my_agent::ui::Painter painter;
        static_cast<void>(my_agent::ui::run_ui(host, terminal, painter));
    }};

    std::string seen;
    const auto read_until = [&seen, primary](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline
               && seen.find(needle) == std::string::npos) {
            pollfd probe{.fd = primary, .events = POLLIN, .revents = 0};
            if (::poll(&probe, 1, 200) > 0) {
                char buffer[4096];
                const ssize_t count = ::read(primary, buffer, sizeof(buffer));
                if (count > 0) {
                    seen.append(buffer, static_cast<std::size_t>(count));
                }
            }
        }
        return seen.find(needle) != std::string::npos;
    };
    const auto stop = [&] {
        const char eof = 0x04;
        static_cast<void>(::write(primary, &eof, 1));
        ui_thread.join();
        host.shutdown();
        ::close(replica);
        ::close(primary);
    };

    if (!read_until("\x1b[?1049h")) {
        ADD_FAILURE() << "driver did not enter the alternate screen";
        stop();
        return;
    }
    EXPECT_EQ(std::string::npos, seen.find(kArgumentSentinel));
    EXPECT_EQ(std::string::npos, seen.find(kContinuationSentinel));

    const std::string typed = "request rejection\r";
    EXPECT_EQ(static_cast<ssize_t>(typed.size()),
              ::write(primary, typed.data(), typed.size()));
    EXPECT_TRUE(read_until("effect=read_fs"));
    EXPECT_NE(std::string::npos, seen.find("Permission Required"));
    EXPECT_NE(std::string::npos, seen.find(kArgumentSentinel));

    const char reject = 'n';
    EXPECT_EQ(1, ::write(primary, &reject, 1));
    EXPECT_TRUE(read_until(kContinuationSentinel));
    const std::string screen = decoded_screen(seen, 80, 24);
    EXPECT_NE(std::string::npos, screen.find("[rejected]"))
        << "拒绝后屏幕上应是被标记为拒绝的工具调用:\n" << screen;
    EXPECT_FALSE(tool_executed.load(std::memory_order_acquire));

    stop();
}

}  // namespace
