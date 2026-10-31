#pragma once

#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <memory>

namespace my_agent::tool::detail {

// engine 由调用方注入并共享：目录扫描与 activate 用同一个发现结果，
// 且测试可以喂临时目录。
[[nodiscard]]
RegisteredTool make_skill_tool(
    std::shared_ptr<skills::SkillEngine> engine
);

}  // namespace my_agent::tool::detail
