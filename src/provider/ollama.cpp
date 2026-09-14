#include "my_agent/provider/ollama.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/msg.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace my_agent::provider::ollama {
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

nlohmann::json encode_tool_calls(const std::vector<ToolCall>& calls)
{
    nlohmann::json encoded = nlohmann::json::array();
    for (const ToolCall& call : calls) {
        encoded.push_back(nlohmann::json{
            {"id", call.id},
            {"function", nlohmann::json{
                {"name", call.name},
                {"arguments", call.args},
            }},
        });
    }
    return encoded;
}

// 我们的 Role 只有 User / Assistant，工具结果存在 assistant message 的
// tool_calls[].status 里。出站时必须展开成 Ollama 认识的形状：assistant 消息
// 本身，紧跟每个已终结 tool_call 的一条 role=="tool" 消息。
//
// 注意 Ollama 用 tool_name 关联结果，不是 tool_call_id。
void append_message(nlohmann::json& messages, const Message& message)
{
    nlohmann::json encoded{
        {"role", message.role == Role::User ? "user" : "assistant"},
        {"content", message.text},
    };

    if (!message.tool_calls.empty()) {
        encoded["tool_calls"] = encode_tool_calls(message.tool_calls);
    }

    messages.push_back(std::move(encoded));

    for (const ToolCall& call : message.tool_calls) {
        // Pending 出现在出站历史里属于逻辑错误：宿主只会在所有工具都终结之后
        // 才发起下一轮请求。跳过它而不是编一个空结果，让上游的 bug 保持可见。
        if (call.is_pending()) {
            continue;
        }

        messages.push_back(nlohmann::json{
            {"role", "tool"},
            {"tool_name", call.name},
            {"content", call.output()},
        });
    }
}

}  // namespace

void StreamStats::record(
    std::uint64_t prompt_eval_count,
    std::uint64_t eval_count,
    std::uint64_t eval_duration_nanoseconds
) noexcept
{
    prompt_eval_count_.store(prompt_eval_count, std::memory_order_relaxed);
    eval_count_.store(eval_count, std::memory_order_relaxed);
    eval_duration_nanoseconds_.store(
        eval_duration_nanoseconds, std::memory_order_relaxed
    );
    available_.store(true, std::memory_order_release);
}

StreamStats::Snapshot StreamStats::snapshot() const noexcept
{
    const bool available = available_.load(std::memory_order_acquire);
    const std::uint64_t prompt_eval_count =
        prompt_eval_count_.load(std::memory_order_relaxed);
    const std::uint64_t eval_count =
        eval_count_.load(std::memory_order_relaxed);
    const std::uint64_t eval_duration_nanoseconds =
        eval_duration_nanoseconds_.load(std::memory_order_relaxed);
    const double eval_duration_seconds =
        static_cast<double>(eval_duration_nanoseconds) / 1'000'000'000.0;
    const double tokens_per_second = eval_duration_seconds > 0.0
        ? static_cast<double>(eval_count) / eval_duration_seconds
        : 0.0;
    return Snapshot{
        .available = available,
        .prompt_eval_count = prompt_eval_count,
        .eval_count = eval_count,
        .eval_duration_seconds = eval_duration_seconds,
        .tokens_per_second = tokens_per_second,
    };
}

StreamDecoder::StreamDecoder(std::shared_ptr<StreamStats> stats)
    : stats_{std::move(stats)}
{
}

void StreamDecoder::feed(std::string_view chunk, const EventSink& sink)
{
    line_buf_.append(chunk);

    std::size_t start = 0;
    while (true) {
        const std::size_t newline = line_buf_.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }

        process_line(std::string_view{line_buf_}.substr(start, newline - start), sink);
        start = newline + 1;
    }

    // 残余是下一个切片的前缀，留着。
    line_buf_.erase(0, start);
}

void StreamDecoder::process_line(std::string_view line, const EventSink& sink)
{
    const nlohmann::json frame = nlohmann::json::parse(line, nullptr, false);
    if (frame.is_discarded() || !frame.is_object()) {
        return;  // 空行或心跳，跳过
    }

    const auto message = frame.find("message");
    if (message != frame.end() && message->is_object()) {
        // 工具调用单帧原子到达，arguments 已经是 JSON object —— 与
        // StreamToolCall 1:1 对应，不需要 delta 累积。
        const auto tool_calls = message->find("tool_calls");
        if (tool_calls != message->end() && tool_calls->is_array()) {
            for (const nlohmann::json& call : *tool_calls) {
                const auto function = call.find("function");
                if (function == call.end() || !function->is_object()) {
                    continue;
                }

                sink(Msg{StreamToolCall{
                    .id = call.value("id", std::string{}),
                    .name = function->value("name", std::string{}),
                    .args = function->value("arguments", nlohmann::json::object()),
                }});
            }
        }

        // 终态帧也带一个空 content，投出空 delta 只会在 Model 里留噪声。
        const std::string content = message->value("content", std::string{});
        if (!content.empty()) {
            sink(Msg{StreamTextDelta{.text = content}});
        }
    }

    if (frame.value("done", false)) {
        if (stats_) {
            stats_->record(
                frame.value("prompt_eval_count", std::uint64_t{0}),
                frame.value("eval_count", std::uint64_t{0}),
                frame.value("eval_duration", std::uint64_t{0})
            );
        }
        sink(Msg{StreamFinished{}});
    }
}

std::string build_request_body(std::string_view model, const Request& request)
{
    nlohmann::json messages = nlohmann::json::array();

    // 系统提示是顶层之外的一条 role=="system" 消息，放在历史最前面。
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
        // thinking 关闭：demo 输出保持干净，且 Msg 变体不需要新增 thinking 通道。
        {"think", false},
        {"messages", std::move(messages)},
    };

    if (!request.tools.empty()) {
        body["tools"] = encode_tools(request.tools);
    }

    return body.dump();
}

StreamEffect make_stream(
    std::string host,
    int port,
    std::string model,
    std::shared_ptr<http::HttpClient> client,
    std::shared_ptr<StreamStats> stats
)
{
    return [
        host = std::move(host),
        port,
        model = std::move(model),
        client = std::move(client),
        stats = std::move(stats)
    ](Request request, EventSink sink) {
        const http::HttpRequest wire{
            .host = host,
            .port = port,
            .path = "/api/chat",
            .headers = {{"Content-Type", "application/json"}},
            .body = build_request_body(model, request),
            .use_tls = false,
        };

        // decoder 的行缓冲跨切片存活，所以它必须活在回调之外。
        StreamDecoder decoder{stats};

        const http::HttpResult result =
            client->post_stream(wire, [&decoder, &sink](std::string_view chunk) {
                decoder.feed(chunk, sink);
                return true;
            });

        // 宿主要求每次 stream 调用恰好投出一个终态事件。成功路径上 done==true
        // 的帧已经投过 StreamFinished，失败路径在这里补 StreamError。
        if (!result) {
            sink(Msg{StreamError{.message = result.error().message}});
        }
    };
}

}  // namespace my_agent::provider::ollama
