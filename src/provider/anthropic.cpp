#include "my_agent/provider/anthropic.hpp"
#include "my_agent/provider/error.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/msg.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace my_agent::provider::anthropic {
namespace {

nlohmann::json encode_tools(const std::vector<ToolSpec>& tools)
{
    nlohmann::json encoded = nlohmann::json::array();
    for (const ToolSpec& spec : tools) {
        // 键叫 input_schema，不是 OpenAI 的 function.parameters，也没有外层
        // {"type":"function"} 包装。
        encoded.push_back(nlohmann::json{
            {"name", spec.name},
            {"description", spec.description},
            {"input_schema", spec.input_schema},
        });
    }
    return encoded;
}

// Anthropic 的 assistant 消息 content 是块数组：text 块与 tool_use 块并列。
// 工具结果不属于 assistant，而是紧跟的一条 user 消息里的 tool_result 块，
// 用 tool_use_id 关联（Ollama 用 tool_name）。
void append_message(nlohmann::json& messages, const Message& message)
{
    nlohmann::json content = nlohmann::json::array();

    if (!message.text.empty()) {
        content.push_back(nlohmann::json{
            {"type", "text"},
            {"text", message.text},
        });
    }

    for (const ToolCall& call : message.tool_calls) {
        content.push_back(nlohmann::json{
            {"type", "tool_use"},
            {"id", call.id},
            {"name", call.name},
            {"input", call.args},
        });
    }

    // 空 content 数组会被 API 拒掉，所以补一个空文本块占位。
    if (content.empty()) {
        content.push_back(nlohmann::json{{"type", "text"}, {"text", ""}});
    }

    messages.push_back(nlohmann::json{
        {"role", message.role == Role::User ? "user" : "assistant"},
        {"content", std::move(content)},
    });

    nlohmann::json results = nlohmann::json::array();
    for (const ToolCall& call : message.tool_calls) {
        // Pending 出现在出站历史里属逻辑错误：宿主只在所有工具终结后才发下一轮。
        // 跳过而不是编一个空结果，让上游 bug 保持可见。
        if (call.is_pending()) {
            continue;
        }

        results.push_back(nlohmann::json{
            {"type", "tool_result"},
            {"tool_use_id", call.id},
            {"content", call.output()},
        });
    }

    if (!results.empty()) {
        messages.push_back(nlohmann::json{
            {"role", "user"},
            {"content", std::move(results)},
        });
    }
}

}  // namespace

void StreamDecoder::feed(std::string_view chunk, const ProviderEventSink& sink)
{
    assembler_.feed(chunk, [this, &sink](const sse::Event& event) {
        handle_event(event, sink);
    });
}

void StreamDecoder::emit_usage(const ProviderEventSink& sink)
{
    if (usage_emitted_ || !input_usage_seen_ || !output_usage_seen_) {
        return;
    }

    const std::uint64_t cache_read = cache_read_input_tokens_.value_or(0);
    const std::uint64_t cache_creation =
        cache_creation_input_tokens_.value_or(0);
    std::optional<std::uint64_t> cached_input_tokens;
    if (cache_read_input_tokens_ || cache_creation_input_tokens_) {
        cached_input_tokens = cache_read + cache_creation;
    }

    sink(ProviderEvent{RequestUsage{
        .input_tokens = input_tokens_ + cache_read + cache_creation,
        .cached_input_tokens = cached_input_tokens,
        .output_tokens = output_tokens_,
        .cache_read_input_tokens = cache_read_input_tokens_,
        .cache_creation_input_tokens = cache_creation_input_tokens_,
        .quality = UsageQuality::Exact,
    }});
    usage_emitted_ = true;
}

void StreamDecoder::finish(const ProviderEventSink& sink)
{
    if (finished_) {
        return;
    }
    emit_usage(sink);
    finished_ = true;
    sink(ProviderEvent{StreamFinished{}});
}

bool StreamDecoder::terminal_emitted() const noexcept
{
    return finished_;
}

