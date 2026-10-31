#include "my_agent/provider/openai.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/msg.hpp"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<my_agent::ProviderEvent> decode(const std::vector<std::string>& chunks)
{
    my_agent::provider::openai::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;

    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    for (const std::string& chunk : chunks) {
        decoder.feed(chunk, sink);
    }
    return messages;
}

}  // namespace

// 场景：两个工具调用在同一个流里按 index 交错到达，参数逐片流式送来。
// 领域语义：这是 OpenAI 与 Anthropic 的关键差异 —— Anthropic 一次只开一个
// content_block，靠 content_block_stop 收尾；OpenAI 用 index 分组且允许交错，
// 没有任何「块结束」事件，只能等 finish_reason 统一 flush。累积状态因此必须是
// index → 累积器的映射，而不是单一组变量。
// Red 原因：仓库尚无 openai.cpp，StreamDecoder::feed 无定义。
TEST(OpenaiStreamDecoderTest, AccumulatesInterleavedToolCallsByIndexAndFlushesInOrder)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_a\",\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"id\":\"call_b\",\"function\":{\"name\":\"write\",\"arguments\":\"\"}}]}}]}\n\n",

        // 两个工具的参数碎片交错到达
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"pa\"}}]}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"function\":{\"arguments\":\"{\\\"pa\"}}]}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"th\\\":\\\"a.txt\\\"}\"}}]}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"function\":{\"arguments\":\"th\\\":\\\"b.txt\\\"}\"}}]}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n",

        "data: [DONE]\n\n",
    });

    ASSERT_EQ(std::size_t{3}, messages.size());

    const auto* first = std::get_if<my_agent::StreamToolCall>(&messages.at(0));
    ASSERT_NE(nullptr, first);
    EXPECT_EQ("call_a", first->id);
    EXPECT_EQ("read", first->name);
    EXPECT_EQ("a.txt", first->args.at("path").get<std::string>());

    const auto* second = std::get_if<my_agent::StreamToolCall>(&messages.at(1));
    ASSERT_NE(nullptr, second);
    EXPECT_EQ("call_b", second->id);
    EXPECT_EQ("write", second->name);
    EXPECT_EQ("b.txt", second->args.at("path").get<std::string>());

    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(2)));
}

// 场景：纯文本回合，以 finish_reason=stop 和 data: [DONE] 收尾。
// 领域语义：文本 delta 立刻投出。终态只投一次 —— finish_reason 与 [DONE] 都算
// 收尾信号，但宿主要求「恰好一个终态事件」，所以第二个必须被吞掉。
TEST(OpenaiStreamDecoderTest, StreamsTextAndEmitsExactlyOneTerminalEvent)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
        "\"content\":\"\"}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"po\"}}]}\n\n",
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ng\"}}]}\n\n",

        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
        "data: [DONE]\n\n",
    });

    ASSERT_EQ(std::size_t{3}, messages.size());
    EXPECT_EQ("po", std::get<my_agent::StreamTextDelta>(messages.at(0)).text);
    EXPECT_EQ("ng", std::get<my_agent::StreamTextDelta>(messages.at(1)).text);
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(2)));
}

TEST(OpenaiStreamDecoderTest, PublishesUsageBeforeTerminalWhenUsageOnlyChunkFollowsFinish)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n",
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":128,"
        "\"completion_tokens\":36,\"total_tokens\":164,"
        "\"prompt_tokens_details\":{\"cached_tokens\":7}}}\n\n",
        "data: [DONE]\n\n",
    });

    ASSERT_EQ(std::size_t{2}, messages.size());
    const auto* usage = std::get_if<my_agent::RequestUsage>(&messages.at(0));
    ASSERT_NE(nullptr, usage);
    EXPECT_EQ(128u, usage->input_tokens);
    ASSERT_TRUE(usage->cached_input_tokens.has_value());
    EXPECT_EQ(7u, *usage->cached_input_tokens);
    EXPECT_EQ(36u, usage->output_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Exact, usage->quality);
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(1)));
}

