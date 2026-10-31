#include "../src/tool/calculator.hpp"
#include "../src/tool/memory_tools.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "../src/tool/read.hpp"
#include "../src/tool/skill_tools.hpp"

#include <concepts>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace tool = my_agent::tool;

static_assert(std::same_as<
    decltype(tool::detail::make_calculator_tool()),
    tool::RegisteredTool>);
static_assert(std::same_as<
    decltype(tool::detail::make_read_tool(std::filesystem::path{})),
    tool::RegisteredTool>);
static_assert(std::same_as<
    decltype(tool::detail::make_memory_tools(
        std::declval<std::shared_ptr<tool::memory::MemoryStore>>())),
    std::vector<tool::RegisteredTool>>);
static_assert(std::same_as<
    decltype(tool::detail::make_skill_tool(
        std::declval<std::shared_ptr<tool::skills::SkillEngine>>())),
    tool::RegisteredTool>);
static_assert(std::same_as<
    decltype(tool::docs::make_search_docs_tool(tool::docs::DocsConfig{})),
    tool::RegisteredTool>);

TEST(TS007NativeRegistrationContractTest, FactoryReturnTypesAreNative)
{
    SUCCEED();
}
