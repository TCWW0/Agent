#include "my_agent/provider/ollama.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

// 把 decoder 投出的 Msg 收集起来，便于对序列做断言。
std::vector<my_agent::ProviderEvent> decode_all(const std::vector<std::string_view>& chunks)
{
    my_agent::provider::ollama::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> collected;

    const my_agent::ProviderEventSink sink = [&collected](my_agent::ProviderEvent msg) {
        collected.push_back(std::move(msg));
    };

    for (const std::string_view chunk : chunks) {
        decoder.feed(chunk, sink);
    }

    return collected;
}

// 场景：一个完整的文本帧恰好落在一个字节切片里。
// 领域语义：NDJSON 的一行就是一个事件。带 content 且 done==false 的帧是一次
// 文本增量，映射成 StreamTextDelta。
// Red 原因：仓库尚不存在 StreamDecoder（编译错误）。
TEST(OllamaTransportTest, MapsTextFrameToStreamTextDelta)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"message":{"role":"assistant","content":"Hello"},"done":false})"
        "\n",
    });

    ASSERT_EQ(std::size_t{1}, msgs.size());
    const auto* delta = std::get_if<my_agent::StreamTextDelta>(&msgs.front());
    ASSERT_NE(nullptr, delta);
    EXPECT_EQ("Hello", delta->text);
}

// 场景：一行 JSON 被 HTTP 层切成了任意几段，且一段里可能含多行。
// 领域语义：分帧必须只看 '\n'，跨切片保留残余。这是传输层最容易出错的地方 ——
// 按切片边界解析会把一行 JSON 拆成两个解析失败。
// Red 原因：无跨切片缓冲时，被切碎的行会解析失败从而丢事件。
TEST(OllamaTransportTest, ReassemblesFramesSplitAcrossChunkBoundaries)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"message":{"content":"po"},"do)",
        "ne\":false}\n{\"message\":{\"content\":\"ng\"},\"done\":false}\n",
    });

    ASSERT_EQ(std::size_t{2}, msgs.size());

    const auto* first = std::get_if<my_agent::StreamTextDelta>(&msgs.at(0));
    ASSERT_NE(nullptr, first);
    EXPECT_EQ("po", first->text);

    const auto* second = std::get_if<my_agent::StreamTextDelta>(&msgs.at(1));
    ASSERT_NE(nullptr, second);
    EXPECT_EQ("ng", second->text);
}

// 场景：模型请求一次工具调用。
// 领域语义：Ollama 的工具调用单帧原子到达，arguments 已经是 JSON object ——
// 与既有的 StreamToolCall{id,name,args} 1:1 对应，不需要 delta 累积。这正是
// 选它作为第一个真实 Provider 的理由（SSE 系的 Anthropic/OpenAI 是逐字符流式
// 的，需要按 index 累积后才能发出一个完整调用）。
// 真实抓包（qwen3.5，2026-08-03）：
//   {"message":{"content":"","tool_calls":[{"id":"call_jwze9vg9",
//    "function":{"index":0,"name":"calculator",
//    "arguments":{"operation":"multiply","left":6,"right":7}}}]},"done":false}
TEST(OllamaTransportTest, MapsAtomicToolCallFrameToStreamToolCall)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"message":{"role":"assistant","content":"","tool_calls":[)"
        R"({"id":"call_jwze9vg9","function":{"index":0,"name":"calculator",)"
        R"("arguments":{"operation":"multiply","left":6,"right":7}}}]},)"
        R"("done":false})"
        "\n",
    });

    ASSERT_EQ(std::size_t{1}, msgs.size());
    const auto* call = std::get_if<my_agent::StreamToolCall>(&msgs.front());
    ASSERT_NE(nullptr, call);
    EXPECT_EQ("call_jwze9vg9", call->id);
    EXPECT_EQ("calculator", call->name);
    EXPECT_EQ("multiply", call->args.at("operation").get<std::string>());
    EXPECT_EQ(6, call->args.at("left").get<int>());
    EXPECT_EQ(7, call->args.at("right").get<int>());
}

