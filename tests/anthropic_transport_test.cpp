#include "my_agent/provider/anthropic.hpp"

#include "my_agent/domain/conversation.hpp"
#include "my_agent/runtime/msg.hpp"

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<my_agent::ProviderEvent> decode(const std::vector<std::string>& chunks)
{
    my_agent::provider::anthropic::StreamDecoder decoder;
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

// 场景：一次工具调用横跨 content_block_start / 多个 input_json_delta /
// content_block_stop 三类事件，参数是逐片流式到达的。
// 领域语义：这是与 Ollama 的**本质差异**。Ollama 的 tool_calls 单帧原子到达，
// arguments 已是 JSON object；Anthropic 在 start 时只有 id 与 name，参数是一串
// partial_json 文本碎片。但我们的 StreamToolCall 契约是「一个完整调用一个事件」，
// 所以传输层必须累积、延迟到 stop 才发出恰好一个事件 —— Core 与宿主完全不需要
// 知道 provider 是原子还是流式的。
// Red 原因：仓库尚无 anthropic.cpp，StreamDecoder::feed 无定义。
TEST(AnthropicStreamDecoderTest, EmitsOneCompleteToolCallAfterAccumulatingPartialJson)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"pa\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"th\\\":\\\"a.txt\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"}\"}}\n\n",

        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());

    const auto* call = std::get_if<my_agent::StreamToolCall>(&messages.front());
    ASSERT_NE(nullptr, call);
    EXPECT_EQ("toolu_1", call->id);
    EXPECT_EQ("read", call->name);
    EXPECT_EQ("a.txt", call->args.at("path").get<std::string>());
}

// 场景：一个回合里先有文本，再有一次工具调用，最后 message_stop 收尾。
// 领域语义：文本块与工具块共享同一套 content_block_* 事件名，只靠 type 区分。
// 文本 delta 必须立刻投出（用户要看到流式），工具调用必须延迟到 stop 才投出 ——
// 两种时序共存于一个解码器里。message_stop 是唯一的终态事件，映射到
// StreamFinished，宿主的「每次 stream 恰好一个终态事件」保证靠它满足。
TEST(AnthropicStreamDecoderTest, StreamsTextImmediatelyAndFinishesOnMessageStop)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}\n\n",

        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Let me \"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"look.\"}}\n\n",

        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n",

        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"read\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"b.txt\\\"}\"}}\n\n",

        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n",

        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n",

        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n",
    });

    ASSERT_EQ(std::size_t{4}, messages.size());

    const auto* first = std::get_if<my_agent::StreamTextDelta>(&messages.at(0));
    ASSERT_NE(nullptr, first);
    EXPECT_EQ("Let me ", first->text);

    const auto* second = std::get_if<my_agent::StreamTextDelta>(&messages.at(1));
    ASSERT_NE(nullptr, second);
    EXPECT_EQ("look.", second->text);

    const auto* call = std::get_if<my_agent::StreamToolCall>(&messages.at(2));
    ASSERT_NE(nullptr, call);
    EXPECT_EQ("toolu_2", call->id);
    EXPECT_EQ("b.txt", call->args.at("path").get<std::string>());

    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(3)));
}

TEST(AnthropicStreamDecoderTest, PublishesCombinedUsageBeforeMessageStopTerminal)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{"
        "\"usage\":{\"input_tokens\":100,"
        "\"cache_read_input_tokens\":7,"
        "\"cache_creation_input_tokens\":3}}}\n\n",

        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{"
        "\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":36}}\n\n",

        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n",
    });

    ASSERT_EQ(std::size_t{2}, messages.size());
    const auto* usage = std::get_if<my_agent::RequestUsage>(&messages.at(0));
    ASSERT_NE(nullptr, usage);
    EXPECT_EQ(110u, usage->input_tokens);
    ASSERT_TRUE(usage->cached_input_tokens.has_value());
    EXPECT_EQ(10u, *usage->cached_input_tokens);
    ASSERT_TRUE(usage->cache_read_input_tokens.has_value());
    EXPECT_EQ(7u, *usage->cache_read_input_tokens);
    ASSERT_TRUE(usage->cache_creation_input_tokens.has_value());
    EXPECT_EQ(3u, *usage->cache_creation_input_tokens);
    EXPECT_EQ(36u, usage->output_tokens);
    EXPECT_EQ(my_agent::UsageQuality::Exact, usage->quality);
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(1)));
}

