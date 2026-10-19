#pragma once

#include <functional>
#include <string>
#include <vector>

#include "engine/core/savestate/section_registry.h"
#include "engine/sound/audio_mixer.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/service_state.h"

namespace ps2r::app {

// Application snapshot owner for the selected file/Audio composition.
// Capture and preflight run on the stopped game owner without pumping work.
// The supervisor restores RAM and its saved logical clock before this owner.
class SessionServices {
public:
    static constexpr uint32_t kTag = 0x53435653; // SVCS
    static constexpr uint32_t kVersion = 1;
    // Version 3 output envelope: master + two stereo group gains only. The
    // Movie gain left this section for MOVI (its owner); version 2 images
    // carrying the two Movie floats are refused, never reinterpreted.
    static constexpr uint32_t kOutputVersion = 3;
    SessionServices(bool selected, title::Audio* audio, sound::AudioMixer* output = nullptr);
    bool Enabled() const { return selected_; }
    // With `legacy_absent`, SVCS stays required of every capture but a frozen
    // legacy image may omit it: the callback publishes the explicit import
    // (ServiceImport) at this owner's restore position.
    savestate::SectionSpec Section(std::function<void()> legacy_absent = {});
    bool CanCheckpoint(std::string* error) const;
    // The legacy import's publication on the file-only arm: the state derived
    // from the image's own StreamSystem (StreamState::MigrateFiles). The
    // Audio-composed arm has no derivable state and refuses.
    bool ImportFiles(const LoaderFileIo::State& files, std::string* error);
    // Original unfiltered image; M0 supplies its validated GCLK domain tick.
    // Active Movie snapshots need their separate consumer/loan join first.
    bool ValidateImage(const std::vector<savestate::Blob>& image, uint64_t tick,
                       std::string* error) const;

private:
    struct State {
        LoaderFileIo::State files;
        title::ServiceState::State services;
        sound::AudioMixer::State output;
    };
    bool Capture(std::vector<uint8_t>& bytes, std::string* error) const;
    bool Decode(const savestate::Blob& blob, State& state, std::string* error) const;
    bool Restore(const savestate::Blob& blob, std::string* error);
    uint64_t Tick(const State& state) const;

    bool selected_ = false;
    title::Audio* audio_ = nullptr;
    sound::AudioMixer* output_ = nullptr;
};

} // namespace ps2r::app
