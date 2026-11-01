#include "context_projection_test_support.hpp"
#include "my_agent/runtime/context_projection.hpp"
#include "my_agent/provider/anthropic.hpp"
#include "my_agent/provider/ollama.hpp"
#include "my_agent/provider/openai.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool valid_utf8(std::string_view text)
{
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t width = 1;
        if ((lead & 0x80U) == 0) {
            width = 1;
        } else if ((lead & 0xe0U) == 0xc0U) {
            width = 2;
        } else if ((lead & 0xf0U) == 0xe0U) {
            width = 3;
        } else if ((lead & 0xf8U) == 0xf0U) {
            width = 4;
        } else {
            return false;
        }
        if (index + width > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xc0U)
                != 0x80U) {
                return false;
            }
        }
        index += width;
    }
    return true;
}

}  // namespace

TEST(ToolReplayProjectionTest, TinyBudgetStillProducesAnAuditableBoundedView)
{
    const my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {
                    my_agent::ToolCall{
                        .id = "tiny",
                        .name = "read",
                        .args = nlohmann::json::object(),
                        .status = my_agent::ToolCall::Done{
                            .output = std::string(4096, 'q'),
                        },
                    },
                },
            },
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "read",
                .replay_mode = my_agent::ToolReplayMode::HeadTail,
            },
        },
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 1.0,
        .floor = 16,
        .ceiling = 16,
        .recent_item_cap = 16,
        .stale_item_cap = 16,
        .recent_count = 1,
    };

    const my_agent::ContextProjection projection = my_agent_test::project_context(
        "openai", "model-a", request,
        my_agent::ContextProjectionLedger{}, policy, 64);

    const auto& calls = projection.request.messages.front().tool_calls;
    ASSERT_EQ(std::size_t{1}, calls.size());
    const std::string& output = calls.front().output();
    EXPECT_NE(output.find("[elided:b=4096:d="), std::string::npos);
    EXPECT_LT(output.size(), 4096u);
    EXPECT_LE(my_agent::GenericUnicodeV1{}.estimate(output).tokens, 16u);

}

TEST(ToolReplayProjectionTest, ProviderBodiesUseTheCanonicalBoundedContent)
{
    my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {
                    my_agent::ToolCall{
                        .id = "call-1",
                        .name = "read",
                        .args = nlohmann::json::object(),
                        .status = my_agent::ToolCall::Done{
                            .output = std::string(5000, 'z'),
                        },
                    },
                },
            },
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "read",
                .replay_mode = my_agent::ToolReplayMode::HeadTail,
            },
        },
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 1.0,
        .floor = 64,
        .ceiling = 64,
        .recent_item_cap = 64,
        .stale_item_cap = 64,
        .recent_count = 1,
    };
    const my_agent::ContextProjection projection = my_agent_test::project_context(
        "provider", "model-a", request,
        my_agent::ContextProjectionLedger{}, policy, 128);
    const std::string& bounded = projection.request.messages.front().tool_calls.front().output();

    const std::string openai = my_agent::provider::openai::build_request_body(
        "model-a", projection.request);
    const std::string anthropic =
        my_agent::provider::anthropic::build_request_body(
            "model-a", projection.request);
    const std::string ollama = my_agent::provider::ollama::build_request_body(
        "model-a", projection.request);

    for (const std::string& body : {openai, anthropic, ollama}) {
        EXPECT_NE(body.find(bounded), std::string::npos);
        EXPECT_EQ(body.find(std::string(5000, 'z')), std::string::npos);
    }
}

