#include "my_agent/runtime/request_preparation.hpp"

#include "my_agent/runtime/context_admission.hpp"

#include <utility>

namespace my_agent {

PreparedRequestSnapshot prepare_request_snapshot(Request request,
    std::vector<ToolSpec> tools,
    std::optional<std::string> system_prompt,
    const RequestPolicy& request_policy,
    const ContextProjectionLedger& ledger)
{
    const auto& policy = request_policy.config();
    request.tools = std::move(tools);
    if (system_prompt) {
        request.system_prompt = std::move(*system_prompt);
    }
    request.max_output_tokens = policy.normal_output_budget();

    ContextProjection projection = project_context(request_policy.provider_name(),
        request_policy.provider_model(),
        request,
        ledger,
        policy.estimator_version,
        policy.projection_policy_version,
        policy.tool_replay_policy,
        policy.effective_limit());
    ContextBudgetAssessment budget =
        assess_context_admission(projection, policy, request.max_output_tokens);
    ContextProjectionReceipt receipt{
        .provider = projection.provider,
        .model = projection.model,
        .fingerprint = projection.fingerprint,
        .estimator_version = projection.estimator_version,
        .policy_version = projection.policy_version,
        .segments = projection.segments,
        .realistic_input_tokens = projection.realistic_input_tokens,
    };
    return PreparedRequestSnapshot{
        .request = std::move(projection.request),
        .budget = std::move(budget),
        .receipt = std::move(receipt),
    };
}

} // namespace my_agent
