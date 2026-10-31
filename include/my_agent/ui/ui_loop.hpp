#pragma once

#include "my_agent/runtime/async_host.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/runtime/msg.hpp"
#include "my_agent/ui/input.hpp"
#include "my_agent/ui/painter.hpp"
#include "my_agent/ui/terminal_session.hpp"
#include "my_agent/ui/transcript_viewport.hpp"
#include "my_agent/ui/ui_config.hpp"
#include "my_agent/ui/ui_state.hpp"

#include <functional>
#include <optional>

namespace my_agent::ui {

// 一次按键的结果。msg 有值表示这次按键跨越了前端与领域的边界 —— 绝大多数按键
// （编辑输入行）不会，这正是 UiState 不进 Model 的理由。
// Msg 也包含后台维护事件，因此 UI 目标必须随 runtime/msg.hpp 一起重编译。
struct KeyOutcome {
    std::optional<Msg> msg;
    std::optional<TranscriptNavigation> transcript_navigation;
    bool quit{false};
};

// 按键 -> UiState 变更 + 可选领域事件。纯：不碰终端、不碰 host。
// 需要 model 是因为同一个按键在不同 phase 下含义不同（AwaitingPermission 时
// 'y' 是批准而不是文本）。
[[nodiscard]]
KeyOutcome apply_key(const Key& key, UiState& ui, const Model& model);

// Bracketed paste crosses the UI boundary as one operation. The body is
// stored out-of-band and represented in the composer by a compact chip.
[[nodiscard]]
KeyOutcome apply_paste(std::string text, UiState& ui, const Model& model);

// 事件循环。poll 在 {stdin, wake_fd} 上等，因此流式输出进行中也能读键盘 ——
// 这是 run_until_quiescent 做不到的（它在 phase 是 Streaming 时不返回）。
// 非 tty 时立即返回 false，调用方回退到行式 REPL。
using StatusProvider = std::function<StatusBarInput()>;

bool run_ui(
    AsyncHost& host,
    TerminalSession& terminal,
    Painter& painter,
    StatusProvider status_provider = {},
    UiConfig config = {}
);

}  // namespace my_agent::ui
