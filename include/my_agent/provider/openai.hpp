#pragma once

#include "my_agent/http/http_client.hpp"
#include "my_agent/provider/provider.hpp"
#include "my_agent/provider/sse.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace my_agent::provider::openai {

// OpenAI /v1/chat/completions 的流式解码器。
//
// 与 Anthropic 同为 SSE + 流式工具参数，但三处关键差异：
//   1. 没有 event 字段，只有 data；收尾是字面量 `data: [DONE]` 而非结构化事件。
//   2. 工具调用按 **index** 分组，多个工具可以交错到达 —— 不像 Anthropic 一次
//      只开一个 content_block。所以累积状态必须是 index → 累积器的映射。
//   3. function.arguments 是**序列化字符串**（Ollama 是 JSON object，Anthropic
//      是流式的 partial_json 文本）。累积完仍需再 parse 一次。
//
// 没有「块结束」事件可依赖，所以工具调用只能在 finish_reason 到达时统一发出。
class StreamDecoder {
public:
    void feed(std::string_view chunk, const ProviderEventSink& sink);
    void finish(const ProviderEventSink& sink);
    [[nodiscard]] bool terminal_emitted() const noexcept;

private:
    struct ToolAccumulator {
        std::string id;
        std::string name;
        std::string arguments;  // 序列化 JSON 文本，逐片累积
    };

    void handle_event(const sse::Event& event, const ProviderEventSink& sink);
    void emit_usage(const nlohmann::json& usage, const ProviderEventSink& sink);
    void flush_tool_calls(const ProviderEventSink& sink);

    sse::FrameAssembler assembler_;

    // map 而非 unordered_map：按 index 有序发出，工具调用顺序才是确定的。
    std::map<int, ToolAccumulator> tools_;
    bool finish_seen_{false};
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

}  // namespace my_agent::provider::openai
