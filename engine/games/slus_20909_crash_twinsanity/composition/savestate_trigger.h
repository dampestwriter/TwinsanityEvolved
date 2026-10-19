#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps2r {
class Config;
namespace app {
class SessionCapture;

// `--set harness.save_at_frame=N`: the F5 capture without a key press, for the
// harness legs that pin the slot round trip. From boundary N on, inside the
// completed game-boundary callback, it asks SessionCapture for the configured
// slot exactly as F5 does; an owner's refusal is retried at the following
// boundaries (the same 120-boundary window as F5) and then REFUSED LOUDLY,
// naming the last refusing owner. The writer's terminal result is polled at
// later boundaries and at Finish (the leg's frame cap). Every terminal result
// reaches stderr, the SAVE log and run.meta (`savestate=`), never only a log.
class SavestateTrigger {
public:
    using MetaSink = std::function<void(const std::string& key, const std::string& value)>;
    enum class State { Idle, Armed, Waiting, Writing, Written, Refused, Failed };

    SavestateTrigger(SessionCapture& capture, const Config& config, MetaSink meta);
    // Game owner, inside the completed game-boundary callback.
    void OnBoundary(uint64_t game_frame, uint64_t boundary);
    // Any host owner, once admission has stopped (the frame cap, the close):
    // joins the writer and reports the last result.
    void Finish();
    State Current() const { return state_; }
    const std::string& Path() const { return path_; }

private:
    void Terminal(State state, const char* phase, const std::string& detail);

    SessionCapture& capture_;
    const Config& config_;
    MetaSink meta_;
    State state_ = State::Idle;
    unsigned attempts_ = 0;
    uint64_t frame_ = 0;
    uint64_t boundary_ = 0;   // the boundary count at the accepted capture
    std::string path_;
    std::string last_refusal_;
};

}  // namespace app
}  // namespace ps2r
