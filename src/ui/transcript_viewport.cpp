#include "my_agent/ui/transcript_viewport.hpp"

#include <maya/core/scroll_state.hpp>

#include <algorithm>

namespace my_agent::ui {

TranscriptViewport::TranscriptViewport(maya::ScrollState& state) noexcept
    : state_{&state}
{
}

void TranscriptViewport::apply(TranscriptNavigation navigation) noexcept
{
    state_->clamp();
    switch (navigation) {
        case TranscriptNavigation::PageUp: {
            const int overlap_page = std::max(
                1, state_->viewport_bounds.h - 1);
            state_->scroll_by(0, -overlap_page);
            return;
        }
        case TranscriptNavigation::PageDown: {
            const int overlap_page = std::max(
                1, state_->viewport_bounds.h - 1);
            state_->scroll_by(0, overlap_page);
            return;
        }
        case TranscriptNavigation::Top:
            state_->scroll_to_top();
            return;
        case TranscriptNavigation::Bottom:
            state_->scroll_to_bottom();
            return;
    }
}

bool TranscriptViewport::apply_mouse(const MouseEvent& event, int scroll_rows) noexcept
{
    state_->clamp();
    if (event.kind != MouseEventKind::Press || event.modifiers.shift ||
        event.modifiers.alt) {
        return false;
    }

    const auto& bounds = state_->viewport_bounds;
    const int column = event.x - 1;
    const int row = event.y - 1;
    if (!bounds.contains(column, row)) {
        return false;
    }

    const int rows = std::max(1, scroll_rows);
    switch (event.button) {
        case MouseButton::ScrollUp:
            state_->scroll_by(0, -rows);
            return true;
        case MouseButton::ScrollDown:
            state_->scroll_by(0, rows);
            return true;
        default:
            return false;
    }
}

TranscriptViewport::Snapshot TranscriptViewport::snapshot() const noexcept
{
    const int maximum = std::max(0, state_->max_y);
    const int offset = std::clamp(state_->y, 0, maximum);
    const int viewport_height = std::max(0, state_->viewport_bounds.h);
    return Snapshot{
        .offset = offset,
        .maximum = maximum,
        .viewport_height = viewport_height,
        .at_bottom = offset == maximum,
    };
}

}  // namespace my_agent::ui