TEST(OpenaiStreamDecoderTest, CompletesAtHttpEofWhenDoneSentinelIsMissing)
{
    my_agent::provider::openai::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;
    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    decoder.feed(
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,"
        "\"completion_tokens\":3}}\n\n",
        sink);
    decoder.finish(sink);
    decoder.finish(sink);

    ASSERT_EQ(std::size_t{2}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::RequestUsage>(messages.at(0)));
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(1)));
}

TEST(OpenaiStreamDecoderTest, IgnoresLateFramesAfterTerminal)
{
    my_agent::provider::openai::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;
    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    decoder.feed("data: [DONE]\n\n", sink);
    decoder.feed(
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,"
        "\"completion_tokens\":3}}\n\n",
        sink);

    ASSERT_EQ(std::size_t{1}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.front()));
}

TEST(OpenaiStreamDecoderTest, IgnoresMalformedFrameAndStillFinishes)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {not-json}\n\n",
        "data: [DONE]\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.front()));
}

TEST(OpenaiRequestTest, RequestsUsageInStreamingChatCompletions)
{
    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::openai::build_request_body(
            "gpt-4o", my_agent::Request{}));

    ASSERT_TRUE(body.contains("stream_options"));
    EXPECT_TRUE(body.at("stream_options").at("include_usage").get<bool>());
}

TEST(OpenaiRequestTest, SendsCompactionOutputBudgetToChatCompletions)
{
    my_agent::Request request;
    request.max_output_tokens = 777;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::openai::build_request_body("gpt-4o", request));

    EXPECT_EQ(777u, body.at("max_tokens").get<std::uint64_t>());
}

TEST(OpenaiRequestTest, SendsNormalOutputCapToChatCompletions)
{
    my_agent::Request request;
    request.max_output_tokens = 2048;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::openai::build_request_body("gpt-4o", request));

    EXPECT_EQ(2048u, body.at("max_tokens").get<std::uint64_t>());
}

// 场景：流里只有 data: [DONE]，没有 finish_reason（部分兼容实现如此）。
// 领域语义：[DONE] 单独也必须能收尾，否则 owner 会永久停在 Streaming 上。
TEST(OpenaiStreamDecoderTest, FinishesOnDoneSentinelAlone)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"}}]}\n\n",
        "data: [DONE]\n\n",
    });

    ASSERT_EQ(std::size_t{2}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(1)));
}

// 场景：服务端在流中途发一个 error 对象。
// 领域语义：与 Anthropic 同理，错误也是终态，折成 StreamError 让回合可见地结束。
TEST(OpenaiStreamDecoderTest, ConvertsErrorPayloadIntoStreamError)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"error\":{\"message\":\"context_length_exceeded\"}}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    const auto* error = std::get_if<my_agent::StreamError>(&messages.front());
    ASSERT_NE(nullptr, error);
    EXPECT_EQ("context_length_exceeded", error->message);
    ASSERT_TRUE(error->provider_error.has_value());
    EXPECT_EQ(
        my_agent::ProviderErrorKind::ContextLimit,
        error->provider_error->kind
    );
}

TEST(OpenaiStreamDecoderTest, NormalizesContextLimitErrorWithProviderMetadata)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "data: {\"error\":{\"message\":\"This model's maximum context "
        "length is 4096 tokens\",\"type\":\"invalid_request_error\","
        "\"code\":\"context_length_exceeded\"}}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    const auto* error = std::get_if<my_agent::StreamError>(&messages.front());
    ASSERT_NE(nullptr, error);
    ASSERT_TRUE(error->provider_error.has_value());
    EXPECT_EQ(
        my_agent::ProviderErrorKind::ContextLimit,
        error->provider_error->kind
    );
    EXPECT_EQ("context_length_exceeded", error->provider_error->code);
    EXPECT_EQ(
        "This model's maximum context length is 4096 tokens",
        error->message
    );
}

