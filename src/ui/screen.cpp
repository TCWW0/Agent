#include "my_agent/ui/screen.hpp"
#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/ui/view.hpp"
#include <maya/style/color.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/status_bar.hpp>
#include <maya/widget/turn.hpp>
#include <string>
#include <type_traits>
#include <variant>

namespace my_agent::ui {

namespace {

maya::Composer::State composer_state(const Model& model)
{
    return std::visit(
        [](const auto& phase) -> maya::Composer::State {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, my_agent::Idle>) {
                return maya::Composer::State::Idle;
            } else if constexpr (std::is_same_v<Phase, my_agent::Streaming>){
                return maya::Composer::State::Streaming;
            } else if constexpr (std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return maya::Composer::State::AwaitingPermission;
            } else {
                return maya::Composer::State::ExecutingTool;
            }
        },
        model.phase
    );
}

maya::Composer::Config composer_config(const Model& model, const UiState& ui)
{
    maya::Composer::Config cfg;
    cfg.text = ui.input;
    cfg.cursor = static_cast<int>(ui.cursor == std::string::npos ? ui.input.size() : ui.cursor);
    cfg.state = composer_state(model);
    return cfg;
}

maya::Conversation::Config transcript_config(
    const Model& model, const maya::Theme& theme)
{
    maya::Conversation::Config cfg;
    cfg.turns.reserve(model.thread.messages.size());
    for (const Message& msg : model.thread.messages) {
        maya::Turn::Config turn;
        if (msg.role == Role::User) {
            turn.label = "you";
            turn.rail_color = theme.accent;    // 用户消息：强调色（旧 IR 同语义）
        } else {
            turn.label = "agent";
            turn.rail_color = theme.primary;   // 助手消息：主色（阅读主体）
        }
        turn.body.emplace_back(maya::Turn::PlainText{.content = msg.text});
        cfg.turns.push_back(std::move(turn));
    }
    return cfg;
}

// 状态栏的显示字段
maya::StatusBar::Config status_config(const Model& model)
{
    maya::StatusBar::Config cfg;
    auto phase = std::visit(
        [](const auto& phase) -> std::string {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, my_agent::Streaming>){
                return "Thinking";
            } else if constexpr (std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return "Awaiting";
            } else if constexpr (std::is_same_v<Phase, my_agent::ExecutingTool>) {
                return "Executing";
            } else {
                return "Ready";
            }
        },
        model.phase
    );
    cfg.phase.verb = phase;
    return cfg; 
}

}

ScreenConfig project_screen(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    return ScreenConfig{
        .transcript = transcript_config(model, theme),
        .dock = DockConfig{
            .composer = composer_config(model, ui),
            .status = status_config(model)
        }
    };
}

}  // namespace my_agent::ui