TEST(AnthropicStreamDecoderTest, CompletesAtHttpEofWhenMessageStopIsMissing)
{
    my_agent::provider::anthropic::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;
    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    decoder.feed(
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{"
        "\"usage\":{\"input_tokens\":12}}}\n\n",
        sink);
    decoder.feed(
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"usage\":{"
        "\"output_tokens\":3}}\n\n",
        sink);
    decoder.finish(sink);

    ASSERT_EQ(std::size_t{2}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::RequestUsage>(messages.at(0)));
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.at(1)));
}

TEST(AnthropicStreamDecoderTest, EmitsOnlyOneTerminalForDuplicateMessageStop)
{
    my_agent::provider::anthropic::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;
    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    decoder.feed(
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n",
        sink);

    ASSERT_EQ(std::size_t{1}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.front()));
}

TEST(AnthropicStreamDecoderTest, IgnoresMalformedFrameAndStillFinishes)
{
    my_agent::provider::anthropic::StreamDecoder decoder;
    std::vector<my_agent::ProviderEvent> messages;
    my_agent::ProviderEventSink sink = [&messages](my_agent::ProviderEvent msg) {
        messages.push_back(std::move(msg));
    };

    decoder.feed(
        "event: message_start\n"
        "data: {not-json}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n",
        sink);

    ASSERT_EQ(std::size_t{1}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamFinished>(messages.front()));
}

// 场景：Anthropic 在流中途发 error 事件（超载、上下文超限等）。
// 领域语义：错误也是终态事件之一。传输层把它折成 StreamError，宿主据此结束回合
// 并让错误在 Model 里可见，而不是让 owner 永久等一个不会来的 message_stop。
TEST(AnthropicStreamDecoderTest, ConvertsErrorEventIntoStreamError)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
        "\"message\":\"Overloaded\"}}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    const auto* error = std::get_if<my_agent::StreamError>(&messages.front());
    ASSERT_NE(nullptr, error);
    EXPECT_EQ("Overloaded", error->message);
}

TEST(AnthropicStreamDecoderTest, NormalizesPromptTooLongAsContextLimit)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{"
        "\"type\":\"invalid_request_error\","
        "\"message\":\"prompt is too long for this model\"}}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    const auto* error = std::get_if<my_agent::StreamError>(&messages.front());
    ASSERT_NE(nullptr, error);
    ASSERT_TRUE(error->provider_error.has_value());
    EXPECT_EQ(
        my_agent::ProviderErrorKind::ContextLimit,
        error->provider_error->kind
    );
    EXPECT_EQ("invalid_request_error", error->provider_error->type);
    EXPECT_EQ("prompt is too long for this model", error->message);
}

// 场景：一个不带任何参数的工具，partial_json 一片都不会到达。
// 领域语义：空累积串不等于「解析失败」，而等于空参数对象。如果按解析失败处理，
// 无参工具就永远调不起来。
TEST(AnthropicStreamDecoderTest, TreatsAnEmptyArgumentAccumulatorAsAnEmptyObject)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_3\",\"name\":\"now\"}}\n\n",

        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    const auto* call = std::get_if<my_agent::StreamToolCall>(&messages.front());
    ASSERT_NE(nullptr, call);
    EXPECT_EQ("now", call->name);
    EXPECT_TRUE(call->args.is_object());
    EXPECT_TRUE(call->args.empty());
}

// 场景：文本块的 content_block_stop 到达。
// 领域语义：stop 事件对文本块必须是**无操作**。若不区分块类型，文本块收尾会
// 投出一个 id/name 全空的假工具调用 —— 这是累积式解码器最容易踩的坑。
TEST(AnthropicStreamDecoderTest, DoesNotSynthesizeAToolCallWhenATextBlockStops)
{
    const std::vector<my_agent::ProviderEvent> messages = decode({
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",

        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n",

        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
    });

    ASSERT_EQ(std::size_t{1}, messages.size());
    EXPECT_TRUE(std::holds_alternative<my_agent::StreamTextDelta>(messages.front()));
}

