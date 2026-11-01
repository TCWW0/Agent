#include "my_agent/ui/painter.hpp"

#include <maya/core/scroll_state.hpp>
#include <maya/dsl.hpp>
#include <maya/element/builder.hpp>
#include <maya/render/frame.hpp>
#include <maya/render/pipeline.hpp>
#include <maya/render/serialize.hpp>
#include <maya/widget/conversation.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace my_agent::ui {

namespace {

[[nodiscard]]
maya::Element build_fullscreen_element(
    const ScreenConfig& screen, const Size& size, maya::ScrollState& scroll)
{
    using namespace maya::dsl;

    maya::Element dock = dock_element_for_height(
        screen.dock, size.columns, size.rows);
    const int dock_rows = maya::measure_element(
        dock, std::max(1, size.columns)).height.value;

    ConversationConfig transcript_config = screen.transcript;
    if (size.rows - dock_rows <= 3 && !transcript_config.turns.empty()) {
        // A normal Turn spends two rows on header + breathing space. In a
        // one/two-row viewport that leaves only a rail visible even at the
        // scroll tail. Compact only the live tail to the public continuation
        // form so the user's last content remains observable; older turns
        // keep their full geometry and the historical scroll range.
        transcript_config.compact_tail = true;
    }
    maya::Element conversation = (
        conversation_element(transcript_config) | width(size.columns)
    ).build();
    maya::Element scroll_content = v(std::move(conversation)).build();
    maya::Element transcript = maya::detail::vstack()
        .grow(1.0f)
        .shrink(1.0f)
        .min_height(maya::Dimension::fixed(0))
        .overflow(maya::Overflow::Hidden)
        (std::move(scroll_content)
            | scrolly(scroll, 0)
            | grow(1.0f));

    return maya::detail::vstack()
        .width(maya::Dimension::fixed(size.columns))
        .height(maya::Dimension::fixed(size.rows))
        .overflow(maya::Overflow::Hidden)
        (
            std::move(transcript),
            std::move(dock) | shrink(0.0f)
        );
}

}  // namespace

Painter::Painter(maya::Theme theme)
    : theme_{std::move(theme)},
      coherence_{Divergent{}},
      transcript_scroll_{std::make_unique<maya::ScrollState>()},
      transcript_viewport_{
          std::make_unique<TranscriptViewport>(*transcript_scroll_)}
{
}

Painter::~Painter() = default;

const maya::Theme& Painter::theme() const noexcept
{
    return theme_;
}

void Painter::apply_transcript_navigation(
    TranscriptNavigation navigation) noexcept
{
    transcript_viewport_->apply(navigation);
}

bool Painter::apply_transcript_mouse(
    const MouseEvent& event, int scroll_rows) noexcept
{
    return transcript_viewport_->apply_mouse(event, scroll_rows);
}

TranscriptViewport::Snapshot Painter::transcript_viewport_snapshot() const noexcept
{
    return transcript_viewport_->snapshot();
}

bool Painter::render(
    const ScreenConfig& screen, Size size, const WriteSink& writer) noexcept
{
    demote_if_size_changed(size);
    const bool follow_tail = transcript_scroll_->at_bottom();
    const auto build = [&]() {
        return build_fullscreen_element(screen, size, *transcript_scroll_);
    };

    if (auto* synced = std::get_if<Synced>(&coherence_)) {
        maya::FrameBuffer& fb = *synced->framebuffer;
        fb.set_cursor_visible(false);
        maya::Element element = build();
        const std::string* bytes = &fb.render(element, theme_);
        if (follow_tail && !transcript_scroll_->at_bottom()) {
            transcript_scroll_->scroll_to_bottom();
            element = build();
            bytes = &fb.render(element, theme_);
        }
        return ship_frame(std::move(synced->framebuffer), *bytes, writer);
    }

    auto fb = std::make_unique<maya::FrameBuffer>(size.columns, size.rows);
    fb->set_cursor_visible(false);
    std::string out;
    const auto paint = [&]() {
        out.clear();
        auto opened = maya::RenderPipeline<maya::stage::Idle>::start(
                          fb->back().canvas, fb->style_pool(), theme_, out)
                          .clear()
                          .paint(build())
                          .open_frame();
        out += "\x1b[H";
        maya::serialize(fb->back().canvas, fb->style_pool(), out);
        std::move(opened).close_frame();
    };
    paint();
    if (follow_tail && !transcript_scroll_->at_bottom()) {
        transcript_scroll_->scroll_to_bottom();
        paint();
    }
    return ship_frame(std::move(fb), out, writer);
}

void Painter::demote_if_size_changed(Size size) noexcept
{
    if (const auto* synced = std::get_if<Synced>(&coherence_)) {
        if (synced->framebuffer->width() != size.columns
            || synced->framebuffer->height() != size.rows) {
            coherence_ = Divergent{};
        }
    }
}

bool Painter::ship_frame(
    std::unique_ptr<maya::FrameBuffer> framebuffer,
    const std::string& bytes,
    const WriteSink& writer
) noexcept
{
    if (!writer || !writer(bytes)) {
        return false;
    }
    framebuffer->commit();
    coherence_ = Synced{std::move(framebuffer)};
    return true;
}

}  // namespace my_agent::ui
