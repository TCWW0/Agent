#pragma once

#include "my_agent/http/http_client.hpp"
#include "my_agent/provider/provider.hpp"

#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace my_agent::provider::ollama {

class StreamStats {
public:
    struct Snapshot {
        bool available{false};
        std::uint64_t prompt_eval_count{0};
        std::uint64_t eval_count{0};
        double eval_duration_seconds{0.0};
        double tokens_per_second{0.0};
        std::uint64_t generation{0};
    };

    void record(
        std::uint64_t prompt_eval_count,
        std::uint64_t eval_count,
        std::uint64_t eval_duration_nanoseconds
    ) noexcept;

    [[nodiscard]]
    Snapshot snapshot() const noexcept;

private:
    std::atomic<bool> available_{false};
    std::atomic<std::uint64_t> prompt_eval_count_{0};
    std::atomic<std::uint64_t> eval_count_{0};
    std::atomic<std::uint64_t> eval_duration_nanoseconds_{0};
    std::atomic<std::uint64_t> generation_{0};
};

// Ollama /api/chat 的响应是 NDJSON：一行一个 JSON 帧。HTTP 层交上来的是任意
// 字节切片，一行可能跨多个切片，也可能一个切片里有好几行 —— 所以需要一个跨
// 切片保留残余的行装配器。
//
// 这是整条传输链路上唯一的有状态部分。单独成类而不是藏在闭包里，是为了能不联网
// 直接喂人为切碎的字节流做单元测试：分帧 + 帧到 Msg 的映射才是真正容易出错的
// 地方，而它与 HTTP 无关。
class StreamDecoder {
public:
    explicit StreamDecoder(std::shared_ptr<StreamStats> stats = {});

    // 消费一个字节切片，对其中每个完整行投出对应的 Msg。
    void feed(std::string_view chunk, const ProviderEventSink& sink);

private:
    void process_line(std::string_view line, const ProviderEventSink& sink);

    std::string line_buf_;
    std::shared_ptr<StreamStats> stats_;
};

// 把 Thread 历史与工具规格编成 Ollama 的请求体。
[[nodiscard]]
std::string build_request_body(
    std::string_view model,
    const Request& request,
    std::optional<std::size_t> num_ctx = {});

// 组装成 StreamEffect：宿主把它放到共享池的 worker 上跑。
[[nodiscard]]
StreamEffect make_stream(
    std::string host,
    int port,
    std::string model,
    std::shared_ptr<http::HttpClient> client,
    std::shared_ptr<StreamStats> stats = {},
    std::optional<std::size_t> num_ctx = {}
);

}  // namespace my_agent::provider::ollama