// 场景：终态帧。
// 领域语义：done==true 是回合终点，映射成 StreamFinished。它同时携带一个空的
// content，不应该额外投出一个空的 StreamTextDelta —— 那会在 Model 里留下噪声。
TEST(OllamaTransportTest, MapsDoneFrameWithoutUsageToFinishedWithoutEmptyDelta)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"message":{"role":"assistant","content":""},"done":true,)"
        R"("done_reason":"stop","eval_count":36})"
        "\n",
    });

    ASSERT_EQ(std::size_t{1}, msgs.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(msgs.at(0)));
}

TEST(OllamaTransportTest, PublishesNormalizedUsageBeforeStreamFinished)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"message":{"role":"assistant","content":""},"done":true,)"
        R"("prompt_eval_count":128,"prompt_eval_cached_count":7,)"
        R"("eval_count":36})"
        "\n",
    });

    ASSERT_EQ(std::size_t{2}, msgs.size());
    const auto* usage = std::get_if<my_agent::RequestUsage>(&msgs.at(0));
    ASSERT_NE(nullptr, usage);
    EXPECT_EQ(128u, usage->input_tokens);
    ASSERT_TRUE(usage->cached_input_tokens.has_value());
    EXPECT_EQ(7u, *usage->cached_input_tokens);
    EXPECT_EQ(36u, usage->output_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Exact, usage->quality);
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(msgs.at(1)));
}

TEST(OllamaTransportTest, NormalizesContextSizeErrorAsContextLimit)
{
    const std::vector<my_agent::ProviderEvent> msgs = decode_all({
        R"({"error":"the request exceeds the available context size"})"
        "\n",
    });

    ASSERT_EQ(std::size_t{1}, msgs.size());
    const auto* error = std::get_if<my_agent::StreamError>(&msgs.front());
    ASSERT_NE(nullptr, error);
    ASSERT_TRUE(error->provider_error.has_value());
    EXPECT_EQ(
        my_agent::ProviderErrorKind::ContextLimit,
        error->provider_error->kind
    );
    EXPECT_EQ("the request exceeds the available context size", error->message);
}

TEST(OllamaTransportTest, PublishesEvalStatsForTheStatusBar)
{
    const auto stats = std::make_shared<my_agent::provider::ollama::StreamStats>();
    my_agent::provider::ollama::StreamDecoder decoder{stats};
    std::vector<my_agent::ProviderEvent> msgs;
    const my_agent::ProviderEventSink sink = [&msgs](my_agent::ProviderEvent msg) {
        msgs.push_back(std::move(msg));
    };

    decoder.feed(
        R"({"message":{"content":""},"done":true,"prompt_eval_count":128,)"
        R"("prompt_eval_duration":1000000000,"eval_count":36,)"
        R"("eval_duration":3000000000,"total_duration":4000000000})"
        "\n",
        sink
    );

    ASSERT_EQ(std::size_t{2}, msgs.size());
    const my_agent::provider::ollama::StreamStats::Snapshot snapshot =
        stats->snapshot();
    EXPECT_EQ(128u, snapshot.prompt_eval_count);
    EXPECT_EQ(36u, snapshot.eval_count);
    EXPECT_EQ(3.0, snapshot.eval_duration_seconds);
    EXPECT_EQ(12.0, snapshot.tokens_per_second);
    EXPECT_EQ(1u, snapshot.generation);
    const auto* usage = std::get_if<my_agent::RequestUsage>(&msgs.at(0));
    ASSERT_NE(nullptr, usage);
    EXPECT_FALSE(usage->cached_input_tokens.has_value());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(msgs.at(1)));
}

