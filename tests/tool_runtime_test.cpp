#include "my_agent/provider/request.hpp"
#include "my_agent/runtime/tool_replay.hpp"
#include "my_agent/tool/tool_protocol.hpp"
#include "my_agent/tool/tool_runtime.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using my_agent::ToolReplayMode;
using my_agent::ToolReplayRule;
using my_agent::ToolSpec;
using my_agent::tool::Effect;
using my_agent::tool::EffectSet;
using my_agent::tool::ExecResult;
using my_agent::tool::RegisteredTool;
using my_agent::tool::ToolDescriptor;
using my_agent::tool::ToolExecutionContext;
using my_agent::tool::ToolName;
using my_agent::tool::ToolOutput;
using my_agent::tool::ToolResolutionError;
using my_agent::tool::ToolRuntime;

nlohmann::json text_schema()
{
    return nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json{
            {"text", nlohmann::json{{"type", "string"}}},
        }},
        {"required", nlohmann::json::array({"text"})},
        {"additionalProperties", false},
    };
}

ExecResult echo_handler(
    const nlohmann::json& args,
    const ToolExecutionContext&)
{
    return ToolOutput{.text = args["text"].get<std::string>()};
}

ExecResult upper_handler(
    const nlohmann::json& args,
    const ToolExecutionContext&)
{
    std::string text = args["text"].get<std::string>();
    for (char& character : text) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return ToolOutput{.text = std::move(text)};
}

RegisteredTool echo_registration()
{
    return RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "echo"},
            .description = "Echo the text argument back.",
            .input_schema = text_schema(),
            .effects = EffectSet{Effect::ReadFs},
            .replay = ToolReplayRule{
                .mode = ToolReplayMode::Full,
                .weight = 3,
            },
        },
        .handler = echo_handler,
    };
}

RegisteredTool upper_registration()
{
    return RegisteredTool{
        .descriptor = ToolDescriptor{
            .name = ToolName{.value = "upper"},
            .description = "Uppercase the text argument.",
            .input_schema = text_schema(),
            .effects = EffectSet{},
            .replay = ToolReplayRule{
                .mode = ToolReplayMode::Ephemeral,
                .weight = 1,
            },
        },
        .handler = upper_handler,
    };
}

class ToolRuntimeTest : public ::testing::Test {
protected:
    ToolRuntime runtime_{std::vector<RegisteredTool>{
        echo_registration(),
        upper_registration(),
    }};
};

TEST_F(ToolRuntimeTest, ProjectionPreservesRegistrationOrderAndDescriptorMetadata)
{
    const std::vector<ToolSpec> projected = runtime_.request_tools();

    ASSERT_EQ(std::size_t{2}, projected.size());

    EXPECT_EQ("echo", projected[0].name);
    EXPECT_EQ("Echo the text argument back.", projected[0].description);
    EXPECT_EQ(text_schema(), projected[0].input_schema);
    EXPECT_EQ(ToolReplayMode::Full, projected[0].replay_mode);
    EXPECT_EQ(std::uint32_t{3}, projected[0].replay_weight);

    EXPECT_EQ("upper", projected[1].name);
    EXPECT_EQ("Uppercase the text argument.", projected[1].description);
    EXPECT_EQ(text_schema(), projected[1].input_schema);
    EXPECT_EQ(ToolReplayMode::Ephemeral, projected[1].replay_mode);
    EXPECT_EQ(std::uint32_t{1}, projected[1].replay_weight);
}

TEST_F(ToolRuntimeTest, ProjectionReturnsOwnedValues)
{
    std::vector<ToolSpec> tampered = runtime_.request_tools();
    ASSERT_EQ(std::size_t{2}, tampered.size());

    tampered[0].name = "tampered";
    tampered[0].description.clear();
    tampered[0].input_schema["type"] = "array";
    tampered[0].replay_weight = 99;

    const std::vector<ToolSpec> fresh = runtime_.request_tools();

    ASSERT_EQ(std::size_t{2}, fresh.size());
    EXPECT_EQ("echo", fresh[0].name);
    EXPECT_EQ("Echo the text argument back.", fresh[0].description);
    EXPECT_EQ(text_schema(), fresh[0].input_schema);
    EXPECT_EQ(ToolReplayMode::Full, fresh[0].replay_mode);
    EXPECT_EQ(std::uint32_t{3}, fresh[0].replay_weight);
}

TEST_F(ToolRuntimeTest, ExactResolutionReturnsStableNonZeroReference)
{
    const auto echo = runtime_.resolve("echo");
    ASSERT_TRUE(echo.has_value());
    EXPECT_NE(std::uint64_t{0}, echo->ref.value);
    EXPECT_TRUE(echo->effects.has(Effect::ReadFs));
    EXPECT_FALSE(echo->effects.has(Effect::WriteFs));

    const auto echo_again = runtime_.resolve("echo");
    ASSERT_TRUE(echo_again.has_value());
    EXPECT_EQ(echo->ref, echo_again->ref);

    const auto upper = runtime_.resolve("upper");
    ASSERT_TRUE(upper.has_value());
    EXPECT_NE(std::uint64_t{0}, upper->ref.value);
    EXPECT_NE(echo->ref, upper->ref);
    EXPECT_FALSE(upper->effects.has(Effect::ReadFs));
    EXPECT_FALSE(upper->effects.has(Effect::WriteFs));
}

TEST_F(ToolRuntimeTest, ExecutionByReferenceReturnsHandlerOutput)
{
    const auto echo = runtime_.resolve("echo");
    ASSERT_TRUE(echo.has_value());
    const auto upper = runtime_.resolve("upper");
    ASSERT_TRUE(upper.has_value());

    const ExecResult echoed = runtime_.execute(
        echo->ref,
        nlohmann::json{{"text", "hello"}}
    );
    ASSERT_TRUE(echoed.has_value());
    EXPECT_EQ("hello", echoed->text);

    const ExecResult uppercased = runtime_.execute(
        upper->ref,
        nlohmann::json{{"text", "hello"}}
    );
    ASSERT_TRUE(uppercased.has_value());
    EXPECT_EQ("HELLO", uppercased->text);
}

TEST_F(ToolRuntimeTest, UnknownNameIsATypedResolutionErrorListingAvailableTools)
{
    const auto missing = runtime_.resolve("nope");

    static_assert(std::is_same_v<
        typename decltype(missing)::error_type,
        ToolResolutionError>);

    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ("nope", missing.error().name);
    EXPECT_EQ(
        "unknown tool: nope; available tools: echo, upper",
        missing.error().message
    );
}

TEST_F(ToolRuntimeTest, ResolutionAppliesNoAliasCaseFoldingOrTrimming)
{
    const std::vector<std::string> rejected{
        "Echo",
        "ECHO",
        " echo",
        "echo ",
        "echoo",
    };

    for (const std::string name : rejected) {
        const auto missing = runtime_.resolve(name);
        ASSERT_FALSE(missing.has_value()) << name;
        EXPECT_EQ(name, missing.error().name) << name;
    }
}

}  // namespace
