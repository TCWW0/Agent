#include "my_agent/ui/screen.hpp"
#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/tool/tool.hpp"
#include "my_agent/ui/view.hpp"
#include <maya/style/color.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/status_bar.hpp>
#include <maya/widget/turn.hpp>
#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

namespace my_agent::ui {

maya::Element dock_element(const DockConfig& dock)
{
    using namespace maya::dsl;

    std::vector<maya::Element> rows;
    if (dock.permission) {
        // 权限请求居顶：它是 dock 里优先级最高的内容 ——
        // 用户必须先响应它，composer 的输入语义整个随它改变。
        rows.push_back(maya::Permission{*dock.permission}.build());
    }
    rows.push_back(blank().build());  // composer 上方的呼吸行
    rows.push_back(maya::Composer{dock.composer}.build());
    rows.push_back(maya::StatusBar{dock.status}.build());
    // 一列水平 gutter：dock 自身的边界声明，不是边框盒 ——
    // 边框属于 composer 自己（它的圆角盒），gutter 属于 dock 整体。
    return (v(std::move(rows)) | padding(0, 1)).build();
}

namespace {

const ToolCall* tool_call_for(const Model& model, std::string_view id)
{
    for (auto message = model.thread.messages.rbegin();
         message != model.thread.messages.rend(); ++message) {
        for (auto call = message->tool_calls.rbegin();
             call != message->tool_calls.rend(); ++call) {
            if (call->id == id) {
                return &*call;
            }
        }
    }
    return nullptr;
}

std::string effect_label(tool::EffectSet effects)
{
    std::string result;
    const auto append = [&result](std::string_view label) {
        if (!result.empty()) {
            result += ',';
        }
        result += label;
    };
    if (effects.has(tool::Effect::ReadFs)) {
        append("read_fs");
    }
    if (effects.has(tool::Effect::WriteFs)) {
        append("write_fs");
    }
    if (effects.has(tool::Effect::Net)) {
        append("net");
    }
    if (effects.has(tool::Effect::Exec)) {
        append("exec");
    }
    return result.empty() ? "none" : result;
}

std::string tool_status_label(const ToolCall& call)
{
    return std::visit(
        [](const auto& status) -> std::string {
            using Status = std::decay_t<decltype(status)>;
            if constexpr (std::is_same_v<Status, ToolCall::Pending>) {
                return "pending";
            } else if constexpr (std::is_same_v<Status, ToolCall::Done>) {
                return "done";
            } else if constexpr (std::is_same_v<Status, ToolCall::Failed>) {
                return "failed";
            } else {
                return "rejected";
            }
        },
        call.status);
}

std::string tool_call_text(const ToolCall& call)
{
    std::string text = "+-- tool_call [" + tool_status_label(call) + "] "
        + call.name + "\n| args: " + call.args.dump();
    if (!call.is_pending()) {
        text += "\n| output: ";
        text += call.output().empty() ? "(empty)" : call.output();
    }
    text += "\n+--";
    return text;
}

std::optional<maya::Permission::Config> permission_config(const Model& model)
{
    if (!model.pending_permission) {
        return std::nullopt;
    }

    const ToolCall* call = tool_call_for(model, model.pending_permission->id);
    const std::string name = call ? call->name : model.pending_permission->id;
    const tool::ToolDef* definition = tool::find(name);
    const std::string effects = definition
        ? effect_label(definition->effects)
        : "none";
    const std::string args = call ? call->args.dump() : "{}";

    maya::Permission::Config config;
    config.tool_name = name;
    config.description = "allow " + name + "? effect=" + effects
        + " args: " + args;
    return config;
}

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

maya::Color phase_color(const Model& model, const maya::Theme& theme)
{
    return std::visit(
        [&theme](const auto& phase) {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, my_agent::Streaming>) {
                return theme.info;
            } else if constexpr (
                std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return theme.warning;
            } else if constexpr (
                std::is_same_v<Phase, my_agent::ExecutingTool>) {
                return theme.success;
            } else {
                return theme.muted;
            }
        },
        model.phase);
}

maya::Composer::ProfileChip profile_chip(
    Profile profile, const maya::Theme& theme)
{
    switch (profile) {
        case Profile::Write:
            return {.label = "write", .color = theme.success};
        case Profile::Ask:
            return {.label = "ask", .color = theme.info};
        case Profile::Minimal:
            return {.label = "minimal", .color = theme.muted};
    }
    return {.label = "write", .color = theme.success};
}

maya::Composer::Config composer_config(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    maya::Composer::Config cfg;
    cfg.text = ui.input;
    cfg.cursor = static_cast<int>(ui.cursor == std::string::npos ? ui.input.size() : ui.cursor);
    cfg.caret_mode = maya::Composer::CaretMode::SolidCell;
    cfg.state = composer_state(model);
    cfg.active_color = phase_color(model, theme);
    cfg.text_color = theme.text;
    cfg.accent_color = theme.accent;
    cfg.warn_color = theme.warning;
    cfg.highlight_color = theme.info;
    cfg.profile = profile_chip(model.profile, theme);
    cfg.min_body_rows = 1;
    return cfg;
}

maya::Conversation::Config transcript_config(
    const Model& model, const maya::Theme& theme)
{
    maya::Conversation::Config cfg;
    cfg.fill_available_height = false;
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
        if (msg.error) {
            turn.error = *msg.error;
        }
        for (const ToolCall& call : msg.tool_calls) {
            turn.body.emplace_back(maya::Turn::PlainText{
                .content = tool_call_text(call),
            });
        }
        cfg.turns.push_back(std::move(turn));
    }
    return cfg;
}

