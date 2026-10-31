#pragma once

#include "my_agent/ui/screen.hpp"
#include "my_agent/ui/size.hpp"
#include "my_agent/ui/transcript_viewport.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <variant>

#include <maya/style/theme.hpp>

namespace maya {
class FrameBuffer;
struct ScrollState;
}  // namespace maya

namespace my_agent::ui {

using WriteSink = std::function<bool(std::string_view)>;

// Owns render-side state. It never owns a file descriptor; callers provide
// the current Size and a write sink for each frame.
class Painter {
public:
    explicit Painter(maya::Theme theme = maya::theme::dark);
    ~Painter();

    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    [[nodiscard]] const maya::Theme& theme() const noexcept;

    void apply_transcript_navigation(TranscriptNavigation navigation) noexcept;
    bool apply_transcript_mouse(
        const MouseEvent& event, int scroll_rows) noexcept;

    [[nodiscard]] TranscriptViewport::Snapshot
    transcript_viewport_snapshot() const noexcept;

    [[nodiscard]] bool render(
        const ScreenConfig& screen, Size size, const WriteSink& writer
    ) noexcept;

private:
    struct Synced {
        std::unique_ptr<maya::FrameBuffer> framebuffer;
    };
    struct Divergent {};

    maya::Theme theme_;
    std::variant<Synced, Divergent> coherence_;
    std::unique_ptr<maya::ScrollState> transcript_scroll_;
    std::unique_ptr<TranscriptViewport> transcript_viewport_;

    void demote_if_size_changed(Size size) noexcept;
    bool ship_frame(
        std::unique_ptr<maya::FrameBuffer> framebuffer,
        const std::string& bytes,
        const WriteSink& writer
    ) noexcept;
};

}  // namespace my_agent::ui
