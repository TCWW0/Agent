#include "my_agent/ui/transcript_guidance.hpp"

#include <gtest/gtest.h>

namespace {

using my_agent::Message;
using my_agent::Model;
using my_agent::Role;
using my_agent::ToolCall;
using my_agent::ui::TranscriptGuidance;
using my_agent::ui::TranscriptHintKind;
using my_agent::ui::TranscriptRevisionTracker;
using my_agent::ui::TranscriptViewport;

TEST(TranscriptGuidanceTest, RevisionTracksMeaningfulTranscriptChangesOnly)
{
    Model model;
    model.domain.thread.messages.push_back({
        .role = Role::Assistant,
        .text = "first",
    });

    TranscriptRevisionTracker tracker;
    const auto initial = tracker.observe(model);
    EXPECT_GT(initial, 0u);
    EXPECT_EQ(initial, tracker.observe(model));

    model.phase = my_agent::Streaming{};
    model.domain.profile = my_agent::Profile::Ask;
    model.session.stream_telemetry.output_bytes = 42;
    model.session.stream_telemetry.generation = 7;
    EXPECT_EQ(initial, tracker.observe(model));

    model.domain.thread.messages.front().text = "first, continued";
    const auto text_revision = tracker.observe(model);
    EXPECT_GT(text_revision, initial);

    model.domain.thread.messages.front().error = "provider failed";
    const auto error_revision = tracker.observe(model);
    EXPECT_GT(error_revision, text_revision);

    model.domain.thread.messages.front().tool_calls.push_back({
        .id = "call-1",
        .name = "read",
        .args = { {"path", "README.md"} },
    });
    const auto tool_revision = tracker.observe(model);
    EXPECT_GT(tool_revision, error_revision);

    model.domain.thread.messages.front().tool_calls.front().status =
        ToolCall::Done{.output = "contents"};
    const auto output_revision = tracker.observe(model);
    EXPECT_GT(output_revision, tool_revision);

    model.domain.thread.messages.push_back({
        .role = Role::User,
        .text = "next turn",
    });
    EXPECT_GT(tracker.observe(model), output_revision);
}

TEST(TranscriptGuidanceTest, GeometryDrivesSeenAndUnseenHints)
{
    TranscriptGuidance guidance;
    const TranscriptViewport::Snapshot bottom{
        .offset = 10,
        .maximum = 10,
        .viewport_height = 20,
        .at_bottom = false,
    };
    const TranscriptViewport::Snapshot frozen{
        .offset = 3,
        .maximum = 10,
        .viewport_height = 20,
        .at_bottom = true,
    };

    guidance.reconcile(bottom, 4);
    EXPECT_EQ(TranscriptHintKind::None, guidance.hint(bottom, 4));

    EXPECT_EQ(TranscriptHintKind::Latest, guidance.hint(frozen, 4));
    guidance.reconcile(frozen, 4);
    EXPECT_EQ(TranscriptHintKind::Latest, guidance.hint(frozen, 4));
    EXPECT_EQ(TranscriptHintKind::NewOutput, guidance.hint(frozen, 5));

    const TranscriptViewport::Snapshot grown_bottom{
        .offset = 12,
        .maximum = 12,
        .viewport_height = 20,
        .at_bottom = false,
    };
    guidance.reconcile(grown_bottom, 5);
    EXPECT_EQ(TranscriptHintKind::None, guidance.hint(grown_bottom, 5));

    const TranscriptViewport::Snapshot still_frozen{
        .offset = 3,
        .maximum = 12,
        .viewport_height = 20,
        .at_bottom = false,
    };
    EXPECT_EQ(TranscriptHintKind::Latest, guidance.hint(still_frozen, 5));
}

}  // namespace
