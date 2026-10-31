#pragma once

#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/context_types.hpp"

#include <cstdint>

namespace my_agent {

[[nodiscard]]
ContextBudgetAssessment assess_context_admission(
    const ContextProjection& projection,
    const ContextManagementConfig& config,
    std::uint64_t request_output_budget);

}  // namespace my_agent
