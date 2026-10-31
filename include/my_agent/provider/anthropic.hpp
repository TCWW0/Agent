#pragma once

#include "my_agent/http/http_client.hpp"
#include "my_agent/provider/provider.hpp"
#include "my_agent/provider/sse.hpp"

#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace my_agent::provider::anthropic {

// Anthropic /v1/messages 的流式解码器。
//
// 与 Ollama 的本质差异（这是本切片要展示的理解）：工具参数是**逐字符流式**的，
// 不是原子到达。一次工具调用横跨三个事件：
//   content_block_start  → 拿到 id 与 name，此时 args 还完全是空的
//   content_block_delta  → input_json_delta.partial_json 一片片送来 JSON 文本
//   content_block_stop   → 参数收齐，此时才能拼成一个完整的 StreamToolCall
//
// 所以必须累积后延迟发出。我们的 StreamToolCall 契约是「一个完整调用一个事件」，
// 累积的复杂度收在传输层，Core 与宿主完全不需要知道 provider 是原子还是流式的。
class StreamDecoder {
public:
    void feed(std::string_view chunk, const ProviderEventSink& sink);
    void finish(const ProviderEventSink& sink);
    [[nodiscard]] bool terminal_emitted() const noexcept;

private:
    void handle_event(const sse::Event& event, const ProviderEventSink& sink);
    void emit_usage(const ProviderEventSink& sink);

    sse::FrameAssembler assembler_;

    // 当前打开的 tool_use 块。partial_json 累积在这里，直到 content_block_stop。
    bool in_tool_block_{false};
    std::string tool_id_;
    std::string tool_name_;
    std::string tool_args_json_;
    bool input_usage_seen_{false};
    std::uint64_t input_tokens_{0};
    std::optional<std::uint64_t> cache_read_input_tokens_;
    std::optional<std::uint64_t> cache_creation_input_tokens_;
    bool output_usage_seen_{false};
    std::uint64_t output_tokens_{0};
    bool usage_emitted_{false};
    bool finished_{false};
};

[[nodiscard]]
std::string build_request_body(std::string_view model, const Request& request);

[[nodiscard]]
StreamEffect make_stream(
    std::string host,
    std::string model,
    std::string api_key,
    std::shared_ptr<http::HttpClient> client
);

}  // namespace my_agent::provider::anthropic