struct PhaseDisplay {
    std::string glyph;
    std::string verb;
    bool active{false};
};

PhaseDisplay phase_display(const Model& model)
{
    return std::visit(
        [&model](const auto& phase) -> PhaseDisplay {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, my_agent::Streaming>) {
                return {.glyph = "◆", .verb = "Thinking", .active = true};
            } else if constexpr (
                std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return {.glyph = "⚠", .verb = "Approve?", .active = true};
            } else if constexpr (std::is_same_v<Phase, my_agent::ExecutingTool>) {
                const ToolCall* call = tool_call_for(model, phase.id);
                return {
                    .glyph = "◆",
                    .verb = call ? call->name : "Running",
                    .active = true,
                };
            } else {
                return {.glyph = "●", .verb = "Ready", .active = false};
            }
        },
        model.phase);
}

int status_count(std::optional<std::size_t> value)
{
    if (!value) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        *value, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

// StatusBar 与 agentty 使用同一种映射方式：每类信息进入对应的子组件，
// 不再把整行预先压成一个字符串。这样 Maya 才能按真实宽度逐级降级。
maya::StatusBar::Config status_config(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    maya::StatusBar::Config cfg;
    const PhaseDisplay display = phase_display(model);
    cfg.phase_color = phase_color(model, theme);
    cfg.phase.glyph = display.glyph;
    cfg.phase.verb = display.verb;
    cfg.phase.color = cfg.phase_color;
    cfg.phase.breathing = display.active;
    if (ui.status.elapsed_seconds) {
        cfg.phase.elapsed_secs = static_cast<float>(*ui.status.elapsed_seconds);
    }

    cfg.token_stream.color = theme.info;
    cfg.token_stream.live = std::holds_alternative<Streaming>(model.phase);
    if (ui.status.tokens_per_second) {
        const float rate = static_cast<float>(*ui.status.tokens_per_second);
        cfg.token_stream.rate = rate;
        cfg.token_stream.history.push_back(rate);
    }

    if (!ui.status.model_name.empty()) {
        cfg.model_badge = maya::ModelBadge{{
            .label = ui.status.model_name,
            .version = {},
            .color = theme.primary,
            .show_dot = false,
        }}.build();
    }
    cfg.context.used = status_count(ui.status.context_used);
    cfg.context.max = status_count(ui.status.context_limit);
    cfg.context.cells = 10;
    return cfg;
}

}

ScreenConfig project_screen(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    return ScreenConfig{
        .transcript = transcript_config(model, theme),
        .dock = DockConfig{
            .permission = permission_config(model),
            .composer = composer_config(model, ui, theme),
            .status = status_config(model, ui, theme)
        }
    };
}

}  // namespace my_agent::ui
