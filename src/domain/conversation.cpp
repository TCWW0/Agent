#include "my_agent/domain/conversation.hpp"

#include <algorithm>
#include <string>
#include <variant>

namespace my_agent{
    namespace {
        template <typename ThreadLike>
        auto resolve_tool_call_impl(ThreadLike& thread, ToolCallRef ref)
        {
            using CallPointer = decltype(&thread.messages.front().tool_calls.front());
            if (ref.assistant_message_id.value == 0 || ref.tool_exec_id.value == 0) {
                return CallPointer{};
            }
            const auto message = std::find_if(
                thread.messages.begin(), thread.messages.end(),
                [ref](const Message& candidate) {
                    return candidate.id == ref.assistant_message_id
                        && candidate.role == Role::Assistant;
                });
            if (message == thread.messages.end()) {
                return CallPointer{};
            }

            CallPointer match = nullptr;
            for (auto& call : message->tool_calls) {
                if (call.tool_exec_id != ref.tool_exec_id) {
                    continue;
                }
                if (match != nullptr) {
                    return CallPointer{};
                }
                match = &call;
            }
            return match;
        }
    }

    ToolCall* resolve_tool_call(Thread& thread, ToolCallRef ref)
    {
        return resolve_tool_call_impl(thread, ref);
    }

    const ToolCall* resolve_tool_call(const Thread& thread, ToolCallRef ref)
    {
        return resolve_tool_call_impl(thread, ref);
    }

    bool ToolCall::is_pending() const noexcept
    {
        return std::holds_alternative<Pending>(status);
    }

    const std::string& ToolCall::output() const noexcept
    {
        static const std::string empty;
        if (const auto* done = std::get_if<Done>(&status)) {
            return done->output;
        }
        if (const auto* failed = std::get_if<Failed>(&status)) {
            return failed->output;
        }
        if (const auto* rejected = std::get_if<Rejected>(&status)){
            return rejected->output;
        }
        if (const auto* cancelled = std::get_if<Cancelled>(&status)) {
            return cancelled->output;
        }
        return empty;
    }
}// namespace my_agent
