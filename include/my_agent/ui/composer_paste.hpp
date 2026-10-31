#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::ui {

// The full body stays in the UI draft until submit. The composer buffer only
// contains a compact sentinel, so a large paste cannot suddenly grow the
// rendered input area.
struct PendingPaste {
    std::string body;
    std::size_t line_count{0};
    std::size_t byte_count{0};
};

struct ComposerDisplayText {
    std::string text;
    std::size_t cursor{0};
};

[[nodiscard]]
std::string make_paste_placeholder(std::size_t index);

[[nodiscard]]
std::size_t paste_placeholder_len_at(
    std::string_view text, std::size_t position) noexcept;

[[nodiscard]]
std::size_t paste_placeholder_len_ending_at(
    std::string_view text, std::size_t position) noexcept;

[[nodiscard]]
std::size_t paste_placeholder_index(
    std::string_view text, std::size_t position) noexcept;

[[nodiscard]]
std::string paste_label(const PendingPaste& paste);

[[nodiscard]]
ComposerDisplayText render_paste_placeholders(
    std::string_view buffer,
    std::size_t cursor,
    const std::vector<PendingPaste>& pastes);

[[nodiscard]]
std::string expand_paste_placeholders(
    std::string_view buffer,
    const std::vector<PendingPaste>& pastes);

}  // namespace my_agent::ui
