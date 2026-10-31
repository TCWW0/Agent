#include "my_agent/provider/openai.hpp"
#include "my_agent/provider/error.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/msg.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace my_agent::provider::openai {
namespace {

nlohmann::json encode_tools(const std::vector<ToolSpec>& tools)
{
    nlohmann::json encoded = nlohmann::json::array();
    for (const ToolSpec& spec : tools) {
        encoded.push_back(nlohmann::json{
            {"type", "function"},
            {"function", nlohmann::json{
                {"name", spec.name},
                {"description", spec.description},
                {"parameters", spec.input_schema},
            }},
        });
    }
    return encoded;
}

// 工具结果是一条独立的 role=="tool" 消息，用 tool_call_id 关联（Ollama 用
// tool_name，Anthropic 用 user 消息里的 tool_result 块）。
void append_message(nlohmann::json& messages, const Message& message)
{
    nlohmann::json encoded{
        {"role", message.role == Role::User ? "user" : "assistant"},
        {"content", message.text},
    };

    if (!message.tool_calls.empty()) {
        nlohmann::json calls = nlohmann::json::array();
        for (const ToolCall& call : message.tool_calls) {
            calls.push_back(nlohmann::json{
                {"id", call.id},
                {"type", "function"},
                {"function", nlohmann::json{
                    {"name", call.name},
                    // arguments 是**序列化字符串**，不是 object —— 与 Ollama
                    // 最容易混淆的一处差异。
                    {"arguments", call.args.dump()},
                }},
            });
        }
        encoded["tool_calls"] = std::move(calls);
    }

    messages.push_back(std::move(encoded));

    for (const ToolCall& call : message.tool_calls) {
        // Pending 出现在出站历史里属逻辑错误：跳过而不是编空结果，让 bug 可见。
        if (call.is_pending()) {
            continue;
        }

        messages.push_back(nlohmann::json{
            {"role", "tool"},
            {"tool_call_id", call.id},
            {"content", call.output()},
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

void StreamDecoder::finish(const ProviderEventSink& sink)
{
    if (finished_) {
        return;
    }
    flush_tool_calls(sink);
    finished_ = true;
    sink(ProviderEvent{StreamFinished{}});
}

bool StreamDecoder::terminal_emitted() const noexcept
{
    return finished_;
}

void StreamDecoder::emit_usage(
    const nlohmann::json& usage,
    const ProviderEventSink& sink)
{
    if (usage_emitted_ || !usage.is_object()) {
        return;
    }

    const auto prompt_tokens = usage.find("prompt_tokens");
    const auto completion_tokens = usage.find("completion_tokens");
    if (prompt_tokens == usage.end()
        || !prompt_tokens->is_number_unsigned()
        || completion_tokens == usage.end()
        || !completion_tokens->is_number_unsigned()) {
        return;
    }

    std::optional<std::uint64_t> cached_input_tokens;
    const auto details = usage.find("prompt_tokens_details");
    if (details != usage.end() && details->is_object()) {
        const auto cached = details->find("cached_tokens");
        if (cached != details->end() && cached->is_number_unsigned()) {
            cached_input_tokens = cached->get<std::uint64_t>();
        }
    }

    sink(ProviderEvent{RequestUsage{
        .input_tokens = prompt_tokens->get<std::uint64_t>(),
        .cached_input_tokens = cached_input_tokens,
        .output_tokens = completion_tokens->get<std::uint64_t>(),
        .cache_read_input_tokens = cached_input_tokens,
        .quality = UsageQuality::Exact,
    }});
    usage_emitted_ = true;
}

void StreamDecoder::flush_tool_calls(const ProviderEventSink& sink)
{
    // map 保证按 index 升序发出，工具调用顺序才是确定的。
    for (auto& [index, accumulator] : tools_) {
        nlohmann::json args = accumulator.arguments.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(accumulator.arguments, nullptr, false);
        if (args.is_discarded() || !args.is_object()) {
            args = nlohmann::json::object();
        }

        sink(ProviderEvent{StreamToolCall{
            .id = accumulator.id,
            .name = accumulator.name,
            .args = std::move(args),
        }});
    }
    tools_.clear();
}

void StreamDecoder::handle_event(const sse::Event& event, const ProviderEventSink& sink)
{
    if (finished_) {
        return;
    }

    // 收尾是字面量 sentinel，不是结构化事件 —— OpenAI SSE 的一个怪癖。
    if (event.data == "[DONE]") {
        finish(sink);
        return;
    }

    const nlohmann::json frame = nlohmann::json::parse(event.data, nullptr, false);
    if (frame.is_discarded() || !frame.is_object()) {
        return;
    }

    const auto error = frame.find("error");
    if (error != frame.end() && error->is_object()) {
        if (!finished_) {
            finished_ = true;
            const auto normalized = normalize_error(
                "openai", 0, frame.dump());
            sink(ProviderEvent{StreamError{
                .message = normalized.message.empty()
                    ? error->value(
                        "message", std::string{"openai stream error"})
                    : normalized.message,
                .provider_error = normalized.metadata,
            }});
        }
        return;
    }

    const auto usage = frame.find("usage");
    if (usage != frame.end()) {
        emit_usage(*usage, sink);
        if (finish_seen_ && usage_emitted_) {
            finish(sink);
            return;
        }
    }

    const auto choices = frame.find("choices");
    if (choices == frame.end() || !choices->is_array() || choices->empty()) {
        return;
    }

    const nlohmann::json& choice = choices->front();

    const auto delta = choice.find("delta");
    if (delta != choice.end() && delta->is_object()) {
        // content 在角色声明帧里是空串，投出空 delta 只会在 Model 里留噪声。
        const std::string content = delta->value("content", std::string{});
        if (!content.empty()) {
            sink(ProviderEvent{StreamTextDelta{.text = content}});
        }

        const auto tool_calls = delta->find("tool_calls");
        if (tool_calls != delta->end() && tool_calls->is_array()) {
            for (const nlohmann::json& call : *tool_calls) {
                // index 是分组键：多个工具可以交错到达，没有「块结束」事件可依赖。
                ToolAccumulator& accumulator = tools_[call.value("index", 0)];

                // id 与 name 只在首帧出现，后续帧只带 arguments 碎片。
                const std::string id = call.value("id", std::string{});
                if (!id.empty()) {
                    accumulator.id = id;
                }

                const auto function = call.find("function");
                if (function == call.end() || !function->is_object()) {
                    continue;
                }

                const std::string name = function->value("name", std::string{});
                if (!name.empty()) {
                    accumulator.name = name;
                }

                accumulator.arguments.append(
                    function->value("arguments", std::string{})
                );
            }
        }
    }

    // finish_reason 是唯一的「本轮内容到此为止」信号，累积的工具调用在此发出。
    const auto finish_reason = choice.find("finish_reason");
    if (finish_reason != choice.end() && finish_reason->is_string() && !finished_) {
        flush_tool_calls(sink);
        finish_seen_ = true;
        if (usage_emitted_) {
            finish(sink);
        }
    }
}

std::string build_request_body(std::string_view model, const Request& request)
{
    nlohmann::json messages = nlohmann::json::array();

    // system 是 messages 数组的首元素，不是顶层字段 —— 与 Anthropic 相反。
    if (!request.system_prompt.empty()) {
        messages.push_back(nlohmann::json{
            {"role", "system"},
            {"content", request.system_prompt},
        });
    }

    for (const Message& message : request.messages) {
        append_message(messages, message);
    }

    nlohmann::json body{
        {"model", std::string{model}},
        {"stream", true},
        {"stream_options", nlohmann::json{{"include_usage", true}}},
        {"messages", std::move(messages)},
    };

    if (!request.tools.empty()) {
        body["tools"] = encode_tools(request.tools);
    }
    // This adapter targets Chat Completions, where max_tokens is the
    // portable output-limit field (Responses uses a different name).
    body["max_tokens"] = request.max_output_tokens;

    return body.dump();
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
            .path = "/v1/chat/completions",
            .headers = {
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + api_key},
            },
            .body = build_request_body(model, request),
            .use_tls = true,
        };

        // decoder 的 SSE 缓冲与按 index 的参数累积都跨切片存活。
        StreamDecoder decoder;

        const http::HttpResult result =
            client->post_stream(wire, [&decoder, &sink](std::string_view chunk) {
                decoder.feed(chunk, sink);
                return true;
            });

        if (result) {
            decoder.finish(sink);
        } else if (!decoder.terminal_emitted()) {
            const auto normalized = normalize_error(
                "openai", result.error().status, result.error().message);
            sink(ProviderEvent{StreamError{
                .message = normalized.message,
                .provider_error = normalized.metadata,
            }});
        }
    };
}

}  // namespace my_agent::provider::openai
