#include "my_agent/provider/error.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace my_agent::provider {
namespace {

std::string lower(std::string_view value)
{
    std::string result{value};
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

}  // namespace

NormalizedProviderError normalize_error(
    std::string_view provider,
    int http_status,
    std::string_view payload)
{
    NormalizedProviderError result;
    result.metadata.http_status = http_status;

    const nlohmann::json frame = nlohmann::json::parse(payload, nullptr, false);
    if (frame.is_object()) {
        const nlohmann::json* error = &frame;
        const auto nested = frame.find("error");
        if (nested != frame.end()) {
            error = &*nested;
        }

        if (error->is_object()) {
            result.message = error->value("message", std::string{});
            result.metadata.code = error->value("code", std::string{});
            result.metadata.type = error->value("type", std::string{});
        } else if (error->is_string()) {
            result.message = error->get<std::string>();
        }
    }
    if (result.message.empty()) {
        result.message = std::string{payload};
    }

    const std::string message = lower(result.message);
    const std::string code = lower(result.metadata.code);
    const std::string type = lower(result.metadata.type);
    const std::string provider_name = lower(provider);

    const bool explicit_context_code = code == "context_length_exceeded"
        || code == "context_window_exceeded"
        || code.contains("context_length")
        || code.contains("context_window");
    const bool context_phrase = message.contains("context_length_exceeded")
        || message.contains("maximum context length")
        || message.contains("context window")
        || message.contains("context length exceeded")
        || message.contains("exceeds the context length")
        || message.contains("exceed the context length")
        || message.contains("exceeds the available context")
        || message.contains("exceed the available context")
        || message.contains("prompt is too long")
        || message.contains("prompt too long")
        || message.contains("input is too long")
        || message.contains("input too long");
    const bool provider_specific_phrase =
        (provider_name == "ollama" && message.contains("context size"))
        || (provider_name == "anthropic"
            && type == "invalid_request_error"
            && message.contains("too long"));

    if (explicit_context_code || context_phrase || provider_specific_phrase) {
        result.metadata.kind = ProviderErrorKind::ContextLimit;
    }
    return result;
}

}  // namespace my_agent::provider
