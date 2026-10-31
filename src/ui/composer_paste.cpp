#include "my_agent/ui/composer_paste.hpp"

#include <charconv>
#include <limits>

namespace my_agent::ui {
namespace {

constexpr char kSentinel = '\x01';
constexpr std::string_view kPrefix = "PASTE:";

[[nodiscard]]
bool has_prefix(std::string_view text, std::size_t position) noexcept
{
    return position < text.size()
        && text[position] == kSentinel
        && text.substr(position + 1).starts_with(kPrefix);
}

[[nodiscard]]
std::size_t closing_sentinel(
    std::string_view text, std::size_t position) noexcept
{
    if (!has_prefix(text, position)) {
        return std::string_view::npos;
    }
    const std::size_t digits = position + 1 + kPrefix.size();
    const std::size_t end = text.find(kSentinel, digits);
    if (end == std::string_view::npos || end == digits) {
        return std::string_view::npos;
    }
    for (std::size_t index = digits; index < end; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return std::string_view::npos;
        }
    }
    return end;
}

}  // namespace

std::string make_paste_placeholder(std::size_t index)
{
    return std::string(1, kSentinel)
        + std::string{kPrefix}
        + std::to_string(index)
        + std::string(1, kSentinel);
}

std::size_t paste_placeholder_len_at(
    std::string_view text, std::size_t position) noexcept
{
    const std::size_t end = closing_sentinel(text, position);
    return end == std::string_view::npos
        ? 0
        : end - position + 1;
}

std::size_t paste_placeholder_len_ending_at(
    std::string_view text, std::size_t position) noexcept
{
    if (position == 0 || position > text.size()
        || text[position - 1] != kSentinel) {
        return 0;
    }

    const std::size_t candidate = position > 1
        ? text.rfind(kSentinel, position - 2)
        : std::string_view::npos;
    if (candidate != std::string_view::npos) {
        const std::size_t length = paste_placeholder_len_at(text, candidate);
        if (length != 0 && candidate + length == position) {
            return length;
        }
    }
    return 0;
}

std::size_t paste_placeholder_index(
    std::string_view text, std::size_t position) noexcept
{
    const std::size_t length = paste_placeholder_len_at(text, position);
    if (length == 0) {
        return std::numeric_limits<std::size_t>::max();
    }

    const std::size_t begin = position + 1 + kPrefix.size();
    const std::string_view digits = text.substr(
        begin, length - (begin - position) - 1);
    std::size_t index = 0;
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), index);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return index;
}

std::string paste_label(const PendingPaste& paste)
{
    if (paste.line_count > 1) {
        return "[Pasted text · " + std::to_string(paste.line_count)
            + " lines · " + std::to_string(paste.byte_count) + " B]";
    }

    std::string preview;
    constexpr std::size_t kPreviewMax = 50;
    bool truncated = false;
    for (std::size_t offset = 0; offset < paste.body.size();) {
        const unsigned char byte = static_cast<unsigned char>(
            paste.body[offset]);
        if (byte == '\r' || byte == '\n' || byte == '\t') {
            if (preview.empty() || preview.back() != ' ') {
                preview.push_back(' ');
            }
            ++offset;
            continue;
        }
        if (byte < 0x20) {
            ++offset;
            continue;
        }

        std::size_t width = 1;
        if ((byte & 0xe0u) == 0xc0u) {
            width = 2;
        } else if ((byte & 0xf0u) == 0xe0u) {
            width = 3;
        } else if ((byte & 0xf8u) == 0xf0u) {
            width = 4;
        }
        if (offset + width > paste.body.size()
            || preview.size() + width > kPreviewMax) {
            truncated = true;
            break;
        }
        preview.append(paste.body, offset, width);
        offset += width;
    }
    if (truncated) {
        preview += "…";
    }
    while (!preview.empty() && preview.back() == ' ') {
        preview.pop_back();
    }
    if (preview.empty()) {
        return "[Pasted text · " + std::to_string(paste.byte_count) + " B]";
    }
    return "[Pasted: " + preview + "]";
}

ComposerDisplayText render_paste_placeholders(
    std::string_view buffer,
    std::size_t cursor,
    const std::vector<PendingPaste>& pastes)
{
    if (cursor > buffer.size()) {
        cursor = buffer.size();
    }

    ComposerDisplayText result;
    result.text.reserve(buffer.size());
    std::size_t position = 0;
    while (position < buffer.size()) {
        if (position == cursor) {
            result.cursor = result.text.size();
        }

        const std::size_t length = paste_placeholder_len_at(buffer, position);
        if (length == 0) {
            result.text.push_back(buffer[position]);
            ++position;
            continue;
        }

        const std::size_t index = paste_placeholder_index(buffer, position);
        result.text += index < pastes.size()
            ? paste_label(pastes[index])
            : "[Pasted text]";
        position += length;
        if (cursor == position) {
            result.cursor = result.text.size();
        }
    }
    if (cursor >= buffer.size()) {
        result.cursor = result.text.size();
    }
    return result;
}

std::string expand_paste_placeholders(
    std::string_view buffer,
    const std::vector<PendingPaste>& pastes)
{
    std::string result;
    result.reserve(buffer.size());
    std::size_t position = 0;
    while (position < buffer.size()) {
        const std::size_t length = paste_placeholder_len_at(buffer, position);
        if (length == 0) {
            result.push_back(buffer[position++]);
            continue;
        }

        const std::size_t index = paste_placeholder_index(buffer, position);
        if (index < pastes.size()) {
            result += pastes[index].body;
        }
        position += length;
    }
    return result;
}

}  // namespace my_agent::ui
