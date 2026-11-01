#pragma once

#include "my_agent/tool/tool_runtime.hpp"
namespace my_agent::tool::detail {
    [[nodiscard]]
    RegisteredTool make_calculator_tool();
}
