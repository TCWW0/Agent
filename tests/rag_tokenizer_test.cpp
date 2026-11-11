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
    });
}

TEST(Tokenizer, ResourceGuardsBoundPathologicalCandidatesWithoutDroppingUsefulParts) {
    const std::string forty_bytes(40, 'a');
    const std::string forty_one_bytes(41, 'a');

    expect_tokens({
        {forty_bytes, {forty_bytes}},
        {forty_one_bytes, {}},
        {"stable_" + forty_one_bytes + "_tail", {"stable", "tail"}},
        {"test_agent_timeline_per_event_hash_id_bounds_cost",
         {"test", "agent", "timeline", "per", "event", "hash", "id",
          "bounds", "cost"}},
        {"part00_part01_part02_part03_part04_part05_part06_part07_part08_"
         "part09_part10_part11_part12_part13_part14_part15_part16_part17",
         {"part00", "part01", "part02", "part03", "part04", "part05",
          "part06", "part07", "part08", "part09", "part10", "part11",
          "part12", "part13", "part14", "part15"}},
    });
}

TEST(Tokenizer, SymbolicTechnicalAtomsPreserveExactAndLegacyRepresentations) {
    expect_tokens({
        {"C++", {"c++"}},
        {"C#", {"c#"}},
        {"C++26", {"c++26", "c++", "26"}},
        {"-k", {"-k"}},
        {"-j2", {"-j2", "j2"}},
        {"--auth-header", {"--auth-header", "auth", "header"}},
        {"v1.2", {"v1.2", "v1"}},
        {"v1.2.3-beta", {"v1.2.3-beta", "v1", "beta"}},
        {"HTTP/2", {"http/2", "http"}},
        {"HTTP/1.1", {"http/1.1", "http"}},

        // M2-B2 deliberately leaves ambiguous punctuation and paths alone.
        {"C + + result", {"result"}},
        {"- item", {"item"}},
        {"Quick-Brown", {"quick", "brown"}},
        {"k1=1.5", {"k1"}},
        {"src/io/auth.cpp", {"src", "io", "auth", "cpp"}},
    });
}

TEST(Tokenizer, IdentityNamesTheSymbolicAtomContract) {
    EXPECT_EQ(rag::tokenizer_identity(),
              "technical-tokenizer-v2-bounded-symbolic-atoms");
}

} // namespace