// 场景：把一段带已完成工具调用的历史编成出站请求。
// 领域语义：我们的 Role 只有 User/Assistant，工具结果藏在 assistant message 的
// tool_calls[].status 里。Ollama 不认识这个形状 —— 出站时必须展开成 assistant
// 消息本身，紧跟每个已终结 tool_call 的一条 role=="tool" 消息。关联键是
// tool_name 而不是 tool_call_id（这是 Ollama 与 OpenAI 的一处真实差异）。
// 漏掉这步展开，模型就看不到工具返回了什么，会反复请求同一个调用。
TEST(OllamaTransportTest, ExpandsFinishedToolCallsIntoSeparateToolMessages)
{
    my_agent::Request request;
    request.messages = {
        my_agent::Message{.role = my_agent::Role::User, .text = "6 times 7?"},
        my_agent::Message{
            .role = my_agent::Role::Assistant,
            .text = {},
            .error = {},
            .tool_calls = {
                my_agent::ToolCall{
                    .id = "call-1",
                    .name = "calculator",
                    .args = {{"operation", "multiply"}, {"left", 6}, {"right", 7}},
                    .status = my_agent::ToolCall::Done{.output = "42"},
                },
            },
        },
    };

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body("qwen3.5:latest", request)
    );

    EXPECT_EQ("qwen3.5:latest", body.at("model").get<std::string>());
    EXPECT_TRUE(body.at("stream").get<bool>());
    // thinking 关闭是已确认的决策：demo 输出保持干净。
    EXPECT_FALSE(body.at("think").get<bool>());

    const nlohmann::json& messages = body.at("messages");
    ASSERT_EQ(std::size_t{3}, messages.size());

    EXPECT_EQ("user", messages.at(0).at("role").get<std::string>());

    EXPECT_EQ("assistant", messages.at(1).at("role").get<std::string>());
    ASSERT_EQ(std::size_t{1}, messages.at(1).at("tool_calls").size());
    EXPECT_EQ(
        "calculator",
        messages.at(1).at("tool_calls").at(0).at("function").at("name")
            .get<std::string>()
    );

    // 展开出来的工具结果消息。
    EXPECT_EQ("tool", messages.at(2).at("role").get<std::string>());
    EXPECT_EQ("calculator", messages.at(2).at("tool_name").get<std::string>());
    EXPECT_EQ("42", messages.at(2).at("content").get<std::string>());
}

// 场景：宿主填好了系统提示。
// 领域语义：系统提示由宿主在 effect 侧注入（构建它要读 memory/skills 文件，而
// update() 必须保持纯）。Ollama 把它当作历史最前面的一条 role=="system" 消息。
// 这是 M7 的注入点，现在先把通路打通。
TEST(OllamaTransportTest, PlacesSystemPromptAsTheFirstMessage)
{
    my_agent::Request request;
    request.system_prompt = "You are a helpful assistant.";
    request.messages = {
        my_agent::Message{.role = my_agent::Role::User, .text = "hi"},
    };

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body("qwen3.5:latest", request)
    );

    const nlohmann::json& messages = body.at("messages");
    ASSERT_EQ(std::size_t{2}, messages.size());
    EXPECT_EQ("system", messages.at(0).at("role").get<std::string>());
    EXPECT_EQ(
        "You are a helpful assistant.",
        messages.at(0).at("content").get<std::string>()
    );
    EXPECT_EQ("user", messages.at(1).at("role").get<std::string>());
}

// 场景：注册表里的工具规格随请求出站。
// 领域语义：Ollama 用 {"type":"function","function":{name,description,parameters}}
// 包装工具，parameters 就是我们的 input_schema。没有这段模型就不知道能调什么。
TEST(OllamaTransportTest, EncodesToolSpecsAsFunctionDeclarations)
{
    my_agent::Request request;
    request.messages = {
        my_agent::Message{.role = my_agent::Role::User, .text = "hi"},
    };
    request.tools = {
        my_agent::ToolSpec{
            .name = "calculator",
            .description = "Multiply two numbers.",
            .input_schema = {{"type", "object"}},
        },
    };

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body("qwen3.5:latest", request)
    );

    ASSERT_EQ(std::size_t{1}, body.at("tools").size());
    const nlohmann::json& tool = body.at("tools").at(0);
    EXPECT_EQ("function", tool.at("type").get<std::string>());
    EXPECT_EQ("calculator", tool.at("function").at("name").get<std::string>());
    EXPECT_EQ(
        "Multiply two numbers.",
        tool.at("function").at("description").get<std::string>()
    );
    EXPECT_EQ(
        "object",
        tool.at("function").at("parameters").at("type").get<std::string>()
    );
}

