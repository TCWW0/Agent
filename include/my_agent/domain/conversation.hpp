#pragma once

#include "my_agent/runtime/identity.hpp"
#include "my_agent/tool/tool_protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace my_agent{
    enum class Role{
        User,
        Assistant,
    };

    struct ToolCall{
        struct Pending{
            tool::ToolBinding binding{};
        };
        struct Done{
            std::string output;
        };
        struct Failed{
            std::string output;
        };
        struct Rejected{
            std::string output;
        };
        struct Cancelled{
            std::string output;
        };

        using Status = std::variant<Pending,Done,Failed,Rejected,Cancelled>;

        std::string id;
        std::string name;
        nlohmann::json args;
        Status status{Pending{}};
        ToolExecId tool_exec_id{};

        [[nodiscard]]
        bool is_pending() const noexcept;

        [[nodiscard]]
        const std::string& output() const noexcept;
    };

    // 代表对话记录中的一条消息：谁说的+说了什么
    struct Message{
        Role role;
        std::string text;
        std::optional<std::string> error;
        std::vector<ToolCall> tool_calls;
        MessageId id{};
        TurnId turn_id{};
    };

    struct ContinuationSummary {
        nlohmann::json value;
    };

    struct AcceptedCompaction {
        CompactionId compaction_id{};
        MessageId covered_through{};
        ContinuationSummary summary;
        RequestFingerprint source_digest;
        std::uint64_t before_tokens{0};
        std::uint64_t after_tokens{0};
    };

    // 一个会话线程，由一段完整的有顺序的会话历史所组成
    struct Thread{
        std::vector<Message> messages;
        std::optional<AcceptedCompaction> accepted_compaction{};
    };

    [[nodiscard]] ToolCall* resolve_tool_call(Thread& thread, ToolCallRef ref);
    [[nodiscard]] const ToolCall* resolve_tool_call(
        const Thread& thread, ToolCallRef ref);
}// namespace my_agent
