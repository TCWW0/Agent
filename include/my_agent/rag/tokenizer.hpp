#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace my_agent::rag {

// The single lexical-analysis seam shared by BM25, quality probes and the
// local reranker. Tokens are returned in source order and duplicates are
// preserved. See docs/specs/RAG-TECHNICAL-TOKENIZER.md for the versioned
// behavior contract.
[[nodiscard]] std::vector<std::string> tokenize(std::string_view text);

// Stable experiment identity for the behavior behind tokenize(). Any change
// that can alter emitted tokens must change this value.
[[nodiscard]] std::string_view tokenizer_identity() noexcept;

} // namespace my_agent::rag
