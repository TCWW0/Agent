#pragma once

#include "my_agent/tool/tool_runtime.hpp"
#include "read_roots.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace my_agent::tool::detail {

// allowed_roots 把可读范围从 workspace 放宽到「workspace ∪ /tmp ∪ 这些根」。
// 会话服务还可以在注册后添加目录；skill 常装在 $HOME 下，正文会引用同目录的
// 脚本和参考文件（Tier3）。
//
// **只放宽读。** 这个列表不参与任何写入判断 —— 一个装在 $HOME 的 skill 目录可读，
// 不意味着 agent 可以往那里写。
[[nodiscard]]
RegisteredTool make_read_tool(
    std::filesystem::path workspace_root,
    std::vector<std::filesystem::path> allowed_roots = {}
);

[[nodiscard]] RegisteredTool make_read_tool(std::shared_ptr<ReadRoots> roots);

} // namespace my_agent::tool::detail
