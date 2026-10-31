#include "my_agent/rag/rag.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace rag = my_agent::rag;

TEST(TeiRerankSmoke, RealEndpointReturnsOneFiniteScorePerPassage)
{
    const char* host = std::getenv("MY_AGENT_TEI_SMOKE_HOST");
    if (host == nullptr || *host == '\0') {
        GTEST_SKIP() << "set MY_AGENT_TEI_SMOKE_HOST to run against a real TEI reranker";
    }

    int port = 8080;
    if (const char* text = std::getenv("MY_AGENT_TEI_SMOKE_PORT");
        text != nullptr && *text != '\0') {
        const char* end = text + std::strlen(text);
        const auto [parsed_end, error] = std::from_chars(text, end, port);
        ASSERT_EQ(error, std::errc{});
        ASSERT_EQ(parsed_end, end);
        ASSERT_GT(port, 0);
        ASSERT_LE(port, 65535);
    }

    const rag::RerankBackend backend = rag::make_tei_rerank_backend(
        rag::TeiRerankConfig{
            .host = host,
            .port = port,
            .timeout_ms = 30000,
        });

    const auto scores = backend(
        "How should the service be deployed?",
        std::vector<std::string>{
            "Run the database migrations before starting the service.",
            "Ocean tides are influenced by the moon.",
        });

    ASSERT_TRUE(scores.has_value()) << scores.error();
    ASSERT_EQ(scores->size(), 2u);
    EXPECT_TRUE(std::isfinite((*scores)[0]));
    EXPECT_TRUE(std::isfinite((*scores)[1]));
}
