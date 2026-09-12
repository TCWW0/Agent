#include "read.hpp"
#include "memory_tools.hpp"
#include "skill_tools.hpp"
#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool.hpp"

#include <cmath>
#include <memory>
#include <utility>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>

namespace my_agent::tool {
namespace {

    namespace fs = std::filesystem;

    fs::path capture_workspace_root()
    {
        std::error_code error;
        fs::path root = fs::current_path(error);
        if(error){
            throw std::runtime_error{
                "fail to capture workspace root: "+
                error.message()
            };
        }

        root = fs::canonical(root,error);
        if (error) {
            throw std::runtime_error{
                "failed to canonicalize workspace root: "+
                error.message()
            };
        }

        return root;
    }

    ExecResult execute_calculator(const nlohmann::json& args)
    {
        if (!args.is_object()
            || !args.contains("operation")
            || !args["operation"].is_string()
            || !args.contains("left")
            || !args["left"].is_number()
            || !args.contains("right")
            || !args["right"].is_number()) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "calculator requires operation, left, and right",
            });
        }

        const std::string operation = args["operation"].get<std::string>();
        if (operation != "multiply") {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "unsupported calculator operation: " + operation,
            });
        }

        const double value =
            args["left"].get<double>() * args["right"].get<double>();
        if (!std::isfinite(value)) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "calculator result is not finite",
            });
        }

        return ToolOutput{.text = std::format("{}", value)};
    }

    ToolDef calculator()
    {
        return ToolDef{
            .name = "calculator",
            .description = "Multiply two numbers.",
            .input_schema = nlohmann::json{
                {"type", "object"},
                {"properties", nlohmann::json{
                    {"operation", nlohmann::json{
                        {"type", "string"},
                        {"enum", nlohmann::json::array({"multiply"})},
                    }},
                    {"left", nlohmann::json{{"type", "number"}}},
                    {"right", nlohmann::json{{"type", "number"}}},
                }},
                {"required", nlohmann::json::array({
                    "operation",
                    "left",
                    "right",
                })},
                {"additionalProperties", false},
            },
            .effects = {},
            .execute = execute_calculator,
        };
    }

    std::vector<ToolDef> build_registry()
    {
        fs::path workspace_root = capture_workspace_root();

        // engine 要在 read 之前建好：read 的允许列表需要 skill 目录，否则装在
        // $HOME 下的 skill 引用的资源文件会被 workspace 边界拒掉（Tier3）。
        auto skill_engine = std::make_shared<skills::SkillEngine>(
            skills::discover_roots()
        );

        std::vector<ToolDef> tools;
        tools.push_back(calculator());
        tools.push_back(detail::make_read_tool(
            std::move(workspace_root),
            skill_engine->directories()
        ));

        // remember / forget 共享一个 store，环境发现只做一次。
        auto memory_store = std::make_shared<memory::MemoryStore>(
            memory::discover_roots()
        );
        for (ToolDef& tool : detail::make_memory_tools(std::move(memory_store))) {
            tools.push_back(std::move(tool));
        }

        for (ToolDef& tool : detail::make_skill_tools(std::move(skill_engine))) {
            tools.push_back(std::move(tool));
        }

        // search_docs（#43）：懒建库 —— 首次调用才 fingerprint walk + build，
        // 注册本身零 IO 零网络。
        tools.push_back(docs::make_search_docs_tool());

        return tools;
    }

} // namespace

std::string ToolError::render() const
{
    switch (kind) {
        case ErrorKind::InvalidArgs:
            return "[invalid args] " + message;
        case ErrorKind::NotFound:
            return "[not found] " + message;
        case ErrorKind::OutOfWorkspace:
            return "[out of workspace] " + message;
        case ErrorKind::ExecutionFailed:
            return "[execution failed] " + message;
    }
    return message;
}

const std::vector<ToolDef>& registry()
{
    static const std::vector<ToolDef> tools = build_registry();
    return tools;
}

const ToolDef* find(std::string_view name)
{
    for (const ToolDef& definition : registry()) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

ExecResult execute(std::string_view name, const nlohmann::json& args)
{
    const ToolDef* definition = find(name);
    if (definition == nullptr) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::NotFound,
            .message = "unknown tool: " + std::string{name},
        });
    }
    return definition->execute(args);
}

} // namespace my_agent::tool
