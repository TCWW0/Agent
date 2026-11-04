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
#include <cstdint>
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

maya::Element compact_status_element(const StatusBarConfig& config)
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

maya::Element compact_composer_element(const ComposerConfig& config)
{
    using namespace maya::dsl;

    maya::Color border_color = config.accent_color;
    if (config.state == maya::Composer::State::AwaitingPermission) {
        border_color = config.warn_color;
    } else if (config.state == maya::Composer::State::Streaming
               || config.state == maya::Composer::State::ExecutingTool) {
        border_color = config.active_color;
    }

    std::string content = "\xe2\x9d\xaf ";  // ❯
    content += config.text;
    maya::TextElement input{
        .content = std::move(content),
        .style = maya::Style{}.with_fg(config.text_color),
        .wrap = maya::TextWrap::TruncateEnd,
    };
    return (v(maya::Element{std::move(input)})
            | padding(0, 1)
            | border(maya::BorderStyle::Round)
            | bcolor(border_color)).build();
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

maya::Element turn_slot_element(const maya::Turn::BodySlot& slot)
{
    return std::visit(
        [](const auto& value) -> maya::Element {
            using Slot = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Slot, maya::Turn::PlainText>) {
                return maya::dsl::text(
                    value.content,
                    maya::Style{}.with_fg(value.color)).build();
            } else if constexpr (std::is_same_v<
                                     Slot, maya::Turn::MarkdownText>) {
                return maya::markdown(value.content);
            } else if constexpr (std::is_same_v<
                                     Slot, maya::AgentTimeline::Config>) {
                return maya::AgentTimeline{value}.build();
            } else if constexpr (std::is_same_v<
                                     Slot, maya::Permission::Config>) {
                return maya::Permission{value}.build();
            } else {
                return value;
            }
        },
        slot);
}

bool empty_text_element(const maya::Element& element)
{
    const maya::TextElement* text = maya::as_text(element);
    return text != nullptr && text->content.empty();
}

std::vector<maya::Element> turn_body_elements(
    const maya::Turn::Config& config)
{
    using namespace maya::dsl;
    std::vector<maya::Element> rows;
    rows.reserve(config.body.size() * 2);
    for (const auto& slot : config.body) {
        maya::Element element = turn_slot_element(slot);
        if (empty_text_element(element)) {
            continue;
        }
        if (!rows.empty()) {
            rows.push_back(blank().build());
        }
        rows.push_back(std::move(element));
    }
    return rows;
}

// D3's Turn widget lets its header/rail container consume the body's allocation
// when embedded in this project's independently-scrolled transcript. Keep the
// renderer dependency, but own a quieter host composition: a colored speaker
// header and a borderless two-column body indent. This preserves multi-row
// Markdown without a hanging rail or a project-specific Maya patch.
maya::Element turn_element(const maya::Turn::Config& config)
{
    using namespace maya::dsl;

    std::vector<maya::Element> rows;
    if (!config.continuation) {
        std::vector<maya::Element> header_parts;
        header_parts.reserve(4);
        header_parts.push_back(text(
            config.glyph,
            maya::Style{}.with_fg(config.rail_color)).build());
        header_parts.push_back(text(" ").build());
        header_parts.push_back(text(
            config.label,
            maya::Style{}.with_fg(config.rail_color).with_bold()).build());
        header_parts.push_back(spacer().build());
        if (config.meta_element) {
            header_parts.push_back(*config.meta_element);
        } else {
            header_parts.push_back(text(
                config.meta,
                maya::Style{}.with_fg(maya::Color::bright_black())
                    .with_dim()).build());
        }
        header_parts.push_back(text(" ").build());
        rows.push_back(h(std::move(header_parts)).build());
        rows.push_back(blank().build());
    }

    std::vector<maya::Element> body = turn_body_elements(config);
    if (!body.empty()) {
        rows.push_back(maya::detail::box()
            .direction(maya::FlexDirection::Column)
            .padding(0, 0, 0, 2)
            (v(std::move(body)).build()));
    }
    if (!config.error.empty()) {
        rows.push_back(blank().build());
        rows.push_back(maya::detail::box()
            .direction(maya::FlexDirection::Column)
            .padding(0, 0, 0, 2)
            (h(
                text("\xe2\x9a\xa0  ",
                     maya::Style{}.with_fg(maya::Color::red()).with_bold()),
                text(config.error,
                     maya::Style{}.with_fg(maya::Color::red())
                         .with_dim().with_italic())).build()));
    }

    maya::Element turn = (v(std::move(rows)) | grow(1.0f)).build();

    if (!config.checkpoint_above) {
        return turn;
    }
    return v(
        maya::CheckpointDivider{{
            .label = config.checkpoint_label,
            .color = config.checkpoint_color,
        }}.build(),
        std::move(turn)).build();
}

