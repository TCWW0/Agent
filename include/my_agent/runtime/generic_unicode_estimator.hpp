#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace my_agent {

inline constexpr std::string_view kGenericUnicodeV1Version =
    "generic-unicode-v1";

struct TextEstimate {
    std::uint64_t tokens{0};
    std::string estimator_version{
        kGenericUnicodeV1Version,
    };
    bool dense_text{false};
    bool valid_utf8{true};
};

class GenericUnicodeV1 {
public:
    [[nodiscard]]
    TextEstimate estimate(std::string_view text) const;
};

}  // namespace my_agent