// 场景：带系统提示与工具表的一次请求。
// 领域语义：system 是 messages 数组的**首元素**（Anthropic 是顶层字段），tools
// 用 {"type":"function","function":{...,"parameters":...}} 包装（Anthropic 是
// 扁平的 input_schema）。同一个 Request 按各家形状分别序列化。
TEST(OpenaiRequestTest, PutsSystemPromptAsTheFirstMessageAndWrapsToolsInFunction)
{
    my_agent::Request request{
        .messages = {
            my_agent::Message{.role = my_agent::Role::User, .text = "hello"},
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "read",
                .description = "read a file",
                .input_schema = nlohmann::json{{"type", "object"}},
            },
        },
        .system_prompt = "you are kiro",
    };

    const nlohmann::json body =
        nlohmann::json::parse(
            my_agent::provider::openai::build_request_body("gpt-4o", request)
        );

    EXPECT_EQ("gpt-4o", body.at("model"));
    EXPECT_TRUE(body.at("stream").get<bool>());

    ASSERT_EQ(std::size_t{2}, body.at("messages").size());
    EXPECT_EQ("system", body.at("messages").at(0).at("role"));
    EXPECT_EQ("you are kiro", body.at("messages").at(0).at("content"));
    EXPECT_EQ("user", body.at("messages").at(1).at("role"));

    ASSERT_EQ(std::size_t{1}, body.at("tools").size());
    EXPECT_EQ("function", body.at("tools").at(0).at("type"));
    EXPECT_EQ("read", body.at("tools").at(0).at("function").at("name"));
    EXPECT_EQ(
        "object",
        body.at("tools").at(0).at("function").at("parameters").at("type")
    );
}

// 场景：历史里有一条带已完成工具调用的 assistant 消息。
// 领域语义：OpenAI 的工具结果是一条独立的 role=="tool" 消息，用 tool_call_id
// 关联（Ollama 用 tool_name，Anthropic 用 user 消息里的 tool_result 块）。
// assistant 的 tool_calls[].function.arguments 出站时必须**序列化成字符串**，
// 不能直接放 object —— 这是与 Ollama 最容易混淆的一处。
TEST(OpenaiRequestTest, SerializesToolArgumentsAsAStringAndUsesToolCallIdForResults)
{
    my_agent::Message assistant{
        .role = my_agent::Role::Assistant,
        .text = "checking",
    };
    assistant.tool_calls.push_back(my_agent::ToolCall{
        .id = "call_a",
        .name = "read",
        .args = nlohmann::json{{"path", "a.txt"}},
        .status = my_agent::ToolCall::Done{.output = "file body"},
    });

    my_agent::Request request{
        .messages = {
            my_agent::Message{.role = my_agent::Role::User, .text = "read it"},
            std::move(assistant),
        },
    };

    const nlohmann::json body =
        nlohmann::json::parse(
            my_agent::provider::openai::build_request_body("m", request)
        );

    ASSERT_EQ(std::size_t{3}, body.at("messages").size());

    const nlohmann::json& turn = body.at("messages").at(1);
    EXPECT_EQ("assistant", turn.at("role"));
    ASSERT_EQ(std::size_t{1}, turn.at("tool_calls").size());

    const nlohmann::json& call = turn.at("tool_calls").at(0);
    EXPECT_EQ("call_a", call.at("id"));
    EXPECT_EQ("function", call.at("type"));
    EXPECT_EQ("read", call.at("function").at("name"));

    // arguments 必须是字符串，内容再解析一次才是参数对象
    const nlohmann::json& arguments = call.at("function").at("arguments");
    ASSERT_TRUE(arguments.is_string());
    EXPECT_EQ(
        "a.txt",
        nlohmann::json::parse(arguments.get<std::string>()).at("path")
    );

    const nlohmann::json& result = body.at("messages").at(2);
    EXPECT_EQ("tool", result.at("role"));
    EXPECT_EQ("call_a", result.at("tool_call_id"));
    EXPECT_EQ("file body", result.at("content"));
}