maya::Element inter_turn_divider()
{
    using namespace maya::dsl;
    return component([](int width, int) {
        constexpr int body_indent = 2;
        if (width <= body_indent) {
            return blank().build();
        }

        std::string line(static_cast<std::size_t>(body_indent), ' ');
        for (int column = body_indent; column < width; ++column) {
            line += "─";
        }
        return text(std::move(line),
            maya::Style{}.with_fg(maya::Color::bright_black()).with_dim())
            .build();
    });
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
            rows.push_back(composer_element(dock.composer));
            rows.push_back(status_bar_element(dock.status));
        }
    } else {
        switch (density) {
            case DockDensity::Full:
                rows.push_back(blank().build());
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                append_queue_preview(rows, dock);
                rows.push_back(composer_element(dock.composer));
                rows.push_back(status_bar_element(dock.status));
                break;
            case DockDensity::NoBreathing:
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                rows.push_back(composer_element(dock.composer));
                rows.push_back(status_bar_element(dock.status));
                break;
            case DockDensity::CompactStatus:
                rows.push_back(transcript_guidance_element(
                    dock.transcript_guidance)
                    | height(1) | overflow(maya::Overflow::Hidden));
                append_queue_preview(rows, dock);
                rows.push_back(composer_element(dock.composer));
                rows.push_back(compact_status_element(dock.status));
                break;
            case DockDensity::NoGuidance:
                append_queue_preview(rows, dock);
                rows.push_back(composer_element(dock.composer));
                rows.push_back(compact_status_element(dock.status));
                break;
            case DockDensity::CompactComposer: {
                append_queue_preview(rows, dock);
                rows.push_back(compact_composer_element(dock.composer));
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

maya::Element conversation_element(const ConversationConfig& config)
{
    if (config.fill_available_height) {
        const auto& base = static_cast<const maya::Conversation::Config&>(
            config);
        return maya::Conversation{base}.build();
    }

    using namespace maya::dsl;
    std::vector<maya::Element> rows;
    rows.reserve(config.turns.size() * 2 + 1);
    for (std::size_t index = 0; index < config.turns.size(); ++index) {
        if (index > 0 && !config.turns[index].continuation) {
            rows.push_back(inter_turn_divider());
        }
        const bool compact_tail = config.compact_tail
            && index + 1 == config.turns.size();
        if (!compact_tail) {
            rows.push_back(turn_element(config.turns[index]));
            continue;
        }

        std::vector<maya::Element> body = turn_body_elements(
            config.turns[index]);
        rows.push_back(v(std::move(body)).build());
    }
    if (config.in_flight) {
        rows.push_back(maya::ActivityIndicator{*config.in_flight}.build());
    }
    return (v(std::move(rows)) | padding(0, 1)).build();
}

maya::Element composer_element(const ComposerConfig& config)
{
    maya::Composer::Config base =
        static_cast<const maya::Composer::Config&>(config);

    if (config.solid_cell) {
        // Maya's stock painted caret inserts a block. Remove the code point
        // under the cursor first so that insertion becomes replacement and
        // therefore cannot shift the suffix. Expiring the stock blink keeps
        // that replacement solid and avoids a second animation cadence.
        const std::size_t cursor = static_cast<std::size_t>(std::clamp(
            base.cursor, 0, static_cast<int>(base.text.size())));
        if (cursor < base.text.size()) {
            std::size_t next = cursor + 1;
            while (next < base.text.size()
                   && (static_cast<unsigned char>(base.text[next]) & 0xc0)
                       == 0x80) {
                ++next;
            }
            base.text.erase(cursor, next - cursor);
        }
        base.last_edit_ms = 1;
    }

    return maya::Composer{std::move(base)}.build();
}

maya::Element status_bar_element(const StatusBarConfig& config)
{
    using namespace maya::dsl;

    const auto& base = static_cast<const maya::StatusBar::Config&>(config);
    if (!config.status_banner.text.empty()) {
        return maya::StatusBar{base}.build();
    }

    constexpr std::string_view separator = " \xc2\xb7 ";
    constexpr int separator_width = maya::unicode::str_width(separator);
    const auto dim = [](maya::Color color) {
        const bool already_muted =
            color.kind() == maya::Color::Kind::Named
            && color.index() == static_cast<std::uint8_t>(
                maya::AnsiColor::BrightBlack);
        return already_muted
            ? maya::Style{}.with_fg(color)
            : maya::Style{}.with_fg(color).with_dim();
    };

    maya::Element activity = component(
        [config, dim, separator](int width, int) {
        using namespace maya::dsl;
        if (width <= 0) {
            return blank().build();
        }

        const maya::Color muted = maya::Color::bright_black();
        const auto width_of = [](const maya::Element& element) {
            return maya::measure_element(element, 1 << 14).width.value;
        };

        struct Shape {
            bool tokens;
            bool stream;
            bool elapsed;
            bool bar;
            bool verb;
            bool gauge;
            bool request;
            bool turn;
            bool badge;
        };
        static constexpr Shape ladder[] = {
            {true, true, true, true, true, true, true, true, true},
            {false, true, true, true, true, true, true, true, true},
            {false, true, true, true, true, true, true, false, true},
            {false, true, true, true, true, true, false, false, true},
            {false, false, true, true, true, true, false, false, true},
            {false, false, false, true, true, true, false, false, true},
            {false, false, false, false, true, true, false, false, true},
            {false, false, false, false, false, true, false, false, true},
            {false, false, false, false, false, false, false, false, true},
            {false, false, false, false, false, false, false, false, false},
        };

        maya::Element right;
        maya::Element phase_element;
        maya::Element middle;
        int fixed_width = 0;
        bool kept_verb = false;
        bool kept_gauge = false;
        for (const Shape& shape : ladder) {
            maya::PhaseChip::Config phase = config.phase;
            phase.verb_width = shape.verb ? 10 : 0;
            if (!shape.elapsed) {
                phase.elapsed_secs = -1.0f;
            }

            maya::ContextGauge::Config context = config.context;
            context.show_bar = shape.bar;
            context.show_tokens = shape.tokens;

            std::vector<maya::Element> parts;
            bool emitted = false;
            auto append = [&](maya::Element element) {
                if (emitted) {
                    parts.push_back(text(separator, dim(muted)));
                }
                parts.push_back(std::move(element));
                emitted = true;
            };
            if (shape.stream && !config.token_stream.adaptive) {
                append(maya::TokenStreamSparkline{
                    config.token_stream}.build());
            }
            if (shape.badge && width_of(config.model_badge) > 0) {
                append(config.model_badge);
            }
            if (shape.request && width_of(config.request_usage) > 0) {
                append(config.request_usage);
            }
            if (shape.turn && width_of(config.turn_usage) > 0) {
                append(config.turn_usage);
            }
            if (shape.gauge && config.context.max > 0) {
                append(maya::ContextGauge{context}.build());
            }
            if (emitted) {
                parts.push_back(text(" "));
            }

            maya::Element candidate_right = h(std::move(parts)).build();
            maya::Element candidate_phase = maya::PhaseChip{phase}.build();
            const bool has_right = width_of(candidate_right) > 0;
            const int middle_min =
                shape.stream && config.token_stream.adaptive
                ? 8 + separator_width
                : (has_right ? 1 : 0);
            const int needed = 3 + width_of(candidate_phase) + middle_min
                + width_of(candidate_right);
            const bool last = &shape == &ladder[std::size(ladder) - 1];
            if (needed <= width || last) {
                right = std::move(candidate_right);
                phase_element = std::move(candidate_phase);
                fixed_width = needed;
                kept_verb = shape.verb;
                kept_gauge = shape.gauge && config.context.max > 0;
                middle = shape.stream && config.token_stream.adaptive
                    ? (h(maya::TokenStreamSparkline{
                            config.token_stream}.build(),
                         text(separator, dim(muted))) | grow(1.0f)).build()
                    : (has_right
                        ? (h(spacer(), text(" ")) | grow(1.0f)).build()
                        : maya::Element{spacer().build()});
                break;
            }
        }

        std::vector<maya::Element> left_parts;
        left_parts.push_back(text(" "));
        if (!config.breadcrumb.title.empty() && kept_verb && kept_gauge) {
            constexpr int chip_decor = 2;
            constexpr int min_title = 12;
            const int leftover = width - fixed_width - separator_width;
            if (leftover >= chip_decor + min_title) {
                maya::TitleChip::Config breadcrumb = config.breadcrumb;
                const std::size_t cap = breadcrumb.max_chars > 0
                    ? breadcrumb.max_chars
                    : 28;
                breadcrumb.max_chars = std::min<std::size_t>(
                    static_cast<std::size_t>(leftover - chip_decor), cap);
                maya::Element chip = maya::TitleChip{breadcrumb}.build();
                if (width_of(chip) + separator_width
                    <= width - fixed_width) {
                    left_parts.push_back(std::move(chip));
                    left_parts.push_back(text(separator, dim(muted)));
                }
            }
        }
        const maya::Style rail_style = config.phase.breathing
            ? maya::Style{}.with_fg(config.phase_color).with_bold()
            : maya::Style{}.with_fg(config.phase_color).with_dim();
        left_parts.push_back(text("\xe2\x96\x8c", rail_style));
        left_parts.push_back(text(" "));
        left_parts.push_back(std::move(phase_element));

        return (h(h(std::move(left_parts)), std::move(middle),
                  std::move(right))
                | overflow(maya::Overflow::Hidden)).build();
        });

    std::vector<maya::Element> rows;
    rows.reserve(3);
    rows.push_back(maya::PhaseAccent{{
        .color = config.phase_color,
        .position = maya::PhaseAccent::Position::Top,
    }}.build());
    rows.push_back((std::move(activity)
                    | height(1) | overflow(maya::Overflow::Hidden)).build());
    rows.push_back(maya::PhaseAccent{{
        .color = config.phase_color,
        .position = maya::PhaseAccent::Position::Bottom,
    }}.build());
    return v(std::move(rows)).build();
}

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

ComposerConfig composer_config(
    const Model& model, const UiState& ui, const maya::Theme& theme)
{
    const ComposerDisplayText display = render_paste_placeholders(
        ui.input,
        ui.cursor == std::string::npos ? ui.input.size() : ui.cursor,
        ui.pastes);
    ComposerConfig cfg;
    cfg.text = display.text;
    cfg.cursor = static_cast<int>(display.cursor);
    cfg.solid_cell = true;
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

ConversationConfig transcript_config(
    const Model& model,
    std::string_view model_name,
    const maya::Theme& theme)
{
    ConversationConfig cfg;
    cfg.fill_available_height = false;
    std::string_view assistant_label = model_name;
    if (assistant_label.empty()) {
        assistant_label = model.domain.request_policy.provider_model();
    }
    if (assistant_label.empty()) {
        assistant_label = "Assistant";
    }
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
            turn.glyph = "❯";
            turn.label = "You";
            turn.rail_color = theme.accent;    // 用户消息：强调色（旧 IR 同语义）
            turn.body.emplace_back(
                maya::Turn::PlainText{.content = msg.text});
        } else {
            turn.glyph = "✦";
            turn.label = assistant_label;
            turn.rail_color = theme.primary;   // 助手消息：主色（阅读主体）
            turn.body.emplace_back(
                maya::Turn::MarkdownText{.content = msg.text});
        }
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
StatusBarConfig status_config(
    const Model& model,
    const StatusBarInput& status,
    const TokenRateHistory& token_rate_history,
    const maya::Theme& theme,
    const ToolActivityProjection& activity)
{
    StatusBarConfig cfg;
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
        .transcript = transcript_config(model, ui.status.model_name, theme),
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
