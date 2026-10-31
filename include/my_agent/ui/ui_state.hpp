#pragma once

#include "my_agent/ui/composer_paste.hpp"
#include "my_agent/ui/status_bar.hpp"
#include "my_agent/ui/stream_rate.hpp"
#include "my_agent/ui/transcript_guidance.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace my_agent::ui {

// UI-local editing and telemetry state. Core sees only a complete Submit.
struct UiState {
    std::string input;
    std::size_t cursor{std::string::npos};
    std::vector<PendingPaste> pastes;
    // Display configuration and sampled rate/time only; usage is read from
    // the current Model when project_screen builds the status bar.
    StatusBarInput status;
    TokenRateHistory token_rate_history;
    LiveTokenRate live_token_rate;
    TranscriptHintKind transcript_hint{TranscriptHintKind::None};
};

}  // namespace my_agent::ui
