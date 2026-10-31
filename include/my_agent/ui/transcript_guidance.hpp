#pragma once

#include "my_agent/runtime/model.hpp"
#include "my_agent/ui/transcript_viewport.hpp"

#include <cstdint>
#include <string>

namespace my_agent::ui {

enum class TranscriptHintKind {
    None,
    Latest,
    NewOutput,
};

// Tracks meaningful transcript content without adding UI state to Model.
class TranscriptRevisionTracker {
public:
    [[nodiscard]] std::uint64_t observe(const Model& model);

private:
    std::string signature_;
    std::uint64_t revision_{0};
    bool initialized_{false};
};

// Tracks which transcript revision was last seen at the geometric tail.
// Scroll position itself remains owned by TranscriptViewport/Painter.
class TranscriptGuidance {
public:
    void reconcile(
        const TranscriptViewport::Snapshot& snapshot,
        std::uint64_t revision) noexcept;

    [[nodiscard]] TranscriptHintKind hint(
        const TranscriptViewport::Snapshot& snapshot,
        std::uint64_t revision) const noexcept;

private:
    std::uint64_t seen_revision_{0};
};

}  // namespace my_agent::ui
