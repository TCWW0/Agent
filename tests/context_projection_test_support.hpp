#pragma once

#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/context_types.hpp"

namespace my_agent_test {

inline my_agent::ContextProjection project_context(
    std::string_view model, const my_agent::Request& request)
{
    const my_agent::ContextManagementConfig config;
    return my_agent::project_context({}, model, request, {},
        config.estimator_version, config.projection_policy_version,
        config.tool_replay_policy, config.effective_limit());
}

inline my_agent::ContextProjection project_context(
    std::string_view provider, std::string_view model,
    const my_agent::Request& request, const my_agent::ContextProjectionLedger& ledger,
    const my_agent::ToolReplayPolicy& policy, std::uint64_t context_limit)
{
    return my_agent::project_context(provider, model, request, ledger,
        my_agent::kGenericUnicodeV1Version, my_agent::kContextProjectionPolicyV1Version,
        policy, context_limit);
}

} // namespace my_agent_test
