// Smoke test for the maya rendering dependency (slice #15).
//
// This is the FIRST consumer of the only maya seam this project uses:
//
//   maya::render::FrameBuffer fb(width, height);
//   maya::Element ui = maya::dsl::text("...");
//   const std::string& bytes = fb.render(ui, maya::theme::dark);   // no swap
//   fb.commit();                                                   // swap
//
// #16 ("two-stage projection skeleton") reuses exactly this call shape:
// build an Element -> render -> inspect bytes. Keep the pattern legible.
//
// We deliberately DO NOT touch maya's run<Program>() event loop; this project
// keeps its own AsyncHost/WorkPool loop and uses maya only for rendering.

#include <maya/dsl.hpp>
#include <maya/render/frame.hpp>
#include <maya/style/theme.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace maya;
using namespace maya::dsl;

namespace {

// A mark that cannot plausibly pre-exist in any framing bytes maya emits
// unconditionally (cursor-hide, sync markers, SGR reset). If it shows up in
// the output, maya really rendered OUR element, not just its wrapper.
constexpr const char* kSentinel = "KIRO7F3A9SMOKE";

} // namespace

// Slice A: the library links and a minimal element renders to non-empty bytes
// that actually carry the content we asked for.
TEST(MayaSmoke, RenderProducesNonEmptyBytesContainingContent) {
    FrameBuffer fb(60, 3);
    const std::string& bytes = fb.render(text(kSentinel), theme::dark);

    EXPECT_FALSE(bytes.empty());
    EXPECT_NE(bytes.find(kSentinel), std::string::npos)
        << "rendered bytes must contain the sentinel we asked maya to draw";
}

// Slice B: pin the render/commit contract we depend on (guards against a
// future maya version drifting this behavior out from under us).
//
// render() does NOT swap. commit() flips front<-back. So committing after a
// successful write means the next identical render is an empty grid diff; the
// content is not re-drawn. This is the "write succeeded" happy path.
TEST(MayaSmoke, CommitAfterRenderMakesNextIdenticalFrameSkipContent) {
    FrameBuffer fb(60, 3);

    const std::string& first = fb.render(text(kSentinel), theme::dark);
    ASSERT_NE(first.find(kSentinel), std::string::npos)
        << "first render draws the content";
    fb.commit();  // as if the write(2) succeeded

    const std::string& second = fb.render(text(kSentinel), theme::dark);
    EXPECT_EQ(second.find(kSentinel), std::string::npos)
        << "after commit, an identical frame is an empty grid diff — content "
           "is not re-drawn";
}

// The mirror case: if the write failed we SKIP commit, so front_ never
// advances and the next render produces a COMPLETE diff (content re-drawn),
// not an empty one. This is the property that keeps a dropped frame from
// leaving the terminal permanently stale.
TEST(MayaSmoke, SkippingCommitMakesNextFrameACompleteDiff) {
    FrameBuffer fb(60, 3);

    const std::string& first = fb.render(text(kSentinel), theme::dark);
    ASSERT_NE(first.find(kSentinel), std::string::npos);
    // No commit() here: simulate a failed/partial write.

    const std::string& second = fb.render(text(kSentinel), theme::dark);
    EXPECT_NE(second.find(kSentinel), std::string::npos)
        << "without commit, front_ did not advance, so the next frame must be "
           "a complete diff that re-draws the content";
}
