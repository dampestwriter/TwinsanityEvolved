#include "games/slus_20909_crash_twinsanity/composition/savestate_trigger.h"

#include <utility>

#include "app/host/savestate_slots.h"
#include "games/slus_20909_crash_twinsanity/composition/session_capture.h"
#include "engine/core/config/config.h"

namespace ps2r::app {
namespace {
constexpr unsigned kMaxAttempts = 120;   // the same window SessionRuntime gives F5
constexpr uint64_t kRequestId = 1;       // the harness owns one request per leg
}  // namespace

SavestateTrigger::SavestateTrigger(SessionCapture& capture, const Config& config, MetaSink meta)
    : capture_(capture), config_(config), meta_(std::move(meta)) {
    if (config_.harness.save_at_frame > 0) state_ = State::Armed;
}

void SavestateTrigger::Terminal(State state, const char* phase, const std::string& detail) {
    state_ = state;
    SavestateSlots::Report("harness save", phase, detail);
    if (meta_) {
        const char* word = state == State::Written ? "written" : state == State::Refused ? "refused" :
                           state == State::Failed ? "failed" : "pending";
        meta_("savestate", SavestateSlots::TriggerMetaLine(word, path_, frame_, boundary_, detail));
    }
}

void SavestateTrigger::OnBoundary(uint64_t game_frame, uint64_t boundary) {
    using Phase = SessionControls::Phase;
    switch (state_) {
        case State::Armed:
            if (boundary < static_cast<uint64_t>(config_.harness.save_at_frame)) return;
            state_ = State::Waiting;
            SavestateSlots::Report("harness save", "Queued",
                "harness.save_at_frame=" + std::to_string(config_.harness.save_at_frame) + " reached at boundary " +
                std::to_string(boundary) + "; capturing at the first admissible boundary");
            [[fallthrough]];
        case State::Waiting: {
            const auto result = capture_.Capture(kRequestId, game_frame);
            frame_ = game_frame;
            boundary_ = boundary;
            path_ = result.path;
            if (result.phase == Phase::Working) {
                state_ = State::Writing;
                return;
            }
            if (result.phase == Phase::Waiting) {
                last_refusal_ = result.detail;
                if (++attempts_ < kMaxAttempts) return;
                Terminal(State::Refused, "Refused",
                    "No admissible boundary in " + std::to_string(attempts_) +
                    " consecutive boundaries; the last owner to refuse: " + last_refusal_);
                return;
            }
            Terminal(result.phase == Phase::Refused ? State::Refused : State::Failed,
                     SessionControls::PhaseLabel(result.phase), result.detail);
            return;
        }
        case State::Writing: {
            const auto result = capture_.Poll();
            if (!result) return;
            path_ = result->path;
            frame_ = result->frame;
            Terminal(result->phase == Phase::Succeeded ? State::Written : State::Failed,
                     SessionControls::PhaseLabel(result->phase), result->detail);
            return;
        }
        default: return;
    }
}

void SavestateTrigger::Finish() {
    using Phase = SessionControls::Phase;
    if (state_ == State::Waiting) {
        Terminal(State::Refused, "Refused",
            "The leg ended after " + std::to_string(attempts_) + " refused boundaries; the last owner to refuse: " +
            last_refusal_);
        return;
    }
    if (state_ != State::Writing) return;
    capture_.Join();
    const auto result = capture_.Poll();
    if (!result) {
        Terminal(State::Failed, "Failed", "The writer joined without a completion for " + path_);
        return;
    }
    path_ = result->path;
    frame_ = result->frame;
    Terminal(result->phase == Phase::Succeeded ? State::Written : State::Failed,
             SessionControls::PhaseLabel(result->phase), result->detail);
}

}  // namespace ps2r::app
