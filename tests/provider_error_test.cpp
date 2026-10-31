#include "my_agent/provider/error.hpp"

#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct ContextLimitFixture {
    std::string_view provider;
    int status;
    std::string_view payload;
};

}  // namespace

TEST(ProviderErrorTest, NormalizesContextLimitHttpFixturesAcrossProviders)
{
    const std::vector<ContextLimitFixture> fixtures = {
        {
            "openai",
            400,
            R"({"error":{"message":"maximum context length is 4096","type":"invalid_request_error","code":"context_length_exceeded"}})",
        },
        {
            "anthropic",
            400,
            R"({"type":"error","error":{"type":"invalid_request_error","message":"prompt is too long for this model"}})",
        },
        {
            "ollama",
            400,
            R"({"error":"the request exceeds the available context size"})",
        },
    };

    for (const ContextLimitFixture& fixture : fixtures) {
        const my_agent::provider::NormalizedProviderError normalized =
            my_agent::provider::normalize_error(
                fixture.provider, fixture.status, fixture.payload);

        EXPECT_EQ(
            my_agent::ProviderErrorKind::ContextLimit,
            normalized.metadata.kind
        ) << fixture.provider;
        EXPECT_EQ(fixture.status, normalized.metadata.http_status)
            << fixture.provider;
        EXPECT_FALSE(normalized.message.empty()) << fixture.provider;
    }
}

TEST(ProviderErrorTest, DoesNotMisclassifyOtherProviderFailures)
{
    struct Fixture {
        std::string_view provider;
        int status;
        std::string_view payload;
    };

    const std::vector<Fixture> fixtures = {
        {
            "openai",
            401,
            R"({"error":{"message":"invalid api key","type":"invalid_request_error","code":"invalid_api_key"}})",
        },
        {
            "anthropic",
            529,
            R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})",
        },
        {
            "ollama",
            404,
            R"({"error":"model 'missing' not found"})",
        },
        {
            "openai",
            400,
            R"({"error":{"message":"unsupported parameter","type":"invalid_request_error","code":"invalid_parameter"}})",
        },
        {"openai", 0, "connection refused"},
    };

    for (const Fixture& fixture : fixtures) {
        const my_agent::provider::NormalizedProviderError normalized =
            my_agent::provider::normalize_error(
                fixture.provider, fixture.status, fixture.payload);

        EXPECT_EQ(
            my_agent::ProviderErrorKind::Unknown,
            normalized.metadata.kind
        ) << fixture.provider << ": " << fixture.payload;
    }
}
