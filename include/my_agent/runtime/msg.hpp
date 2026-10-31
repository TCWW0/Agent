#pragma once

#include "my_agent/domain/profile.hpp"
#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/compaction.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/context_types.hpp"
#include "my_agent/runtime/identity.hpp"
#include "my_agent/runtime/request_preparation.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "nlohmann/json_fwd.hpp"

#include <optional>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <variant>
#include <utility>

#include <nlohmann/json.hpp>

namespace my_agent{
    /* 表示一个明确已经发生的事情,eg:用户提交了这段文本，是对于外部事件的一个封装
     * 后续可以继续进行扩展
    */
    struct Submit{
        std::string text;
    };

    struct RequestCompaction{
    };

    struct StreamTextDelta{
        StreamRef ref{};
        std::string text;
    };

    struct StreamToolCall{
        StreamRef ref{};
        std::string id;
        std::string name;
        nlohmann::json args;
    };

    using ToolCallResolution = std::variant<tool::ToolBinding,tool::ToolResolutionError>;

    struct ResolvedStreamToolCall {
        ResolvedStreamToolCall(StreamRef stream, std::string call_id,
            std::string tool_name, nlohmann::json arguments, ToolCallResolution resolved)
            : ref(stream), id(std::move(call_id)), name(std::move(tool_name)),
              args(std::move(arguments)), resolution(std::move(resolved))
        {
            if (const auto* binding = std::get_if<tool::ToolBinding>(&resolution);
                binding && binding->ref.value == 0) {
                throw std::invalid_argument{"resolved tool call requires a non-zero binding"};
            }
        }

        StreamRef ref{};
        std::string id;
        std::string name;
        nlohmann::json args;
        ToolCallResolution resolution;
    };

    struct StreamError{
        StreamRef ref{};
        std::string message;
        FailureDisposition disposition{FailureDisposition::FailAndContinue};
        BlockReason block_reason{BlockReason::ProviderUnavailable};
        std::optional<ProviderError> provider_error{};
    };

    struct ToolExecOutput{
        ToolCallRef call{};
        tool::ToolRef ref{};
        tool::ExecResult result;
    };

    struct StreamFinished{
        StreamRef ref{};
    };

    struct RequestUsage {
        OperationRef operation{};
        RequestFingerprint request_fingerprint{};
        std::uint64_t input_tokens{0};
        std::optional<std::uint64_t> cached_input_tokens{};
        std::uint64_t output_tokens{0};
        std::optional<std::uint64_t> cache_read_input_tokens{};
        std::optional<std::uint64_t> cache_creation_input_tokens{};
        UsageQuality quality{UsageQuality::Exact};
    };

    struct RequestPrepared {
        PreparationId preparation_id{};
        PreparedRequestSnapshot snapshot;
    };

    struct RequestPreparationFailed {
        PreparationId preparation_id{};
        std::string message;
    };

    struct CompactionDelta {
        CompactionId compaction_id{};
        std::string text;
    };

    struct CompactionFinished {
        CompactionId compaction_id{};
    };

    struct CompactionError {
        CompactionId compaction_id{};
        std::string message;
    };

    struct CancelActive{
    };

    struct ResumeQueue{
    };

    // Drop only prompts that have not been admitted into the transcript.
    // Active turns and committed conversation history are never affected.
    struct ClearQueue{
    };

    struct SetProfile{
        Profile profile;
    };

    struct SetRequestPolicy {
        RequestPolicy policy;
    };

    struct PermissionApprove {
        ToolCallRef call{};
    };

    struct PermissionReject{
        ToolCallRef call{};
        std::optional<std::string> feedback{};
    };

    using Msg = std::variant<Submit,RequestCompaction,StreamTextDelta,ResolvedStreamToolCall,
        StreamFinished,RequestUsage,RequestPrepared,RequestPreparationFailed,
        CompactionDelta,CompactionFinished,CompactionError,StreamError,
        ToolExecOutput,SetProfile,SetRequestPolicy,
        PermissionApprove,PermissionReject,
        CancelActive,ResumeQueue,ClearQueue>;
}// namespace my_agent
