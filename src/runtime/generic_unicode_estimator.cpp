#include "my_agent/runtime/generic_unicode_estimator.hpp"

#include <cstddef>
#include <limits>

namespace my_agent {
namespace {

void add_saturating(std::uint64_t& total, std::uint64_t value)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (value > max - total) {
        total = max;
        return;
    }
    total += value;
}

struct DecodedCodePoint {
    std::uint32_t value{0};
    std::size_t width{0};
};

bool is_continuation(unsigned char byte) noexcept
{
    return (byte & 0xc0U) == 0x80U;
}

bool decode(
    std::string_view text,
    std::size_t index,
    DecodedCodePoint& decoded) noexcept
{
    const auto first = static_cast<unsigned char>(text[index]);
    if (first < 0x80U) {
        decoded = DecodedCodePoint{.value = first, .width = 1};
        return true;
    }

    if (first >= 0xc2U && first <= 0xdfU
        && index + 2 <= text.size()) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        if (is_continuation(second)) {
            decoded = DecodedCodePoint{
                .value = static_cast<std::uint32_t>(first & 0x1fU) << 6
                    | (second & 0x3fU),
                .width = 2,
            };
            return true;
        }
    }

    if (first >= 0xe0U && first <= 0xefU
        && index + 3 <= text.size()) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        const auto third = static_cast<unsigned char>(text[index + 2]);
        const bool boundary_ok = first != 0xe0U || second >= 0xa0U;
        const bool surrogate_ok = first != 0xedu || second < 0xa0U;
        if (boundary_ok && surrogate_ok
            && is_continuation(second) && is_continuation(third)) {
            decoded = DecodedCodePoint{
                .value = static_cast<std::uint32_t>(first & 0x0fU) << 12
                    | static_cast<std::uint32_t>(second & 0x3fU) << 6
                    | (third & 0x3fU),
                .width = 3,
            };
            return true;
        }
    }

    if (first >= 0xf0U && first <= 0xf4U
        && index + 4 <= text.size()) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        const auto third = static_cast<unsigned char>(text[index + 2]);
        const auto fourth = static_cast<unsigned char>(text[index + 3]);
        const bool boundary_ok = first != 0xf0U || second >= 0x90U;
        const bool max_ok = first != 0xf4U || second <= 0x8fU;
        if (boundary_ok && max_ok
            && is_continuation(second)
            && is_continuation(third)
            && is_continuation(fourth)) {
            decoded = DecodedCodePoint{
                .value = static_cast<std::uint32_t>(first & 0x07U) << 18
                    | static_cast<std::uint32_t>(second & 0x3fU) << 12
                    | static_cast<std::uint32_t>(third & 0x3fU) << 6
                    | (fourth & 0x3fU),
                .width = 4,
            };
            return true;
        }
    }
    return false;
}

bool is_ascii_alphanumeric(std::uint32_t code_point) noexcept
{
    return (code_point >= 'a' && code_point <= 'z')
        || (code_point >= 'A' && code_point <= 'Z')
        || (code_point >= '0' && code_point <= '9');
}

bool is_cjk(std::uint32_t code_point) noexcept
{
    return (code_point >= 0x3400U && code_point <= 0x4dbfU)
        || (code_point >= 0x4e00U && code_point <= 0x9fffU)
        || (code_point >= 0xf900U && code_point <= 0xfaffU)
        || (code_point >= 0x3040U && code_point <= 0x30ffU)
        || (code_point >= 0xac00U && code_point <= 0xd7afU);
}

bool is_emoji(std::uint32_t code_point) noexcept
{
    return (code_point >= 0x1f000U && code_point <= 0x1faffU)
        || (code_point >= 0x2600U && code_point <= 0x27bfU);
}

bool is_hex(unsigned char byte) noexcept
{
    return (byte >= '0' && byte <= '9')
        || (byte >= 'a' && byte <= 'f')
        || (byte >= 'A' && byte <= 'F');
}

bool is_base64_byte(unsigned char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z')
        || (byte >= 'A' && byte <= 'Z')
        || (byte >= '0' && byte <= '9')
        || byte == '+' || byte == '/' || byte == '=';
}

bool has_no_whitespace(std::string_view text) noexcept
{
    for (const char byte : text) {
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            return false;
        }
    }
    return true;
}

bool is_uuid(std::string_view text) noexcept
{
    if (text.size() != 36) {
        return false;
    }
    constexpr std::size_t hyphens[] = {8, 13, 18, 23};
    for (std::size_t index = 0; index < text.size(); ++index) {
        bool should_be_hyphen = false;
        for (const std::size_t position : hyphens) {
            should_be_hyphen = should_be_hyphen || index == position;
        }
        if (should_be_hyphen) {
            if (text[index] != '-') {
                return false;
            }
        } else if (!is_hex(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    return true;
}

bool is_dense_text(std::string_view text) noexcept
{
    if (text.size() < 8 || !has_no_whitespace(text)) {
        return false;
    }
    if (is_uuid(text)) {
        return true;
    }

    bool all_hex = text.size() >= 32;
    bool all_base64 = text.size() >= 24;
    bool long_unseparated = text.size() >= 24;
    for (const unsigned char byte : text) {
        all_hex = all_hex && is_hex(byte);
        all_base64 = all_base64 && is_base64_byte(byte);
        long_unseparated = long_unseparated
            && ((byte >= 'a' && byte <= 'z')
                || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9')
                || byte == '_' || byte == '-');
    }
    if (all_hex || all_base64 || long_unseparated) {
        return true;
    }

    const bool object = text.front() == '{' && text.back() == '}';
    const bool array = text.front() == '[' && text.back() == ']';
    return (object || array) && text.size() >= 12;
}

}  // namespace

TextEstimate GenericUnicodeV1::estimate(std::string_view text) const
{
    TextEstimate result;
    for (std::size_t index = 0; index < text.size();) {
        DecodedCodePoint decoded;
        if (!decode(text, index, decoded)) {
            result.valid_utf8 = false;
            result.tokens = static_cast<std::uint64_t>(text.size());
            return result;
        }
        index += decoded.width;
    }

    if (is_dense_text(text)) {
        result.dense_text = true;
        result.tokens = static_cast<std::uint64_t>(text.size() / 2
            + text.size() % 2);
        return result;
    }

    for (std::size_t index = 0; index < text.size();) {
        DecodedCodePoint decoded;
        static_cast<void>(decode(text, index, decoded));
        if (is_ascii_alphanumeric(decoded.value)) {
            std::size_t run_end = index + decoded.width;
            while (run_end < text.size()) {
                DecodedCodePoint next;
                static_cast<void>(decode(text, run_end, next));
                if (!is_ascii_alphanumeric(next.value)) {
                    break;
                }
                run_end += next.width;
            }
            const auto run_bytes = run_end - index;
            add_saturating(result.tokens,
                static_cast<std::uint64_t>((run_bytes + 3) / 4));
            index = run_end;
            continue;
        }

        if (decoded.value < 0x80U) {
            add_saturating(result.tokens, 1);
        } else if (is_cjk(decoded.value)) {
            add_saturating(result.tokens, 1);
        } else if (is_emoji(decoded.value)) {
            add_saturating(result.tokens, 2);
        } else {
            add_saturating(result.tokens, 2);
        }
        index += decoded.width;
    }
    return result;
}

}  // namespace my_agent
