#pragma once

#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/tool_replay.hpp"
#include "my_agent/tool/effects.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "nlohmann/json_fwd.hpp"
#include <expected>
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>

namespace my_agent::tool {

struct ToolDescriptor{
    ToolName name;
    std::string description;
    nlohmann::json input_schema;
    EffectSet effects{};
    ToolReplayRule replay{};

};

using ToolHandler = std::function<ExecResult(
    const nlohmann::json& args,
    const ToolExecutionContext& context)>;

struct RegisteredTool {
    ToolDescriptor descriptor;
    ToolHandler handler;
};

class ToolRuntime final {
public:
    explicit ToolRuntime(std::vector<RegisteredTool> registrations);

    ToolRuntime(const ToolRuntime&) = delete;
    ToolRuntime& operator=(const ToolRuntime&) = delete;

    [[nodiscard]]
    std::vector<ToolSpec> request_tools() const;

    [[nodiscard]]
    std::expected<ToolBinding, ToolResolutionError> resolve(std::string_view name) const;

    [[nodiscard]]
    ExecResult execute(ToolRef ref, const nlohmann::json& args, const ToolExecutionContext& context = {}) const;

private:
    std::vector<RegisteredTool> registrations_;
};

} // namespace my_agent::tool