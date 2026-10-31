#pragma once

#include "my_agent/runtime/model.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace my_agent {

// A self-contained view of the current tool activity. UI and REPL code use
// this value for display and permission events; they never resolve transcript
// calls or inspect Pending/binding themselves.
struct ToolActivityProjection {
    enum class Kind {
        None,
        AwaitingPermission,
        Executing,
        Invalid,
    };

    Kind kind{Kind::None};
    ToolCallRef call{};
    std::string tool_name;
    std::string arguments;
    std::string effects;
    std::string diagnostic;
};

[[nodiscard]] inline ToolActivityProjection tool_activity_projection(
    const Model& model)
{
    const AwaitingPermission* awaiting =
        std::get_if<AwaitingPermission>(&model.phase);
    const ExecutingTool* executing =
        std::get_if<ExecutingTool>(&model.phase);
    if (awaiting == nullptr && executing == nullptr) {
        return {};
    }

    const ToolCallRef ref = awaiting != nullptr
        ? awaiting->call : executing->call;
    const ToolCall *call = resolve_tool_call(model.domain.thread, ref);
    const auto* pending = call == nullptr
        ? nullptr : std::get_if<ToolCall::Pending>(&call->status);
    if (pending == nullptr || pending->binding.ref.value == 0) {
        return ToolActivityProjection{
            .kind = ToolActivityProjection::Kind::Invalid,
            .call = ref,
            .diagnostic = awaiting != nullptr
                ? "permission state invalid" : "tool execution state invalid",
        };
    }

    std::string effects;
    const auto append_effect = [&effects](std::string_view label) {
        if (!effects.empty()) {
            effects += ',';
        }
        effects += label;
    };
    if (pending->binding.effects.has(tool::Effect::ReadFs)) {
        append_effect("read_fs");
    }
    if (pending->binding.effects.has(tool::Effect::WriteFs)) {
        append_effect("write_fs");
    }
    if (pending->binding.effects.has(tool::Effect::Net)) {
        append_effect("net");
    }
    if (pending->binding.effects.has(tool::Effect::Exec)) {
        append_effect("exec");
    }
    if (effects.empty()) {
        effects = "none";
    }

    return ToolActivityProjection{
        .kind = awaiting != nullptr
            ? ToolActivityProjection::Kind::AwaitingPermission
            : ToolActivityProjection::Kind::Executing,
        .call = ref,
        .tool_name = call->name,
        .arguments = call->args.dump(),
        .effects = effects,
    };
}

}  // namespace my_agent
