#include "my_agent/rag/tokenizer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rag = my_agent::rag;

struct TokenCase {
    std::string input;
    std::vector<std::string> expected;
};

void expect_tokens(const std::vector<TokenCase>& cases) {
    for (const TokenCase& test_case : cases) {
        SCOPED_TRACE(test_case.input);
        EXPECT_EQ(rag::tokenize(test_case.input), test_case.expected);
    }
}

TEST(Tokenizer, LegacyAsciiContractIsFrozen) {
    expect_tokens({
        {"The Quick-Brown FOX", {"the", "quick", "brown", "fox"}},
        {"a I am", {"am"}},
        {"k1=1.5", {"k1"}},
        {"Beta beta alpha", {"beta", "beta", "alpha"}},
        {"!!! ... ???", {}},
        {"", {}},
    });
}

TEST(Tokenizer, LegacyNonAsciiContractIsFrozen) {
    const std::string truncated{"\xE4\xB8", 2};
    expect_tokens({
        {"检索引擎", {"检索", "索引", "引擎"}},
        {"注册表", {"注册", "册表"}},
        {"图", {"图"}},
        {"配置BM25参数", {"配置", "bm25", "参数"}},
        {"café", {"caf", "é"}},
        {truncated, {truncated}},
    });
}

TEST(Tokenizer, CompositeIdentifiersPreserveLegacyIdentityAndAddSemanticParts) {
    expect_tokens({
        {"MY_AGENT_DOCS_DIR",
         {"my", "agent", "docs", "dir"}},
        {"HybridRrfWeights",
         {"hybridrrfweights", "hybrid", "rrf", "weights"}},
        {"getHTTPResponseCode",
         {"gethttpresponsecode", "get", "http", "response", "code"}},
        {"HTTPServerError",
         {"httpservererror", "http", "server", "error"}},
        {"std::string_view",
         {"std", "string", "view"}},
        {"IconName::AlertCircle",
         {"iconname", "icon", "name", "alertcircle", "alert", "circle"}},
        {"retrieval-cache",
         {"retrieval", "cache"}},
        {"hybrid retrieval weights", {"hybrid", "retrieval", "weights"}},

        // M2-A does not enable the technical-form rules reserved for M2-B.
        {"C++", {}},
        {"v1.2", {"v1"}},
    });
}

TEST(Tokenizer, IdentityNamesTheCompositeIdentifierContract) {
    EXPECT_EQ(rag::tokenizer_identity(),
              "technical-tokenizer-v2-identifier-parts");
}

} // namespace
