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
// 旧的自有 Frame IR（view.hpp）逐片退役，本结构是它的后继者。
// 与 view() 同一条纪律：纯函数，不碰终端、不碰时钟、不碰文件。
// 不收 Size —— 宽度属于布局层（maya RenderContext），语义层与坐标系无关，
// 这是旧 Frame「不带屏幕尺寸」原则的延续。

#include <optional>

#include <maya/style/theme.hpp>
#include <maya/widget/composer.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/permission.hpp>
#include <maya/widget/status_bar.hpp>

#include "my_agent/runtime/model.hpp"
#include "my_agent/ui/view.hpp"

namespace my_agent::ui {

// 屏幕的 dock：活动中的 chrome。permission slot 是领域决策的出口，
// composer 是输入焦点，status 是相位与开销。三者在 dock 里，不进 transcript。
struct DockConfig {
    std::optional<maya::Permission::Config> permission;
    maya::Composer::Config composer;
    maya::StatusBar::Config status;
};

// 语义屏幕。transcript 只装已结算的对话内容；一切「活着」的东西都在 dock。
struct ScreenConfig {
    maya::Conversation::Config transcript;
    DockConfig dock;
};

// Model + UiState → 语义屏幕的投影。纯函数。配色走 Theme（语义色 →
// 实际色的解析在投影内完成），主题由调用方注入 —— 与旧链路
// to_maya_element(frame, theme) 同一条纪律：颜色策略从正门进来。
[[nodiscard]]
ScreenConfig project_screen(
    const Model& model, const UiState& ui, const maya::Theme& theme);

}  // namespace my_agent::ui
