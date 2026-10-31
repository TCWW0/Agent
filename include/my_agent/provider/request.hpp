#pragma once

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/tool_replay.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace my_agent {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    ToolReplayMode replay_mode{ToolReplayMode::HeadTail};
    std::uint32_t replay_weight{1};
};

struct Request {
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;

    // 由宿主在 effect 侧填充，不是 update() 填的：构建系统提示要读文件
    // （memory / skills），而 update() 必须保持纯。缺省为空。
    std::string system_prompt;

    // Every request carries an explicit provider output cap. Host setup
    // replaces the default with ContextManagementConfig::normal_output_budget
    // or compaction_output_budget before the request reaches transport.
    std::uint64_t max_output_tokens{2048};
};

// A deterministic identity for the complete request preparation input. The
// model is included because changing the model invalidates any exact usage
// anchor even when the transcript itself is unchanged.
[[nodiscard]]
inline RequestFingerprint fingerprint_request(
    std::string_view model,
    const Request& request)
{
    std::string canonical;
    const auto append = [&canonical](std::string_view value) {
        canonical += std::to_string(value.size());
        canonical += ':';
        canonical += value;
        canonical += '|';
    };

    append(model);
    append(request.system_prompt);
    append(std::to_string(request.max_output_tokens));
    for (const Message& message : request.messages) {
        append(message.role == Role::User ? "user" : "assistant");
        append(message.text);
        for (const ToolCall& call : message.tool_calls) {
            append(call.id);
            append(call.name);
            append(call.args.dump());
            append(call.output());
        }
    }
    for (const ToolSpec& tool : request.tools) {
        append(tool.name);
        append(tool.description);
        append(tool.input_schema.dump());
    }

    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }

    std::string digest;
    constexpr char hex[] = "0123456789abcdef";
    digest.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        digest += hex[(hash >> shift) & 0xfU];
    }
    return RequestFingerprint{.value = std::move(digest)};
}

[[nodiscard]]
inline RequestFingerprint fingerprint_request(const Request& request)
{
    return fingerprint_request({}, request);
}

}  // namespace my_agent
