#include "my_agent/ui/screen.hpp"
#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/model.hpp"
#include "my_agent/runtime/permission_projection.hpp"
#include <maya/style/color.hpp>
#include <maya/core/motion.hpp>
#include <maya/element/text.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/phase_chip.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/status_bar.hpp>
#include <maya/widget/turn.hpp>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace my_agent::ui {

namespace {

enum class DockDensity {
    Full,
    NoBreathing,
    CompactStatus,
    NoGuidance,
    CompactComposer,
    PermissionCompact,
    PermissionAction,
    TooSmall,
};

bool context_debug_enabled() noexcept
{
    const char* value = std::getenv("MY_AGENT_CONTEXT_DEBUG");
    if (value == nullptr) {
        return false;
    }
    const std::string_view flag{value};
    return flag == "1" || flag == "true" || flag == "on";
}

maya::Element transcript_guidance_element(
    const TranscriptGuidanceConfig& config)
{
    using namespace maya::dsl;
    return component([config](int width, int) {
        auto candidate = [](const std::vector<TranscriptGuidanceConfig::Run>& runs) {
            std::string content;
            std::vector<maya::StyledRun> styled_runs;
            for (const auto& run : runs) {
                if (run.text.empty()) {
                    continue;
                }
                const std::size_t offset = content.size();
                content += run.text;
                styled_runs.push_back(maya::StyledRun{
                    .byte_offset = offset,
                    .byte_length = run.text.size(),
                    .style = run.style,
                });
            }

            maya::TextElement label{
                .content = std::move(content),
                .style = {},
                .wrap = maya::TextWrap::TruncateEnd,
                .runs = std::move(styled_runs),
            };
            return h(spacer(), maya::Element{std::move(label)}, spacer()).build();
        };

        const std::vector<const std::vector<TranscriptGuidanceConfig::Run>*> candidates = {
            &config.full,
            &config.compact,
        };
        for (const auto* runs : candidates) {
            if (runs->empty()) {
                continue;
            }
            maya::Element element = candidate(*runs);
            if (measure_element(element, /*max_width=*/1 << 14).width.value
                <= width) {
                return element;
            }
        }
        return blank().build();
    });
}

maya::Element queue_preview_element(const QueuePreviewConfig& config)
{
    using namespace maya::dsl;
    return (component([config](int, int) {
        const std::string separator = " · ";
        std::string content = config.label + separator + config.text;
        std::vector<maya::StyledRun> runs;
        runs.push_back(maya::StyledRun{
            .byte_offset = 0,
            .byte_length = config.label.size(),
            .style = config.label_style,
        });
        runs.push_back(maya::StyledRun{
            .byte_offset = config.label.size() + separator.size(),
            .byte_length = config.text.size(),
            .style = config.text_style,
        });

        maya::TextElement label{
            .content = std::move(content),
            .style = {},
            .wrap = maya::TextWrap::TruncateEnd,
            .runs = std::move(runs),
        };
        return (maya::Element{std::move(label)}
                | height(1) | overflow(maya::Overflow::Hidden)).build();
    }) | height(1) | overflow(maya::Overflow::Hidden)).build();
}

void append_queue_preview(
    std::vector<maya::Element>& rows,
    const DockConfig& dock)
{
    if (dock.queue_preview) {
        rows.push_back(queue_preview_element(*dock.queue_preview));
    }
}

maya::Element compact_status_element(const maya::StatusBar::Config& config)
{
    using namespace maya::dsl;
    std::string content = config.phase.glyph;
    if (!content.empty() && !config.phase.verb.empty()) {
        content += " ";
    }
    content += config.phase.verb;

    maya::TextElement label{
        .content = std::move(content),
        .style = maya::Style{}.with_fg(config.phase.color).with_bold(),
        .wrap = maya::TextWrap::TruncateEnd,
    };
    return (maya::Element{std::move(label)}
            | height(1) | overflow(maya::Overflow::Hidden)).build();
}

maya::Element compact_permission_element(
    const maya::Permission::Config& config, bool action_only)
{
    using namespace maya::dsl;
    return (component([config, action_only](int width, int) {
        std::string content = "[y] allow  [n] deny  " + config.tool_name;
        if (!action_only && !config.description.empty()) {
            content += ": ";
            content += config.description;
        }

        // At the extreme permission tier, keep the decision keys and tool
        // identity together. The labels are useful but expendable; the
        // short form avoids letting them push the tool name out of view.
        if (action_only
            && maya::string_width(content) > std::max(1, width)) {
            content = "[y] [n] " + config.tool_name;
        }

        maya::TextElement label{
            .content = std::move(content),
            .style = maya::Style{}.with_fg(maya::Color::yellow()),
            .wrap = maya::TextWrap::TruncateEnd,
        };
        return maya::Element{std::move(label)};
    }) | height(1) | overflow(maya::Overflow::Hidden)).build();
}

maya::Element too_small_element()
{
    using namespace maya::dsl;
    maya::TextElement label{
        .content = "terminal too small · resize",
        .style = maya::Style{}.with_fg(maya::Color::yellow()).with_bold(),
        .wrap = maya::TextWrap::TruncateEnd,
    };
    return (maya::Element{std::move(label)}
            | height(1) | overflow(maya::Overflow::Hidden)).build();
}

maya::Element dock_element_for_density(
    const DockConfig& dock, DockDensity density)
{
    using namespace maya::dsl;

    std::vector<maya::Element> rows;
    const bool permission = dock.permission.has_value();
    if (permission) {
        switch (density) {
            case DockDensity::Full:
            case DockDensity::NoBreathing:
            case DockDensity::CompactStatus:
            case DockDensity::NoGuidance:
            case DockDensity::CompactComposer:
                rows.push_back(maya::Permission{*dock.permission}.build());
                break;
            case DockDensity::PermissionCompact:
                rows.push_back(compact_permission_element(
                    *dock.permission, /*action_only=*/false));
                rows.push_back(compact_status_element(dock.status));
                break;
            case DockDensity::PermissionAction:
                rows.push_back(compact_permission_element(
                    *dock.permission, /*action_only=*/true));
                break;
            case DockDensity::TooSmall:
                rows.push_back(too_small_element());
                break;
        }

        if (density == DockDensity::Full) {
            rows.push_back(blank().build());
            rows.push_back(transcript_guidance_element(dock.transcript_guidance)
                           | height(1) | overflow(maya::Overflow::Hidden));
            append_queue_preview(rows, dock);
            rows.push_back(maya::Composer{dock.composer}.build());
            rows.push_back(maya::StatusBar{dock.status}.build());
        }
    } else {
        switch (density) {
            case DockDensity::Full:
                rows.push_back(blank().build());
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                append_queue_preview(rows, dock);
                rows.push_back(maya::Composer{dock.composer}.build());
                rows.push_back(maya::StatusBar{dock.status}.build());
                break;
            case DockDensity::NoBreathing:
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                rows.push_back(maya::Composer{dock.composer}.build());
                rows.push_back(maya::StatusBar{dock.status}.build());
                break;
            case DockDensity::CompactStatus:
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                append_queue_preview(rows, dock);
                rows.push_back(maya::Composer{dock.composer}.build());
                rows.push_back(compact_status_element(dock.status));
                break;
            case DockDensity::NoGuidance:
                append_queue_preview(rows, dock);
                rows.push_back(maya::Composer{dock.composer}.build());
                rows.push_back(compact_status_element(dock.status));
                break;
            case DockDensity::CompactComposer: {
                append_queue_preview(rows, dock);
                maya::Composer::Config composer = dock.composer;
                composer.hide_footer = true;
                rows.push_back(maya::Composer{std::move(composer)}.build());
                rows.push_back(compact_status_element(dock.status));
                break;
            }
            case DockDensity::PermissionCompact:
            case DockDensity::PermissionAction:
                break;
            case DockDensity::TooSmall:
                rows.push_back(too_small_element());
                break;
        }
    }

    return (v(std::move(rows)) | padding(0, 1)).build();
}

DockDensity choose_dock_density(
    const DockConfig& dock, int available_width, int terminal_rows)
{
    const std::vector<DockDensity> candidates = dock.permission
        ? std::vector<DockDensity>{
              DockDensity::Full,
              DockDensity::PermissionCompact,
              DockDensity::PermissionAction,
          }
        : std::vector<DockDensity>{
              DockDensity::Full,
              DockDensity::NoBreathing,
              DockDensity::CompactStatus,
              DockDensity::NoGuidance,
              DockDensity::CompactComposer,
              DockDensity::TooSmall,
          };

    const int transcript_rows = std::max(0, terminal_rows - 1);
    for (const DockDensity candidate : candidates) {
        const maya::Element element = dock_element_for_density(dock, candidate);
        const int natural_height = maya::measure_element(
            element, std::max(1, available_width)).height.value;
        if (natural_height <= transcript_rows) {
            return candidate;
        }
    }

    return candidates.back();
}

}  // namespace