TEST(ToolReplayProjectionTest, PreservesSmallResultsAndIsDeterministic)
{
    const auto make_request = [] {
        return my_agent::Request{
            .messages = {
                my_agent::Message{
                    .role = my_agent::Role::Assistant,
                    .tool_calls = {
                        my_agent::ToolCall{
                            .id = "full",
                            .name = "small",
                            .args = nlohmann::json::object(),
                            .status = my_agent::ToolCall::Done{
                                .output = "byte-identical result",
                            },
                        },
                    },
                },
            },
            .tools = {
                my_agent::ToolSpec{
                    .name = "small",
                    .replay_mode = my_agent::ToolReplayMode::Full,
                    .replay_weight = 2,
                },
            },
        };
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 0.5,
        .floor = 64,
        .ceiling = 64,
        .recent_item_cap = 64,
        .stale_item_cap = 16,
        .recent_count = 1,
    };

    const my_agent::ContextProjection first = my_agent_test::project_context(
        "ollama", "model-a", make_request(),
        my_agent::ContextProjectionLedger{}, policy, 128);
    const my_agent::ContextProjection second = my_agent_test::project_context(
        "ollama", "model-a", make_request(),
        my_agent::ContextProjectionLedger{}, policy, 128);

    const auto& first_calls = first.request.messages.front().tool_calls;
    ASSERT_EQ(std::size_t{1}, first_calls.size());
    EXPECT_EQ("byte-identical result", first_calls.front().output());
    EXPECT_EQ(first.fingerprint, second.fingerprint);
    EXPECT_EQ(first_calls.front().output(),
              second.request.messages.front().tool_calls.front().output());

}

TEST(ToolReplayProjectionTest, EphemeralStaleResultsAreNotReplayed)
{
    const auto call = [](std::string id, std::uint64_t turn) {
        return my_agent::ToolCall{
            .id = std::move(id),
            .name = "ephemeral",
            .args = nlohmann::json::object(),
            .status = my_agent::ToolCall::Done{.output = "tool output"},
        };
    };
    const my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {call("old", 1)},
                .turn_id = my_agent::TurnId{1},
            },
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {call("new", 2)},
                .turn_id = my_agent::TurnId{2},
            },
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "ephemeral",
                .replay_mode = my_agent::ToolReplayMode::Ephemeral,
                .replay_weight = 1,
            },
        },
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 1.0,
        .floor = 64,
        .ceiling = 64,
        .recent_item_cap = 64,
        .stale_item_cap = 16,
        .recent_count = 1,
    };

    const my_agent::ContextProjection projection = my_agent_test::project_context(
        "anthropic", "model-a", request,
        my_agent::ContextProjectionLedger{}, policy, 64);

    ASSERT_EQ(std::size_t{2}, projection.request.messages.size());
    EXPECT_TRUE(projection.request.messages.front().tool_calls.front().output().empty());
    EXPECT_EQ("tool output",
              projection.request.messages.back().tool_calls.front().output());
}

TEST(ToolReplayProjectionTest, SharesBudgetAndPrioritizesRecentResults)
{
    const auto result = [](std::string id, std::uint64_t turn) {
        return my_agent::ToolCall{
            .id = std::move(id),
            .name = "read",
            .args = nlohmann::json::object(),
            .status = my_agent::ToolCall::Done{
                .output = std::string(9000, 'x'),
            },
        };
    };
    const my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {result("old", 1)},
                .turn_id = my_agent::TurnId{1},
            },
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {result("middle", 2)},
                .turn_id = my_agent::TurnId{2},
            },
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {result("new", 3)},
                .turn_id = my_agent::TurnId{3},
            },
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "read",
                .replay_mode = my_agent::ToolReplayMode::HeadTail,
                .replay_weight = 1,
            },
        },
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 0.25,
        .floor = 1,
        .ceiling = 10000,
        .recent_item_cap = 1200,
        .stale_item_cap = 300,
        .recent_count = 1,
    };

    const my_agent::ContextProjection at_8k = my_agent_test::project_context(
        "openai", "model-a", request, my_agent::ContextProjectionLedger{},
        policy, 8192);
    const my_agent::ContextProjection at_32k = my_agent_test::project_context(
        "openai", "model-a", request, my_agent::ContextProjectionLedger{},
        policy, 32768);

    ASSERT_EQ(std::size_t{3}, at_8k.request.messages.size());
    EXPECT_GT(at_8k.request.messages.back().tool_calls.front().output().size(),
              at_8k.request.messages.front().tool_calls.front().output().size());
    for (const auto* projection : {&at_8k, &at_32k}) {
        std::uint64_t tokens = 0;
        for (const auto& message : projection->request.messages) {
            tokens += my_agent::GenericUnicodeV1{}.estimate(
                message.tool_calls.front().output()).tokens;
        }
        EXPECT_LE(tokens, projection == &at_8k ? 2048u : 8192u);
    }

    // The projection is wire-only. The source request remains a complete
    // transcript-shaped value for the caller.
    EXPECT_EQ(9000u, request.messages.front().tool_calls.front().output().size());
    EXPECT_LT(
        at_8k.request.messages.front().tool_calls.front().output().size(),
        request.messages.front().tool_calls.front().output().size());
}