TEST(OllamaTransportTest, SendsConfiguredContextLimitAsNumCtx)
{
    my_agent::Request request;
    request.messages = {
        my_agent::Message{.role = my_agent::Role::User, .text = "hello"},
    };

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body(
            "qwen3.5:latest", request, 4096)
    );

    ASSERT_TRUE(body.contains("options"));
    EXPECT_EQ(4096u, body.at("options").at("num_ctx").get<std::size_t>());
}

TEST(OllamaTransportTest, SendsCompactionOutputBudgetAsNumPredict)
{
    my_agent::Request request;
    request.max_output_tokens = 777;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body(
            "qwen3.5:latest", request));

    EXPECT_EQ(777u, body.at("options").at("num_predict")
        .get<std::uint64_t>());
}

TEST(OllamaTransportTest, SendsNormalOutputCapAsNumPredict)
{
    my_agent::Request request;
    request.max_output_tokens = 2048;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::ollama::build_request_body(
            "qwen3.5:latest", request));

    EXPECT_EQ(2048u, body.at("options").at("num_predict")
        .get<std::uint64_t>());
}

// ── 集成测试 ──────────────────────────────────────────────────────────────
// 以下测试打真实的 Ollama。11434 不可达时 skip 而不是失败 —— 这条边界让 CI 和
// 没装 Ollama 的机器仍然能跑全套测试。

constexpr const char* kOllamaHost = "localhost";
constexpr int kOllamaPort = 11434;
constexpr const char* kModel = "qwen3.5:latest";

bool ollama_reachable()
{
    const my_agent::http::HttpClient client;
    const my_agent::http::HttpRequest probe{
        .host = kOllamaHost,
        .port = kOllamaPort,
        .path = "/api/tags",
        .headers = {},
        .body = {},
        .use_tls = false,
    };

    // /api/tags 只接受 GET，POST 会拿到非 2xx —— 但那已经证明连接建立成功。
    const my_agent::http::HttpResult result =
        client.post_stream(probe, [](std::string_view) { return true; });

    return result
        || result.error().kind != my_agent::http::HttpErrorKind::ConnectionFailed;
}

// 场景：对真实 qwen3.5 跑一次完整的流式文本回合。
// 领域语义：第一颗真正的 tracer bullet —— 请求编码、HTTP 流式、行分帧、帧到
// Msg 的映射整条链路联通。断言收到至少一个文本增量，且恰好一个 StreamFinished
// 收尾（宿主的 in-flight 判定依赖这个终态唯一性）。
TEST(OllamaIntegrationTest, StreamsRealTextTurnEndingInExactlyOneFinished)
{
    if (!ollama_reachable()) {
        GTEST_SKIP() << "Ollama is not reachable on localhost:11434";
    }

    const my_agent::StreamEffect stream = my_agent::provider::ollama::make_stream(
        kOllamaHost,
        kOllamaPort,
        kModel,
        std::make_shared<my_agent::http::HttpClient>()
    );

    my_agent::Request request;
    request.messages = {
        my_agent::Message{
            .role = my_agent::Role::User,
            .text = "Reply with exactly the word: pong",
        },
    };

    std::string text;
    int finished = 0;
    int errors = 0;

    stream(request, [&](my_agent::ProviderEvent msg) {
        if (const auto* delta = std::get_if<my_agent::StreamTextDelta>(&msg)) {
            text += delta->text;
        } else if (std::holds_alternative<my_agent::StreamFinished>(msg)) {
            ++finished;
        } else if (const auto* error = std::get_if<my_agent::StreamError>(&msg)) {
            ++errors;
            ADD_FAILURE() << "unexpected stream error: " << error->message;
        }
    });

    EXPECT_EQ(0, errors);
    EXPECT_EQ(1, finished);
    EXPECT_FALSE(text.empty());
}

