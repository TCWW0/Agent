#pragma once
// my_agent::ui::screen —— 语义屏幕：transcript 与 dock 的分离（issue #26）。
//
// 分层对应：本层与 agentty 的 view/thread/thread_config() 同位 —— Model →
// widget Config 的纯数据抽取。视觉与布局来自 maya 现成 widget
// （Conversation / Composer / StatusBar / Permission），本项目不重写它们；
// 差别在组合：agentty 把 composer/status 塞进 maya::AppLayout::Config 交给
// widget 组合，我们的 dock 组合语义（permission slot 的位置、空行与 gutter
// 规则）归本层所有，布局片（#26 后续红）再落成 Element。
//
// 纯投影：不碰终端、不碰时钟、不碰文件。
// 不收 Size —— 宽度属于布局层（maya RenderContext），语义层与坐标系无关。

#include <optional>
#include <string>
#include <vector>

#include <maya/element/element.hpp>
#include <maya/style/theme.hpp>
#include <maya/style/style.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/permission.hpp>
#include <maya/widget/status_bar.hpp>

#include "my_agent/runtime/model.hpp"
#include "my_agent/ui/ui_state.hpp"

namespace my_agent::ui {

// Project-owned host adapters. Maya remains the rendering backend; fields
// that express my_agent policy live here instead of in a private Maya fork.
struct ConversationConfig : maya::Conversation::Config {
    bool fill_available_height = false;
    bool compact_tail = false;
};

struct ComposerConfig : maya::Composer::Config {
    bool solid_cell = false;
    std::string queued_placeholder;
};

struct StatusBarConfig : maya::StatusBar::Config {
    maya::Element request_usage;
    maya::Element turn_usage;
};

struct TranscriptGuidanceConfig {
    struct Run {
        std::string text;
        maya::Style style{};
    };

    std::vector<Run> full;
    std::vector<Run> compact;
};

// The queue is domain state, but its waiting preview belongs to the fixed dock
// rather than the scrollable transcript. It is deliberately one compact row:
// the label carries FIFO position/count and the text is truncated by the
// layout element when the terminal is narrow.
struct QueuePreviewConfig {
    std::string label;
    std::string text;
    maya::Style label_style{};
    maya::Style text_style{};
};

// 屏幕的 dock：活动中的 chrome。permission slot 是领域决策的出口，
// guidance 是 transcript 导航提示，queue preview 是待消费队头，composer 是
// 输入焦点，status 是相位与开销。它们都在 dock 里，不进 transcript。
struct DockConfig {
    std::optional<maya::Permission::Config> permission;
    TranscriptGuidanceConfig transcript_guidance;
    std::optional<QueuePreviewConfig> queue_preview;
    ComposerConfig composer;
    StatusBarConfig status;
};

// 语义屏幕。transcript 只装已结算的对话内容；一切「活着」的东西都在 dock。
struct ScreenConfig {
    ConversationConfig transcript;
    DockConfig dock;
};

// Model + UiState → 语义屏幕的投影。纯函数。配色走 Theme（语义色 →
// 实际色的解析在投影内完成），主题由调用方注入。
[[nodiscard]]
ScreenConfig project_screen(
    const Model& model, const UiState& ui, const maya::Theme& theme);

// Translate the project-owned adapter configs into Maya element trees.
[[nodiscard]]
maya::Element conversation_element(const ConversationConfig& config);

[[nodiscard]]
maya::Element composer_element(const ComposerConfig& config);

[[nodiscard]]
maya::Element status_bar_element(const StatusBarConfig& config);

// dock 的布局翻译：DockConfig → maya::Element。布局契约（issue #26 验收 2）：
// permission slot（如有）居顶；composer 上方保留 breathing row 与 guidance row；
// composer 紧贴 status 上方；status 之下不画任何行；左右各一列 gutter。
// transcript 的布局与整屏贴底策略不在此层 —— 全屏 adapter（painter.cpp）
// 持有它们，本函数只对 dock 自身的内部规则负责。
[[nodiscard]]
maya::Element dock_element(const DockConfig& dock);

// Height-aware dock used by the fullscreen adapter. The selected candidate is
// measured at the real terminal width and must leave one transcript row when
// the terminal has at least one row to spare. This is a layout policy only;
// scroll geometry remains owned by the transcript viewport.
[[nodiscard]]
maya::Element dock_element_for_height(
    const DockConfig& dock, int available_width, int terminal_rows);

}  // namespace my_agent::ui
