#include "my_agent/ui/screen.hpp"

namespace my_agent::ui {

// Red 桩（issue #26 红 1）：结构与签名已定，投影逻辑待 Green 实现。
// 返回默认构造 —— transcript 空、dock 全默认，测试应当红。
ScreenConfig project_screen(const Model& /*model*/, const UiState& /*ui*/)
{
    return ScreenConfig{};
}

}  // namespace my_agent::ui