// 场景：给真实模型一个工具，让它实际发起调用。
// 领域语义：验证「工具调用单帧原子到达」这个假设在真实模型上成立 —— 拿到的
// StreamToolCall 必须带完整的 name 与已解析成 object 的 args，不需要任何 delta
// 累积。这是选 Ollama 作为第一个真实 Provider 的核心理由，值得对真货断言而不是
// 只信抓包。
TEST(OllamaIntegrationTest, ReceivesCompleteToolCallInASingleEvent)
{
    if (!ollama_reachable()) {
        GTEST_SKIP() << "Ollama is not reachable on localhost:11434";
    }

    const my_agent::StreamEffect stream = my_agent::provider::ollama::make_stream(
        kOllamaHost,
        kOllamaPort,
        kModel,
        std::make_shared<my_agent::http::HttpClient>()
    );

    my_agent::Request request;
    request.messages = {
        my_agent::Message{
            .role = my_agent::Role::User,
            .text = "What is 6 times 7? Use the calculator tool.",
        },
    };
    request.tools = {
        my_agent::ToolSpec{
            .name = "calculator",
            .description = "Multiply two numbers.",
            .input_schema = {
                {"type", "object"},
                {"properties", {
                    {"operation", {{"type", "string"}, {"enum", {"multiply"}}}},
                    {"left", {{"type", "number"}}},
                    {"right", {{"type", "number"}}},
                }},
                {"required", {"operation", "left", "right"}},
            },
        },
    };

    std::vector<my_agent::StreamToolCall> calls;
    int finished = 0;

    stream(request, [&](my_agent::ProviderEvent msg) {
        if (const auto* call = std::get_if<my_agent::StreamToolCall>(&msg)) {
            calls.push_back(*call);
        } else if (std::holds_alternative<my_agent::StreamFinished>(msg)) {
            ++finished;
        } else if (const auto* error = std::get_if<my_agent::StreamError>(&msg)) {
            ADD_FAILURE() << "unexpected stream error: " << error->message;
        }
    });

    EXPECT_EQ(1, finished);
    ASSERT_FALSE(calls.empty()) << "model did not request the tool";

    const my_agent::StreamToolCall& call = calls.front();
    EXPECT_EQ("calculator", call.name);
    EXPECT_FALSE(call.id.empty());
    // args 到手就已经是 object，不是待累积的字符串片段。
    ASSERT_TRUE(call.args.is_object());
    EXPECT_EQ("multiply", call.args.at("operation").get<std::string>());
    EXPECT_EQ(42, call.args.at("left").get<int>() * call.args.at("right").get<int>());
}

// 场景：请求一个不存在的模型。
// 领域语义：传输层失败必须变成 StreamError 而不是静默返回。宿主用「恰好一个
// 终态事件」判定 in-flight，静默返回会让 owner 永久停在 Streaming 上。
TEST(OllamaIntegrationTest, ReportsUnknownModelAsStreamError)
{
    if (!ollama_reachable()) {
        GTEST_SKIP() << "Ollama is not reachable on localhost:11434";
    }

    const my_agent::StreamEffect stream = my_agent::provider::ollama::make_stream(
        kOllamaHost,
        kOllamaPort,
        "definitely-not-a-real-model",
        std::make_shared<my_agent::http::HttpClient>()
    );

    my_agent::Request request;
    request.messages = {
        my_agent::Message{.role = my_agent::Role::User, .text = "hi"},
    };

    int errors = 0;
    int finished = 0;

    stream(request, [&](my_agent::ProviderEvent msg) {
        if (std::holds_alternative<my_agent::StreamError>(msg)) {
            ++errors;
        } else if (std::holds_alternative<my_agent::StreamFinished>(msg)) {
            ++finished;
        }
    });

    EXPECT_EQ(1, errors);
    EXPECT_EQ(0, finished);
}

}  // namespace
