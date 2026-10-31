#include "my_agent/tool/tool_runtime.hpp"
#include "my_agent/provider/request.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::tool {

namespace {

std::string unknown_tool_message(std::string_view name, const std::vector<RegisteredTool>& registrations){
    std::string message = "unknown tool: ";
    message += name;
    message += "; available tools: ";
    for (std::size_t index = 0; index < registrations.size(); ++index) {
        if (index != 0) {
            message += ", ";
        }
        message += registrations[index].descriptor.name.value;
    }
    return message;
}

} // namespace

ToolRuntime::ToolRuntime(std::vector<RegisteredTool> registrations)
    : registrations_{std::move(registrations)}
{

}

std::vector<ToolSpec> ToolRuntime::request_tools() const
{
    std::vector<ToolSpec> tools;
    tools.reserve(registrations_.size());
    for (const RegisteredTool& registration: registrations_) {
        const ToolDescriptor& des = registration.descriptor;
        tools.push_back(ToolSpec{
            .name = des.name.value,
            .description = des.description,
            .input_schema = des.input_schema,
            .replay_mode = des.replay.mode,
            .replay_weight = des.replay.weight,
        });
    }
    return tools;
}

std::expected<ToolBinding, ToolResolutionError>
ToolRuntime::resolve(std::string_view name) const
{
    for (std::size_t index = 0; index < registrations_.size(); ++index) {
        const ToolDescriptor& des = registrations_[index].descriptor;
        if (des.name.value != name) {
            continue;
        }
        return ToolBinding{
            .ref = ToolRef{.value = index + 1},
            .effects = des.effects,
        };
    }

    return std::unexpected(ToolResolutionError{
        .name = std::string{name},
        .message = unknown_tool_message(name, registrations_),
    });
}

ExecResult ToolRuntime::execute(ToolRef ref, const nlohmann::json& args, const ToolExecutionContext& context) const
{
    if (ref.value == 0 || ref.value > registrations_.size()) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::ExecutionFailed,
            .message = std::format("invalid tool reference: {}",ref.value),
        });
    }

    const ToolHandler& handler = registrations_[ref.value - 1].handler;
    try {
        return handler(args, context);
    } catch (const std::exception& error) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::ExecutionFailed,
            .message = error.what(),
        });
    } catch (...) {
        return std::unexpected(ToolError{
            .kind = ErrorKind::ExecutionFailed,
            .message = "unknown tool failure",
        });
    }
}

} // namespace my_agent::tool
