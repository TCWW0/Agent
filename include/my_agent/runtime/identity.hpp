#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace my_agent {

// Domain identities are allocated once per linear session. Zero is invalid;
// only raw ProviderEvents may remain unbound until they cross the Host boundary.
struct TurnId {
    std::uint64_t value{0};

    friend bool operator==(const TurnId&, const TurnId&) = default;
};

struct MessageId {
    std::uint64_t value{0};

    friend bool operator==(const MessageId&, const MessageId&) = default;
};

struct StreamId {
    std::uint64_t value{0};

    friend bool operator==(const StreamId&, const StreamId&) = default;
};

struct PreparationId {
    std::uint64_t value{0};

    friend bool operator==(const PreparationId&, const PreparationId&) = default;
};

struct ToolExecId {
    std::uint64_t value{0};

    friend bool operator==(const ToolExecId&, const ToolExecId&) = default;
};

// Stable transcript identity for one accepted assistant tool call. The
// MessageId states ownership; the ToolExecId distinguishes calls within the
// session lineage. Provider call IDs remain wire/transcript data only.
struct ToolCallRef {
    MessageId assistant_message_id{};
    ToolExecId tool_exec_id{};

    friend bool operator==(const ToolCallRef&, const ToolCallRef&) = default;
};

struct CompactionId {
    std::uint64_t value{0};

    friend bool operator==(const CompactionId&, const CompactionId&) = default;
};

struct StreamRef {
    TurnId turn_id{};
    StreamId stream_id{};

    friend bool operator==(const StreamRef&, const StreamRef&) = default;
};

using OperationRef = std::variant<StreamRef, ToolCallRef, CompactionId>;

[[nodiscard]] inline bool is_valid(StreamRef ref) noexcept
{
    return ref.turn_id.value != 0 && ref.stream_id.value != 0;
}

[[nodiscard]] inline bool is_valid(ToolCallRef ref) noexcept
{
    return ref.assistant_message_id.value != 0 && ref.tool_exec_id.value != 0;
}

[[nodiscard]] inline bool is_valid(CompactionId ref) noexcept
{
    return ref.value != 0;
}

struct RequestFingerprint {
    std::string value;

    friend bool operator==(
        const RequestFingerprint&, const RequestFingerprint&) = default;
};

enum class UsageQuality : std::uint8_t {
    Exact,
    Estimated,
    Last,
    Stale,
    Unavailable,
};

enum class FailureDisposition : std::uint8_t {
    RetryCurrent,
    FailAndContinue,
    BlockSession,
};

enum class BlockReason : std::uint8_t {
    ProviderUnavailable,
    Authentication,
    InvalidConfiguration,
    NoModel,
    ContextLimit,
    ExplicitResumeRequired,
};

enum class ProviderErrorKind : std::uint8_t {
    Unknown,
    ContextLimit,
};

struct ProviderError {
    ProviderErrorKind kind{ProviderErrorKind::Unknown};
    int http_status{0};
    std::string code;
    std::string type;
};

}  // namespace my_agent
