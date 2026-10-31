#pragma once

#include "my_agent/runtime/generic_unicode_estimator.hpp"
#include "my_agent/runtime/tool_replay.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace my_agent {

enum class ContextBlockDetail : std::uint8_t {
    RequestTooLarge,
    NoCompleteHistory,
    CompactionSourceTooLarge,
    CompactionFailed,
    CompactionInsufficient,
};

// The single source of truth for request admission and provider output caps.
// 用于记录本次配置的模型上下文，并且依据对应的上下文配置来计算输出预算/上下文上限
struct ContextManagementConfig {
    std::uint64_t configured_limit{8192};
    std::optional<std::uint64_t> provider_limit{};
    std::uint64_t safety_margin{128};

    double normal_output_ratio{0.25};
    std::uint64_t normal_output_ceiling{16384};

    double compaction_output_ratio{0.125};
    std::uint64_t compaction_output_ceiling{4096};
    double compaction_trigger_ratio{0.80};

    ToolReplayPolicy tool_replay_policy{};
    std::string estimator_version{kGenericUnicodeV1Version};
    std::string projection_policy_version{"context-projection-v1"};

    [[nodiscard]] std::uint64_t effective_limit() const noexcept;

    [[nodiscard]] std::uint64_t normal_output_budget() const;

    [[nodiscard]] std::uint64_t compaction_output_budget() const;

    void validate() const;
};

// A request configuration is validated before it can enter a Model or Msg.
// Algorithms may still work directly with ContextManagementConfig values.
class RequestPolicy {
  public:
    explicit RequestPolicy(ContextManagementConfig config = {},
        std::string provider_name = {},
        std::string provider_model = {});

    [[nodiscard]] const ContextManagementConfig& config() const noexcept
    {
        return config_;
    }
    [[nodiscard]] const std::string& provider_name() const noexcept
    {
        return provider_name_;
    }
    [[nodiscard]] const std::string& provider_model() const noexcept
    {
        return provider_model_;
    }

  private:
    ContextManagementConfig config_;
    std::string provider_name_;
    std::string provider_model_;
};

struct ContextBudgetAssessment {
    std::uint64_t projected_input_tokens{0};
    std::uint64_t hard_input_limit{0};
    bool estimated{true};
    bool triggered{false};
    bool admitted{false};
    // Optional diagnostics for the UI's environment-gated context debug line.
    std::uint64_t incremental_input_tokens{0};
    bool exact_anchor_reused{false};
};

} // namespace my_agent