maya::Element dock_element(const DockConfig& dock)
{
    // 直接调用保留完整 dock 语义，供 widget/接缝测试与非全屏调用使用。
    return dock_element_for_density(dock, DockDensity::Full);
}

maya::Element dock_element_for_height(
    const DockConfig& dock, int available_width, int terminal_rows)
{
    const DockDensity density = choose_dock_density(
        dock, available_width, terminal_rows);
    return dock_element_for_density(dock, density);
}

namespace {

const Message* message_for(const Model& model, MessageId id)
{
    if (id.value == 0) {
        return nullptr;
    }
    for (const Message& message : model.domain.thread.messages) {
        if (message.id == id) {
            return &message;
        }
    }
    return nullptr;
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
            } else if constexpr (
                std::is_same_v<Status, ToolCall::Cancelled>) {
                return "cancelled";
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

std::optional<maya::Permission::Config> permission_config(
    const ToolActivityProjection& activity)
{
    if (activity.kind != ToolActivityProjection::Kind::AwaitingPermission) {
        return std::nullopt;
    }

    maya::Permission::Config config;
    config.tool_name = activity.tool_name;
    config.description = "allow " + activity.tool_name + "? effect="
        + activity.effects + " args: " + activity.arguments;
    return config;
}

TranscriptGuidanceConfig transcript_guidance_config(
    const UiState& ui, const maya::Theme& theme)
{
    TranscriptGuidanceConfig config;
    switch (ui.transcript_hint) {
        case TranscriptHintKind::None:
            break;
        case TranscriptHintKind::Latest:
            config.full = {
                {.text = "Ctrl+End · latest",
                 .style = maya::Style{}.with_fg(theme.muted).with_dim()},
            };
            config.compact = {
                {.text = "↓ Ctrl+End",
                 .style = maya::Style{}.with_fg(theme.muted).with_dim()},
            };
            break;
        case TranscriptHintKind::NewOutput:
            config.full = {
                {.text = "↓ new output",
                 .style = maya::Style{}.with_fg(theme.info)},
                {.text = " · Ctrl+End",
                 .style = maya::Style{}.with_fg(theme.muted).with_dim()},
            };
            config.compact = {
                {.text = "↓ Ctrl+End",
                 .style = maya::Style{}.with_fg(theme.muted).with_dim()},
            };
            break;
    }
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
            } else if constexpr (std::is_same_v<Phase, my_agent::Compacting>) {
                return maya::Composer::State::Idle;
            } else if constexpr (std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return maya::Composer::State::AwaitingPermission;
            } else if constexpr (std::is_same_v<Phase, my_agent::Blocked>) {
                return maya::Composer::State::Idle;
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
            } else if constexpr (std::is_same_v<Phase, my_agent::Compacting>) {
                return theme.info;
            } else if constexpr (
                std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return theme.warning;
            } else if constexpr (
                std::is_same_v<Phase, my_agent::ExecutingTool>) {
                return theme.success;
            } else if constexpr (
                std::is_same_v<Phase, my_agent::Blocked>) {
                return theme.warning;
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
    const ComposerDisplayText display = render_paste_placeholders(
        ui.input,
        ui.cursor == std::string::npos ? ui.input.size() : ui.cursor,
        ui.pastes);
    maya::Composer::Config cfg;
    cfg.text = display.text;
    cfg.cursor = static_cast<int>(display.cursor);
    cfg.caret_mode = maya::Composer::CaretMode::SolidCell;
    cfg.state = composer_state(model);
    cfg.active_color = phase_color(model, theme);
    cfg.text_color = theme.text;
    cfg.accent_color = theme.accent;
    cfg.warn_color = theme.warning;
    cfg.highlight_color = theme.info;
    cfg.profile = profile_chip(model.domain.profile, theme);
    cfg.queued = model.session.queued_messages.size();
    cfg.queued_placeholder = "Ctrl-X clear queued prompts";
    cfg.min_body_rows = 1;
    return cfg;
}

std::optional<QueuePreviewConfig> queue_preview_config(
    const Model& model, const maya::Theme& theme)
{
    if (model.session.queued_messages.empty()) {
        return std::nullopt;
    }

    const QueuedMessage& head = model.session.queued_messages.front();
    std::string label;
    if (const auto* blocked = std::get_if<my_agent::Blocked>(&model.phase);
        blocked != nullptr
        && blocked->reason == BlockReason::ContextLimit
        && blocked->budget) {
        label = "ctx ";
        if (blocked->budget->estimated) {
            label += '~';
        }
        label += std::to_string(
            blocked->budget->projected_input_tokens);
        label += " > ";
        label += std::to_string(blocked->budget->hard_input_limit);
        label += " · estimate";
        if (context_debug_enabled()) {
            const auto& budget = *blocked->budget;
            label += " · dbg next=";
            if (budget.estimated) {
                label += '~';
            }
            label += std::to_string(budget.projected_input_tokens);
            if (const auto& anchor = model.session.projection_ledger.exact_anchor) {
                const std::uint64_t previous_total =
                    anchor->input_tokens + anchor->output_tokens;
                label += " prev=" + std::to_string(anchor->input_tokens)
                    + "+" + std::to_string(anchor->output_tokens)
                    + "=" + std::to_string(previous_total);
                label += budget.exact_anchor_reused
                    ? " anchor=hit"
                    : " anchor=miss";
                if (budget.exact_anchor_reused) {
                    label += " delta="
                        + std::to_string(budget.incremental_input_tokens);
                }
            } else {
                label += " prev=none anchor=miss";
            }
        }
        label += " · ";
    }
    label += "queued 1 / ";
    label += std::to_string(model.session.queued_messages.size());
    label += " · Ctrl-X clear";
    return QueuePreviewConfig{
        .label = std::move(label),
        .text = head.text,
        .label_style = maya::Style{}
            .with_fg(theme.info)
            .with_bold(),
        .text_style = maya::Style{}
            .with_fg(theme.muted)
            .with_dim(),
    };
}

maya::Conversation::Config transcript_config(
    const Model& model, const maya::Theme& theme)
{
    maya::Conversation::Config cfg;
    cfg.fill_available_height = false;
    const Message* last = nullptr;
    if (const auto* streaming = std::get_if<my_agent::Streaming>(
            &model.phase)) {
        last = message_for(model, streaming->target_message_id);
    }
    if (last == nullptr && !model.domain.thread.messages.empty()) {
        last = &model.domain.thread.messages.back();
    }
    const bool thinking =
        std::holds_alternative<my_agent::Streaming>(model.phase)
        && last != nullptr
        && last->role == Role::Assistant
        && last->text.empty()
        && last->tool_calls.empty()
        && !last->error.has_value();

    const std::size_t turn_count = model.domain.thread.messages.size()
        - (thinking ? 1u : 0u);
    cfg.turns.reserve(turn_count);
    for (std::size_t index = 0; index < turn_count; ++index) {
        const Message& msg = model.domain.thread.messages[index];
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

    if (thinking) {
        static constexpr std::string_view kThinking = "thinking";
        maya::ActivityIndicator::Config indicator;
        indicator.edge_color = theme.info;
        indicator.words.emplace_back(kThinking);
        cfg.in_flight = std::move(indicator);
    } else if (std::holds_alternative<my_agent::Compacting>(model.phase)) {
        static constexpr std::string_view kCompacting =
            "Compacting context…";
        maya::ActivityIndicator::Config indicator;
        indicator.edge_color = theme.info;
        indicator.words.emplace_back(kCompacting);
        cfg.in_flight = std::move(indicator);
    }
    return cfg;
}

struct PhaseDisplay {
    std::string glyph;
    std::string verb;
    bool active{false};
};

PhaseDisplay phase_display(
    const Model& model, const ToolActivityProjection& activity)
{
    return std::visit(
        [&model, &activity](const auto& phase) -> PhaseDisplay {
            using Phase = std::decay_t<decltype(phase)>;
            if constexpr (std::is_same_v<Phase, my_agent::PreparingRequest>) {
                return {
                    .glyph = "●",
                    .verb = std::holds_alternative<RetryRequest>(phase.intent)
                        ? "Retrying context request"
                        : "Preparing",
                    .active = true,
                };
            } else if constexpr (std::is_same_v<Phase, my_agent::Compacting>) {
                return {
                    .glyph = "●",
                    .verb = "Compacting context · "
                        + std::to_string(model.session.queued_messages.size())
                        + " queued",
                    .active = true,
                };
            } else if constexpr (std::is_same_v<Phase, my_agent::Streaming>) {
                return {.glyph = "●", .verb = "Thinking", .active = true};
            } else if constexpr (
                std::is_same_v<Phase, my_agent::AwaitingPermission>) {
                return {.glyph = "⚠", .verb = "Approve?", .active = true};
            } else if constexpr (std::is_same_v<Phase, my_agent::ExecutingTool>) {
                return {
                    .glyph = "●",
                    .verb = activity.kind == ToolActivityProjection::Kind::Executing
                        ? activity.tool_name
                        : "tool execution state invalid",
                    .active = true,
                };
            } else if constexpr (std::is_same_v<Phase, my_agent::Blocked>) {
                return {
                    .glyph = "!",
                    .verb = phase.reason == BlockReason::ContextLimit
                        ? "Ctx limit"
                        : "Blocked",
                    .active = false,
                };
            } else {
                return {.glyph = "○", .verb = "Ready", .active = false};
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

std::string usage_badge_text(
    std::string_view label,
    std::optional<std::size_t> input,
    std::optional<std::size_t> output,
    std::optional<UsageQuality> quality)
{
    if (quality && *quality == UsageQuality::Unavailable) {
        return std::string{label} + " unavailable";
    }
    if (!input && !output) {
        return std::string{label} + " unavailable";
    }
    const std::string prefix = quality
        && *quality == UsageQuality::Estimated ? "~" : "";
    return std::string{label} + " " + prefix + "in "
        + (input ? std::to_string(*input) : "unavailable")
        + " out " + (output ? std::to_string(*output) : "unavailable");
}

std::string context_quality_badge(
    std::optional<UsageQuality> quality)
{
    if (!quality) {
        return {};
    }
    switch (*quality) {
        case UsageQuality::Exact:
            return "ctx exact";
        case UsageQuality::Last:
            return "ctx last";
        case UsageQuality::Stale:
            return "ctx stale";
        case UsageQuality::Estimated:
            return "ctx estimated";
        case UsageQuality::Unavailable:
            return "ctx unavailable";
    }
    return {};
}

// StatusBar 与 agentty 使用同一种映射方式：每类信息进入对应的子组件，
// 不再把整行预先压成一个字符串。这样 Maya 才能按真实宽度逐级降级。
maya::StatusBar::Config status_config(
    const Model& model,
    const StatusBarInput& status,
    const TokenRateHistory& token_rate_history,
    const maya::Theme& theme,
    const ToolActivityProjection& activity)
{
    maya::StatusBar::Config cfg;
    const PhaseDisplay display = phase_display(model, activity);
    cfg.phase_color = phase_color(model, theme);
    cfg.phase.glyph = display.glyph;
    cfg.phase.verb = display.verb;
    if (activity.kind == ToolActivityProjection::Kind::Invalid) {
        cfg.phase.verb = activity.diagnostic + "; Ctrl-C to cancel";
    }
    cfg.phase.color = cfg.phase_color;
    cfg.phase.breathing = display.active;
    if (status.elapsed_seconds) {
        cfg.phase.elapsed_secs = static_cast<float>(*status.elapsed_seconds);
    }

    cfg.token_stream.color = theme.info;
    cfg.token_stream.live = std::holds_alternative<Streaming>(model.phase);
    cfg.token_stream.history = token_rate_history.samples();
    if (status.tokens_per_second) {
        const float rate = static_cast<float>(*status.tokens_per_second);
        cfg.token_stream.rate = rate;
    }
    if (status.tokens_generated) {
        cfg.token_stream.total = static_cast<int>(std::min<std::size_t>(
            *status.tokens_generated,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    if (display.active) {
        cfg.phase.frame = static_cast<int>(maya::anim::frame_index(
            32, 100.0));
    }

    // Keep request/turn telemetry as separate measured fragments. Maya's
    // status degradation ladder can then shed them without dropping the
    // model identity or observed-context quality with the same badge.
    std::vector<maya::Element> badge_parts;
    if (!status.model_name.empty()) {
        badge_parts.push_back(maya::ModelBadge{{
            .label = status.model_name,
            .version = {},
            .color = theme.primary,
            .show_dot = false,
        }}.build());
    }
    const auto& usage = model.session.usage;
    const std::optional<UsageQuality> context_quality = usage.observed_context
        ? std::optional{usage.observed_context->quality}
        : (status.context_limit ? std::optional{UsageQuality::Unavailable} : std::nullopt);
    if (const std::string quality = context_quality_badge(
            context_quality); !quality.empty()) {
        std::string content = quality;
        if (!badge_parts.empty()) {
            content = " · " + content;
        }
        badge_parts.push_back(maya::dsl::text(
            content, maya::Style{}.with_fg(theme.info)).build());
    }
    if (!badge_parts.empty()) {
        cfg.model_badge = maya::dsl::h(std::move(badge_parts)).build();
    }

    const auto usage_badge = [&theme](
        std::string_view label,
        std::optional<std::size_t> input,
        std::optional<std::size_t> output,
        std::optional<UsageQuality> quality) -> maya::Element {
        if (!input && !output && !quality) {
            return {};
        }
        return maya::dsl::text(
            usage_badge_text(label, input, output, quality),
            maya::Style{}.with_fg(theme.info)).build();
    };
    if (usage.latest_request) {
        const auto& request = *usage.latest_request;
        const bool available = request.quality != UsageQuality::Unavailable;
        cfg.request_usage = usage_badge("req",
            available ? std::optional<std::size_t>{request.totals.input_tokens} : std::nullopt,
            available ? std::optional<std::size_t>{request.totals.output_tokens} : std::nullopt,
            request.quality);
    }
    if (usage.turn) {
        cfg.turn_usage = usage_badge("turn",
            static_cast<std::size_t>(usage.turn->totals.input_tokens),
            static_cast<std::size_t>(usage.turn->totals.output_tokens),
            usage.current_turn_quality());
    }
    if (usage.observed_context && usage.observed_context->quality != UsageQuality::Unavailable) {
        const auto& totals = usage.observed_context->totals;
        cfg.context.used = status_count(static_cast<std::size_t>(
            totals.input_tokens + totals.output_tokens));
    }
    cfg.context.max = status_count(status.context_limit);
    cfg.context.cells = 10;

    return cfg;
}

}

ScreenConfig project_screen(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    const ToolActivityProjection activity = tool_activity_projection(model);
    return ScreenConfig{
        .transcript = transcript_config(model, theme),
        .dock = DockConfig{
            .permission = permission_config(activity),
            .transcript_guidance = transcript_guidance_config(ui, theme),
            .queue_preview = queue_preview_config(model, theme),
            .composer = composer_config(model, ui, theme),
            .status = status_config(model, ui.status, ui.token_rate_history, theme, activity)
        }
    };
}

}  // namespace my_agent::ui
