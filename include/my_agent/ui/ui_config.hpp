#pragma once

namespace my_agent::ui {

struct UiConfig {
    static constexpr int default_mouse_scroll_rows = 3;

    int mouse_scroll_rows{default_mouse_scroll_rows};

    [[nodiscard]]
    static UiConfig from_environment_value(const char* value) noexcept;
};

}  // namespace my_agent::ui
