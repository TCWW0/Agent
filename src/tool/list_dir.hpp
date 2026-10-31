#pragma once

#include "my_agent/tool/tool_runtime.hpp"

#include <memory>

namespace my_agent::tool::detail {

class ReadRoots;

[[nodiscard]] RegisteredTool make_list_dir_tool(std::shared_ptr<ReadRoots> roots);

} // namespace my_agent::tool::detail
