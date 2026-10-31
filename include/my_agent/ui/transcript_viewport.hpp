#pragma once

#include "my_agent/ui/input.hpp"

namespace maya {
struct ScrollState;
}  // namespace maya

namespace my_agent::ui {

// Frontend-only intents for the fullscreen transcript. These never cross the
// domain boundary as runtime messages.
enum class TranscriptNavigation {
    PageUp,
    PageDown,
    Top,
    Bottom,
};

// Project-owned adapter around Painter's Maya ScrollState. It exposes only
// transcript navigation intents and a read-only geometry snapshot; follow and
// frozen behavior are always derived from the current scroll geometry.
class TranscriptViewport {
public:
    struct Snapshot {
        int offset{0};
        int maximum{0};
        int viewport_height{0};
        bool at_bottom{true};
    };

    explicit TranscriptViewport(maya::ScrollState& state) noexcept;

    void apply(TranscriptNavigation navigation) noexcept;
    bool apply_mouse(const MouseEvent& event, int scroll_rows) noexcept;

    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    maya::ScrollState* state_;
};

}  // namespace my_agent::ui
