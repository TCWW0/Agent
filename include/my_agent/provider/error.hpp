#pragma once

#include "my_agent/runtime/identity.hpp"

#include <string>
#include <string_view>

namespace my_agent::provider {

struct NormalizedProviderError {
    ProviderError metadata;
    std::string message;
};

// Normalize a provider's raw error payload. `http_status` is zero for an
// error emitted inside a streaming body rather than by the HTTP layer.
[[nodiscard]]
NormalizedProviderError normalize_error(
    std::string_view provider,
    int http_status,
    std::string_view payload);

}  // namespace my_agent::provider
