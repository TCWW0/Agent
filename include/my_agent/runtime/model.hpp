#pragma once

#include "my_agent/domain/conversation.hpp"
#include "my_agent/domain/profile.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/runtime/identity.hpp"
#include "my_agent/runtime/phase.hpp"
#include "my_agent/runtime/usage.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace my_agent{
    struct IdSequence {
        std::uint64_t next_turn{1};
        std::uint64_t next_message{1};
        std::uint64_t next_stream{1};
        std::uint64_t next_preparation{1};
        std::uint64_t next_compaction{1};
        std::uint64_t next_tool_exec{1};
    };

    struct QueuedMessage {
        TurnId turn_id{};
        MessageId message_id{};
        std::string text;
    };

    // 当前子回合的增量输出统计。它只记录流式 delta 的原始字节数与变更代数；
    // 时间、速率和历史采样属于 UI telemetry 层，不让 Core 偷渡时钟依赖。
    struct StreamTelemetry {
        std::size_t output_bytes{0};
        std::uint64_t generation{0};
    };

    struct FailedCompactionAttempt {
        std::optional<RequestFingerprint> source_digest{};
        ContextBlockDetail detail{ContextBlockDetail::CompactionFailed};
    };

    struct Domain {
        Thread thread{};
        Profile profile{Profile::Write};
        RequestPolicy request_policy{};
    };

    struct SessionState {
        StreamTelemetry stream_telemetry{};
        UsageState usage{};
        ContextProjectionLedger projection_ledger{};
        std::optional<FailedCompactionAttempt> last_failed_compaction{};
        std::deque<QueuedMessage> queued_messages;
        IdSequence ids{};
    };

    // 这是对于系统核心的瞬时建模，应该包含的是当前系统的状态信息
    struct Model{
        Phase phase{Idle{}};
        Domain domain{};
        SessionState session{};
    };
}// namespace my_agent