// 场景：带系统提示与工具表的一次请求。
// 领域语义：Anthropic 的 system 是**顶层字段**，不是 messages 数组的首元素
// （Ollama 与 OpenAI 都是后者）。tools 的 schema 键叫 input_schema，也不是
// OpenAI 的 function.parameters。同一个 Request 结构要按各家的形状分别序列化，
// 这层差异被关在传输层里，Core 完全不知情。
TEST(AnthropicRequestTest, PutsSystemPromptAtTheTopLevelAndUsesInputSchemaForTools)
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
            my_agent::provider::anthropic::build_request_body(
                "claude-opus-4-7", request
            )
        );

    EXPECT_EQ("claude-opus-4-7", body.at("model"));
    EXPECT_TRUE(body.at("stream").get<bool>());
    EXPECT_EQ("you are kiro", body.at("system"));

    ASSERT_EQ(std::size_t{1}, body.at("messages").size());
    EXPECT_EQ("user", body.at("messages").at(0).at("role"));

    ASSERT_EQ(std::size_t{1}, body.at("tools").size());
    EXPECT_EQ("read", body.at("tools").at(0).at("name"));
    EXPECT_EQ("object", body.at("tools").at(0).at("input_schema").at("type"));
}

TEST(AnthropicRequestTest, SendsConfiguredOutputBudgetAsMaxTokens)
{
    my_agent::Request request;
    request.max_output_tokens = 777;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::anthropic::build_request_body("claude", request));

    EXPECT_EQ(777u, body.at("max_tokens").get<std::uint64_t>());
}

TEST(AnthropicRequestTest, SendsNormalOutputCapAsMaxTokens)
{
    my_agent::Request request;
    request.max_output_tokens = 2048;

    const nlohmann::json body = nlohmann::json::parse(
        my_agent::provider::anthropic::build_request_body("claude", request));

    EXPECT_EQ(2048u, body.at("max_tokens").get<std::uint64_t>());
}

// 场景：历史里有一条带已完成工具调用的 assistant 消息。
// 领域语义：我们的 Role 只有 User/Assistant，工具结果藏在 tool_calls[].status
// 里。Anthropic 要求 assistant 消息的 content 是**块数组**（text 块 + tool_use
// 块），工具结果则是紧跟的一条 **user** 消息里的 tool_result 块，用 tool_use_id
// 关联（Ollama 用 tool_name，这里用 id）。出站时必须展开成这个形状。
TEST(AnthropicRequestTest, ExpandsFinishedToolCallsIntoToolUseAndToolResultBlocks)
{
    my_agent::Message assistant{
        .role = my_agent::Role::Assistant,
        .text = "checking",
    };
    assistant.tool_calls.push_back(my_agent::ToolCall{
        .id = "toolu_1",
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
            my_agent::provider::anthropic::build_request_body("m", request)
        );

    // user 提问 → assistant(text + tool_use) → user(tool_result)
    ASSERT_EQ(std::size_t{3}, body.at("messages").size());

    const nlohmann::json& turn = body.at("messages").at(1);
    EXPECT_EQ("assistant", turn.at("role"));
    ASSERT_EQ(std::size_t{2}, turn.at("content").size());
    EXPECT_EQ("text", turn.at("content").at(0).at("type"));
    EXPECT_EQ("checking", turn.at("content").at(0).at("text"));
    EXPECT_EQ("tool_use", turn.at("content").at(1).at("type"));
    EXPECT_EQ("toolu_1", turn.at("content").at(1).at("id"));
    EXPECT_EQ("read", turn.at("content").at(1).at("name"));
    EXPECT_EQ("a.txt", turn.at("content").at(1).at("input").at("path"));

    const nlohmann::json& result = body.at("messages").at(2);
    EXPECT_EQ("user", result.at("role"));
    ASSERT_EQ(std::size_t{1}, result.at("content").size());
    EXPECT_EQ("tool_result", result.at("content").at(0).at("type"));
    EXPECT_EQ("toolu_1", result.at("content").at(0).at("tool_use_id"));
    EXPECT_EQ("file body", result.at("content").at(0).at("content"));
}
