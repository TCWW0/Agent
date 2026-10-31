#pragma once

#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/context_types.hpp"

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace my_agent {

struct PreparedRequestSnapshot {
    Request request;
    ContextBudgetAssessment budget;
    ContextProjectionReceipt receipt;
};

[[nodiscard]]
PreparedRequestSnapshot prepare_request_snapshot(
    Request request,
    std::vector<ToolSpec> tools,
    std::optional<std::string> system_prompt,
    const RequestPolicy& request_policy,
    const ContextProjectionLedger& ledger);

}  // namespace my_agent
