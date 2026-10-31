#pragma once

#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <memory>
#include <vector>

namespace my_agent::tool::detail {

// remember 与 forget 两个原生注册。store 由调用方注入并共享 —— 两个工具操作同一
// 份存储，测试也能喂一个指向临时目录的 store。
[[nodiscard]]
std::vector<RegisteredTool> make_memory_tools(
    std::shared_ptr<memory::MemoryStore> store
);

}  // namespace my_agent::tool::detail
