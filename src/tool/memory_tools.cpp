#include "memory_tools.hpp"

#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace my_agent::tool::detail {
namespace {

ExecResult execute_remember(
    memory::MemoryStore& store,
    const nlohmann::json& args
)
{
    if (!args.is_object() || !args.contains("text") || !args["text"].is_string()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::InvalidArgs,
            .message = "remember requires a string text",
        });
    }

    // 缺省 project：写错了只影响当前工作区，而默认全局会污染每一个其他工作区的系统提示。
    memory::Scope scope = memory::Scope::Project;
    if (args.contains("scope")) {
        if (!args["scope"].is_string()) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "remember scope must be a string",
            });
        }

        const std::string name = args["scope"].get<std::string>();
        const std::optional<memory::Scope> parsed = memory::parse_scope(name);
        if (!parsed) {
            return std::unexpected(ToolError{
                .kind = ErrorKind::InvalidArgs,
                .message = "unknown memory scope: " + name
                    + R"( (expected "user" or "project"))",
            });
        }
        scope = *parsed;
    }

    const memory::AppendResult result =
        store.append(scope, args["text"].get<std::string>());
    if (!result.error.empty()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::ExecutionFailed,
            .message = result.error,
        });
    }

    // id 要回给模型：系统提示里的记录带 id，模型据此才能精确 forget。
    return ToolOutput{
        .text = "remembered [" + result.id + "] in "
            + std::string{memory::to_string(scope)} + " scope",
    };
}

ExecResult execute_forget(
    memory::MemoryStore& store,
    const nlohmann::json& args
)
{
    if (!args.is_object()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::InvalidArgs,
            .message = "forget requires an object with id or text",
        });
    }

    const bool has_id = args.contains("id") && args["id"].is_string()
        && !args["id"].get<std::string>().empty();
    const bool has_text = args.contains("text") && args["text"].is_string()
        && !args["text"].get<std::string>().empty();

    // 无参 forget 若被当成空子串就会清空整个存储 —— 在入口拒绝。
    if (!has_id && !has_text) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::InvalidArgs,
            .message = "forget requires either an id or a non-empty text to match",
        });
    }

    const std::size_t removed = has_id
        ? store.forget_by_id(args["id"].get<std::string>())
        : store.forget_by_substring(args["text"].get<std::string>());

    // 零命中不是失败（引用一个已被删掉的 id 是常见情况），但措辞必须让模型看出
    // 什么都没被删。"forgot 0 record(s)" 会被读成成功 —— 端到端验证里模型据此
    // 向用户宣称删掉了，而磁盘上记录还在。同时指回 id：提示里每条都带 id，
    // 那是唯一可靠的寻址方式，子串必须逐字命中存储文本。
    if (removed == 0) {
        return ToolOutput{
            .text = "no memory record matched, so 0 records were removed and "
                    "nothing changed. Each record in the memory block is "
                    "prefixed with its id, like [a1b2c3d4]; pass that id for an "
                    "exact removal. A text match must be a literal substring of "
                    "the stored wording, not a description of it.",
        };
    }

    return ToolOutput{
        .text = "forgot " + std::to_string(removed) + " memory record(s)",
    };
}

}  // namespace

std::vector<RegisteredTool> make_memory_tools(
    std::shared_ptr<memory::MemoryStore> store
)
{
    std::vector<RegisteredTool> tools;
    tools.reserve(2);

    tools.push_back(RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "remember"},
            .description =
                "Store a durable fact about the user or this project so it is "
                "available in future turns. Keep each fact short and "
                "self-contained.",
            .input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"text", nlohmann::json{
                    {"type", "string"},
                    {"minLength", 1},
                    {"description", "The fact to remember."},
                }},
                {"scope", nlohmann::json{
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"user", "project"})},
                    {"description",
                     "\"project\" (default) keeps the fact to this workspace; "
                     "\"user\" loads it into every workspace."},
                }},
            }},
            {"required", nlohmann::json::array({"text"})},
            {"additionalProperties", false},
        },
        // 如实声明：这个工具写文件。write profile 下 policy 一律 Allow，所以
        // 演示路径不受影响；ask/minimal 下用户才能管住 agent 记了什么。
            .effects = {Effect::WriteFs},
            .replay = {},
        },
        .handler = [store](
            const nlohmann::json& args,
            const ToolExecutionContext&) {
            return execute_remember(*store, args);
        },
    });

    tools.push_back(RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "forget"},
            .description =
                "Remove stored memory records. Prefer the id shown in front of each "
                "record in the memory block; that is exact. Text matching is a "
                "literal substring search over the stored wording and removes every "
                "record it hits.",
            .input_schema = nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"id", nlohmann::json{
                    {"type", "string"},
                    {"description",
                     "The id shown in front of the record in the memory block, "
                     "e.g. \"a1b2c3d4\". Use this whenever the record is "
                     "visible to you."},
                }},
                {"text", nlohmann::json{
                    {"type", "string"},
                    {"minLength", 1},
                    {"description",
                     "A literal substring copied from the stored record text. "
                     "Not a description of what to forget: \"shell preference\" "
                     "will not match a record reading \"prefers fish shell\"."},
                }},
            }},
            {"additionalProperties", false},
        },
            .effects = {Effect::WriteFs},
            .replay = {},
        },
        .handler = [store](
            const nlohmann::json& args,
            const ToolExecutionContext&) {
            return execute_forget(*store, args);
        },
    });

    return tools;
}

}  // namespace my_agent::tool::detail
