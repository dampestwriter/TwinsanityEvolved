#pragma once

#include "games/slus_20909_crash_twinsanity/composition/input_save.h"

class StateRestorer;
namespace ps2r::app {
class MovieCheckpoint;
class SessionClock;

// Boot composition validates the original provenance/clock before converting
// Input/Save records. The prepared image always carries explicit Movie state;
// no permission to omit that owner escapes into future reproduction restores.
class MovieBoot {
public:
    MovieBoot(MovieCheckpoint&, const InputSave&, const SessionClock&);
    bool PrepareFile(const std::string& path, const InputSave::Checkpoint::LegacyPolicy&,
                     std::string& error);
    const savestate::FileMeta& Meta() const { return meta_; }
    bool Restore(StateRestorer&, std::string& error);

private:
    MovieCheckpoint& movie_;
    const InputSave& input_;
    const SessionClock& clock_;
    savestate::FileMeta meta_;
    std::vector<uint8_t> image_;
    bool prepared_ = false, imported_ = false;
};
}  // namespace ps2r::app