void StreamDecoder::handle_event(const sse::Event& event, const ProviderEventSink& sink)
{
    if (finished_) {
        return;
    }

    const nlohmann::json frame = nlohmann::json::parse(event.data, nullptr, false);
    if (frame.is_discarded() || !frame.is_object()) {
        return;
    }

    // event 名与 data.type 总是一致，但 OpenAI 那边没有 event 名，所以统一以
    // data 里的 type 为准 —— 让两个 provider 的解码器结构对称。
    const std::string type = frame.value("type", std::string{});

    if (type == "message_start") {
        const auto message = frame.find("message");
        if (message == frame.end() || !message->is_object()) {
            return;
        }
        const auto usage = message->find("usage");
        if (usage == message->end() || !usage->is_object()) {
            return;
        }

        const auto input = usage->find("input_tokens");
        if (input != usage->end() && input->is_number_unsigned()) {
            input_tokens_ = input->get<std::uint64_t>();
            input_usage_seen_ = true;
        }
        const auto cache_read = usage->find("cache_read_input_tokens");
        if (cache_read != usage->end() && cache_read->is_number_unsigned()) {
            cache_read_input_tokens_ = cache_read->get<std::uint64_t>();
        }
        const auto cache_creation = usage->find("cache_creation_input_tokens");
        if (cache_creation != usage->end()
            && cache_creation->is_number_unsigned()) {
            cache_creation_input_tokens_ =
                cache_creation->get<std::uint64_t>();
        }
        return;
    }

    if (type == "message_delta") {
        const nlohmann::json* usage = nullptr;
        const auto frame_usage = frame.find("usage");
        if (frame_usage != frame.end() && frame_usage->is_object()) {
            usage = &*frame_usage;
        } else {
            const auto delta = frame.find("delta");
            if (delta != frame.end() && delta->is_object()) {
                const auto delta_usage = delta->find("usage");
                if (delta_usage != delta->end() && delta_usage->is_object()) {
                    usage = &*delta_usage;
                }
            }
        }
        if (usage != nullptr) {
            const auto output = usage->find("output_tokens");
            if (output != usage->end() && output->is_number_unsigned()) {
                output_tokens_ = std::max(
                    output_tokens_, output->get<std::uint64_t>());
                output_usage_seen_ = true;
            }
        }
        return;
    }

    if (type == "content_block_start") {
        const auto block = frame.find("content_block");
        if (block == frame.end() || !block->is_object()) {
            return;
        }
        if (block->value("type", std::string{}) != "tool_use") {
            return;  // text 块不需要开场状态
        }

        in_tool_block_ = true;
        tool_id_ = block->value("id", std::string{});
        tool_name_ = block->value("name", std::string{});
        tool_args_json_.clear();
        return;
    }

    if (type == "content_block_delta") {
        const auto delta = frame.find("delta");
        if (delta == frame.end() || !delta->is_object()) {
            return;
        }

        const std::string delta_type = delta->value("type", std::string{});
        if (delta_type == "text_delta") {
            const std::string text = delta->value("text", std::string{});
            if (!text.empty()) {
                sink(ProviderEvent{StreamTextDelta{.text = text}});
            }
            return;
        }

        if (delta_type == "input_json_delta" && in_tool_block_) {
            tool_args_json_.append(delta->value("partial_json", std::string{}));
        }
        return;
    }

    if (type == "content_block_stop") {
        if (!in_tool_block_) {
            return;
        }

        // 参数收齐了，此刻才能拼出一个完整的 StreamToolCall。空参数工具的
        // partial_json 一片都不会来，所以空串要当成 {}。
        nlohmann::json args = tool_args_json_.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(tool_args_json_, nullptr, false);
        if (args.is_discarded() || !args.is_object()) {
            args = nlohmann::json::object();
        }

        sink(ProviderEvent{StreamToolCall{
            .id = tool_id_,
            .name = tool_name_,
            .args = std::move(args),
        }});

        in_tool_block_ = false;
        tool_id_.clear();
        tool_name_.clear();
        tool_args_json_.clear();
        return;
    }

    // 以下两个是终态事件。宿主要求每次 stream 调用恰好投出一个终态事件，
    // Anthropic 侧就靠它们满足 —— message_stop 是正常收尾，error 是流中途
    // 出错（超载、上下文超限），两者都必须让回合结束而不是悬着。
    if (type == "message_stop") {
        finish(sink);
        return;
    }

    if (type == "error") {
        const auto error = frame.find("error");
        std::string message{"anthropic stream error"};
        if (error != frame.end() && error->is_object()) {
            message = error->value("message", message);
        }
        const auto normalized = normalize_error(
            "anthropic", 0, frame.dump());
        finished_ = true;
        sink(ProviderEvent{StreamError{
            .message = normalized.message.empty()
                ? std::move(message)
                : normalized.message,
            .provider_error = normalized.metadata,
        }});
    }
}

StreamEffect make_stream(
    std::string host,
    std::string model,
    std::string api_key,
    std::shared_ptr<http::HttpClient> client
)
{
    return [
        host = std::move(host),
        model = std::move(model),
        api_key = std::move(api_key),
        client = std::move(client)
    ](Request request, ProviderEventSink sink) {
        const http::HttpRequest wire{
            .host = host,
            .port = 443,
            .path = "/v1/messages",
            .headers = {
                {"Content-Type", "application/json"},
                {"x-api-key", api_key},
                {"anthropic-version", "2023-06-01"},
            },
            .body = build_request_body(model, request),
            .use_tls = true,
        };

        // decoder 的 SSE 缓冲与工具参数累积都跨切片存活，所以必须活在回调之外。
        StreamDecoder decoder;

        const http::HttpResult result =
            client->post_stream(wire, [&decoder, &sink](std::string_view chunk) {
                decoder.feed(chunk, sink);
                return true;
            });

        // 成功路径上 message_stop 已经投过 StreamFinished，失败路径在这里补齐
        // 宿主要求的「恰好一个终态事件」。
        if (result) {
            decoder.finish(sink);
        } else if (!decoder.terminal_emitted()) {
            const auto normalized = normalize_error(
                "anthropic", result.error().status, result.error().message);
            sink(ProviderEvent{StreamError{
                .message = normalized.message,
                .provider_error = normalized.metadata,
            }});
        }
    };
}

std::string build_request_body(std::string_view model, const Request& request)
{
    nlohmann::json messages = nlohmann::json::array();
    for (const Message& message : request.messages) {
        append_message(messages, message);
    }

    nlohmann::json body{
        {"model", std::string{model}},
        {"stream", true},
        // max_tokens 是必填字段，且必须使用请求自己的 output cap。
        {"max_tokens", request.max_output_tokens},
        {"messages", std::move(messages)},
    };

    // system 是顶层字段，不是 messages 首元素 —— 与 Ollama / OpenAI 都不同。
    if (!request.system_prompt.empty()) {
        body["system"] = request.system_prompt;
    }

    if (!request.tools.empty()) {
        body["tools"] = encode_tools(request.tools);
    }

    return body.dump();
}

}  // namespace my_agent::provider::anthropic