TEST(ToolReplayPolicyTest, ValidatesBoundsAndScalesBudgetMonotonically)
{
    const my_agent::ToolReplayPolicy policy{
        .fraction = 0.25,
        .floor = 64,
        .ceiling = 4096,
        .recent_item_cap = 1024,
        .stale_item_cap = 256,
        .recent_count = 2,
    };

    EXPECT_NO_THROW(policy.validate());
    EXPECT_EQ(64u, policy.budget_for_context(128));
    EXPECT_EQ(2048u, policy.budget_for_context(8192));
    EXPECT_EQ(4096u, policy.budget_for_context(32768));
    EXPECT_LE(policy.budget_for_context(8192),
              policy.budget_for_context(32768));

    auto invalid_fraction = policy;
    invalid_fraction.fraction = 1.1;
    EXPECT_THROW(invalid_fraction.validate(), std::invalid_argument);

    auto invalid_range = policy;
    invalid_range.floor = 5000;
    EXPECT_THROW(invalid_range.validate(), std::invalid_argument);

    auto invalid_rule = my_agent::ToolReplayRule{};
    invalid_rule.weight = 0;
    EXPECT_THROW(invalid_rule.validate(), std::invalid_argument);
}

TEST(ToolReplayProjectionTest, BoundsHeadTailAtUtf8CodePointBoundaries)
{
    const my_agent::Request request{
        .messages = {
            my_agent::Message{
                .role = my_agent::Role::Assistant,
                .tool_calls = {
                    my_agent::ToolCall{
                        .id = "call-1",
                        .name = "read",
                        .args = nlohmann::json::object(),
                        .status = my_agent::ToolCall::Done{
                            .output =
                                "头部🙂-0123456789-尾部😀-0123456789-"
                                "中间内容中间内容中间内容中间内容-尾端😀",
                        },
                    },
                },
            },
        },
        .tools = {
            my_agent::ToolSpec{
                .name = "read",
                .replay_mode = my_agent::ToolReplayMode::HeadTail,
                .replay_weight = 1,
            },
        },
    };
    const my_agent::ToolReplayPolicy policy{
        .fraction = 1.0,
        .floor = 32,
        .ceiling = 32,
        .recent_item_cap = 32,
        .stale_item_cap = 4,
        .recent_count = 1,
    };

    const my_agent::ContextProjection projection =
        my_agent_test::project_context(
            "openai",
            "model-a",
            request,
            my_agent::ContextProjectionLedger{},
            policy,
            32);

    const std::string& wire_result =
        projection.request.messages.front().tool_calls.front().output();
    EXPECT_NE(wire_result.find("elided"), std::string::npos);
    EXPECT_NE(wire_result.find("头"), std::string::npos);
    EXPECT_NE(wire_result.find("😀"), std::string::npos);
    EXPECT_TRUE(valid_utf8(wire_result));
}
