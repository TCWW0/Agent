#include "my_agent/ui/transcript_guidance.hpp"

#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace my_agent::ui {

namespace {

void append_field(std::string& output, std::string_view value)
{
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
}

std::string transcript_signature(const Model& model)
{
    std::string signature;
    for (const Message& message : model.domain.thread.messages) {
        signature.push_back(message.role == Role::User ? 'u' : 'a');
        append_field(signature, message.text);
        if (message.error) {
            signature.push_back('e');
            append_field(signature, *message.error);
        } else {
            signature.push_back('n');
        }

        signature += std::to_string(message.tool_calls.size());
        signature.push_back(';');
        for (const ToolCall& call : message.tool_calls) {
            append_field(signature, call.id);
            append_field(signature, call.name);
            append_field(signature, call.args.dump());
            std::visit(
                [&signature](const auto& status) {
                    using Status = std::decay_t<decltype(status)>;
                    if constexpr (std::is_same_v<Status, ToolCall::Pending>) {
                        signature.push_back('p');
                    } else if constexpr (std::is_same_v<Status, ToolCall::Done>) {
                        signature.push_back('d');
                        append_field(signature, status.output);
                    } else if constexpr (
                        std::is_same_v<Status, ToolCall::Failed>) {
                        signature.push_back('f');
                        append_field(signature, status.output);
                    } else if constexpr (
                        std::is_same_v<Status, ToolCall::Cancelled>) {
                        signature.push_back('c');
                        append_field(signature, status.output);
                    } else {
                        signature.push_back('r');
                        append_field(signature, status.output);
                    }
                },
                call.status);
        }
    }
    return signature;
}

bool at_bottom(const TranscriptViewport::Snapshot& snapshot) noexcept
{
    return snapshot.offset == snapshot.maximum;
}

}  // namespace

std::uint64_t TranscriptRevisionTracker::observe(const Model& model)
{
    std::string next = transcript_signature(model);
    if (!initialized_) {
        initialized_ = true;
        signature_ = std::move(next);
        revision_ = 1;
        return revision_;
    }
    if (next != signature_) {
        signature_ = std::move(next);
        ++revision_;
    }
    return revision_;
}

void TranscriptGuidance::reconcile(
    const TranscriptViewport::Snapshot& snapshot,
    std::uint64_t revision) noexcept
{
    if (at_bottom(snapshot)) {
        seen_revision_ = revision;
    }
}

TranscriptHintKind TranscriptGuidance::hint(
    const TranscriptViewport::Snapshot& snapshot,
    std::uint64_t revision) const noexcept
{
    if (at_bottom(snapshot)) {
        return TranscriptHintKind::None;
    }
    return revision > seen_revision_
        ? TranscriptHintKind::NewOutput
        : TranscriptHintKind::Latest;
}

}  // namespace my_agent::ui
