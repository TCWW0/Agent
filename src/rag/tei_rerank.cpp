#include "my_agent/http/http_client.hpp"
#include "my_agent/rag/rag.hpp"
#include "nlohmann/json_fwd.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::rag {

namespace {

std::expected<std::string, std::string>
post_tei_rerank(const TeiRerankConfig& config, std::string_view request_body) {
    const http::HttpClient client;
    const auto response = client.post(
        http::HttpRequest{
            .host = config.host,
            .port = config.port,
            .path = "/rerank",
            .headers = {
                {"Content-Type", "application/json"},
            },
            .body = std::string{request_body},
            .use_tls = false,
            .timeout_ms = config.timeout_ms,
        }
    );

    if (!response) {
        const http::HttpError& error = response.error();

        std::string diagnostic = "TEI rerank HTTP failed";
        if (error.status != 0) {
            diagnostic += " (status " + std::to_string(error.status) + ")";
        }
        if (!error.message.empty()) {
            diagnostic += ": " + error.message;
        }

        return std::unexpected(std::move(diagnostic));
    }

    return *response;
}

} // namespace

std::string
build_tei_rerank_request_body(std::string_view query,
                              const std::vector<std::string>& passages) {
    nlohmann::json body;
    body["query"] = std::string{query};
    body["texts"] = passages;
    return body.dump();
}

std::expected<std::vector<double>, std::string>
parse_tei_rerank_response(std::string_view body, std::size_t expected_count) {
    nlohmann::json response;

    try {
        response = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected(
            std::string{"TEI rerank response is not valid JSON: "}
            + error.what()
        );
    }

    if (!response.is_array()) {
        return std::unexpected("TEI rerank response must be an array");
    }

    if (response.size() != expected_count) {
        return std::unexpected(
            "TEI rerank response count mismatch: expected "
            + std::to_string(expected_count)
            + ", got "
            + std::to_string(response.size())
        );
    }


    std::vector<double> scores(expected_count);
    std::vector<bool> seen(expected_count, false);

    for (const auto& item : response) {
        if (!item.is_object()) {
            return std::unexpected("TEI rerank response item must be an object");
        }

        const auto index_it = item.find("index");
        const auto score_it = item.find("score");

        if (index_it == item.end() || !index_it->is_number_unsigned()) {
            return std::unexpected("TEI rerank response item has invalid index");
        }

        if (score_it == item.end() || !score_it->is_number()) {
            return std::unexpected("TEI rerank response has invalid score");
        }

        const std::size_t index = index_it->get<std::size_t>();
        const double score = score_it->get<double>();

        if (index >= expected_count) {
            return std::unexpected(
                "TEI rerank response index out of range: "
                + std::to_string(index));
        }

        if (seen[index]) {
            return std::unexpected(
                "TEI rerank response contains duplicate index: "
                + std::to_string(index));
        }

        if (!std::isfinite(score)) {
            return std::unexpected("TEI response contains non-finite score");
        }

        seen[index] = true;
        scores[index] = score;
    }

    return scores;
}

RerankBackend make_tei_rerank_backend(TeiRerankConfig config, TeiRerankTransport transport) {
    if (!transport) {
        transport = post_tei_rerank;
    }

    return [
        config = std::move(config),
        transport = std::move(transport)
    ](std::string_view query, const std::vector<std::string>& passages)
        -> std::expected<std::vector<double>, std::string> {
        if (passages.empty()) {
            return std::vector<double>{};
        }
        const std::string request_body = build_tei_rerank_request_body(query, passages);
        const auto response = transport(config, request_body);
        if (!response) {
            return std::unexpected(response.error());
        }

        return parse_tei_rerank_response(*response, passages.size());
    };
}

}  // namespace my_agent::rag
