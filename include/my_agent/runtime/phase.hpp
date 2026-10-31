#pragma once

#include "my_agent/runtime/compaction.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/context_types.hpp"
#include "my_agent/runtime/identity.hpp"

#include <optional>
#include <string>
#include <variant>

namespace my_agent {

    // 明确表示一个Thread的闲置状态
    struct Idle{
    };

    struct QueuedTurnRequest { TurnId turn_id; MessageId queued_message_id; };
    struct ToolContinuationRequest { TurnId turn_id; MessageId previous_assistant_id; };
    struct RetryRequest { TurnId turn_id; MessageId target_assistant_id; };
    using RequestIntent = std::variant<QueuedTurnRequest, ToolContinuationRequest, RetryRequest>;

    struct PreparingRequest {
        PreparationId preparation_id{};
        RequestIntent intent{};
    };

    struct Streaming {
        StreamRef ref{};
        MessageId target_message_id{};
        ContextProjectionReceipt projection_receipt;
        bool usage_accepted{false};
        std::optional<MessageId> rollback_user_message{};
    };

    struct Compacting {
        CompactionPlan plan;
        std::string pending_summary;
        bool usage_accepted{false};
    };

    struct AwaitingPermission {
        ToolCallRef call{};
    };

    struct ExecutingTool {
        ToolCallRef call{};
    };

    struct Blocked {
        BlockReason reason{BlockReason::ExplicitResumeRequired};
        std::optional<ContextBlockDetail> context_detail{};
        std::optional<ContextBudgetAssessment> budget{};
        std::string message;
    };

    using Phase = std::variant<
        Idle,
        PreparingRequest,
        Compacting,
        Streaming,
        AwaitingPermission,
        ExecutingTool,
        Blocked>;

} // namespace my_agent
