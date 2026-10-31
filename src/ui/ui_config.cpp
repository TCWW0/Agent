#include "my_agent/ui/ui_config.hpp"

#include <charconv>
#include <string_view>

namespace my_agent::ui {

UiConfig UiConfig::from_environment_value(const char* value) noexcept
{
    if (value == nullptr || *value == '\0') {
        return {};
    }

    const std::string_view text{value};
    int rows = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), rows);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || rows < 1 || rows > 20) {
        return {};
    }
    return UiConfig{.mouse_scroll_rows = rows};
}

}  // namespace my_agent::ui
