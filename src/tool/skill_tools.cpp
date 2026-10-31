#include "skill_tools.hpp"

#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace my_agent::tool::detail {
namespace {

ExecResult execute_skill(
    const skills::SkillEngine& engine,
    const nlohmann::json& args
)
{
    if (!args.is_object() || !args.contains("name") || !args["name"].is_string()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::InvalidArgs,
            .message = "skill requires a string name from the skills catalog",
        });
    }

    const std::string name = args["name"].get<std::string>();
    const std::optional<std::string> payload = engine.activate(name);
    if (payload) {
        return ToolOutput{.text = *payload};
    }

    // 带上真实可用的名字：模型会写错名字或凭印象编一个，看到实际清单才能一次
    // 纠正，而不是反复猜。
    std::string available;
    for (const skills::Skill& skill : engine.all()) {
        available += available.empty() ? "" : ", ";
        available += skill.name;
    }

    return std::unexpected(ToolError{
        .kind = ErrorKind::NotFound,
        .message = "unknown skill: " + name
            + (available.empty() ? " (no skills are installed)"
                                 : " (available: " + available + ")"),
    });
}

}  // namespace

RegisteredTool make_skill_tool(
    std::shared_ptr<skills::SkillEngine> engine
)
{
    return RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "skill"},
            .description =
                "Load the full instructions of a skill listed in the skills "
                "catalog. Do this when its task comes up rather than guessing what "
                "the skill says.",
            .input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"name", nlohmann::json{
                    {"type", "string"},
                    {"minLength", 1},
                    {"description", "Exact skill name from the skills catalog."},
                }},
            }},
            {"required", nlohmann::json::array({"name"})},
            {"additionalProperties", false},
        },
        // activate 只读 SKILL.md，所以是 ReadFs 而不是 WriteFs。如实声明是纪律：
        // 谎报 effects 会让整个 policy 层失去可信度。
            .effects = {Effect::ReadFs},
            .replay = {},
        },
        .handler = [engine = std::move(engine)](
            const nlohmann::json& args,
            const ToolExecutionContext&) {
            return execute_skill(*engine, args);
        },
    };
}

}  // namespace my_agent::tool::detail
