#include "games/slus_20909_crash_twinsanity/composition/session_services.h"

#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace ps2r::app {
namespace {
bool Fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}
}

SessionServices::SessionServices(bool selected, title::Audio* audio, sound::AudioMixer* output)
    : selected_(selected), audio_(audio), output_(output) {}

bool SessionServices::Capture(std::vector<uint8_t>& bytes, std::string* error) const {
    if (!selected_ || !StreamLibrary::Enabled()) return Fail(error, "file owner is not selected");
    // Queued Movie PCM is the Movie owner's refusal (MovieAudio::CanCheckpoint
    // through MovieCheckpoint); the output sink holds no Movie state.
    if (output_ && !audio_) return Fail(error, "owned output requires the Audio owner");
    std::vector<uint8_t> payload;
    if (audio_) {
        title::ServiceState::State state;
        if (!title::ServiceState::Capture(*audio_, state, error) ||
            !title::ServiceState::ValidateMovie(state, {}, error) ||
            !title::ServiceState::Encode(*audio_, state, payload, error)) return false;
    } else if (!StreamState::Capture(payload, error)) {
        return false;
    }
    const size_t envelope = output_ ? 28 : 8;
    if (payload.size() + envelope > savestate::kMaxSectionPayload)
        return Fail(error, "owned service state exceeds the state-file section limit");
    std::vector<uint8_t> out;
    title::StateBytes::Put(out, output_ ? kOutputVersion : kVersion);
    title::StateBytes::Put(out, (audio_ ? 1 : 0) | (output_ ? 2 : 0));
    if (output_) {
        const auto& state = output_->Inspect();
        title::StateBytes::PutFloat(out, state.master);
        for (uint32_t group = 0; group < sound::AudioMixer::kGroups; ++group) {
            title::StateBytes::PutFloat(out, state.left[group]);
            title::StateBytes::PutFloat(out, state.right[group]);
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    bytes = std::move(out);
    return true;
}

bool SessionServices::CanCheckpoint(std::string* error) const {
    if (!selected_) return true;
    std::vector<uint8_t> bytes;
    return Capture(bytes, error);
}

bool SessionServices::Decode(const savestate::Blob& blob, State& out, std::string* error) const {
    if (!selected_ || !StreamLibrary::Enabled() || blob.tag != kTag ||
        blob.version != (output_ ? kOutputVersion : kVersion) || blob.flags != savestate::kSectionRequired ||
        blob.payload.size() > savestate::kMaxSectionPayload)
        return Fail(error, "service section envelope or selection mismatch");
    title::StateBytes in(blob.payload);
    uint32_t version = 0, owners = 0;
    std::vector<uint8_t> payload;
    if (!in.Get(version) || version != blob.version || !in.Get(owners) ||
        owners != ((audio_ ? 1u : 0u) | (output_ ? 2u : 0u)) || (output_ && !audio_))
        return Fail(error, "service section version or Audio selection mismatch");
    State state;
    if (output_) {
        if (!in.GetFloat(state.output.master)) return Fail(error, "missing output master gain");
        for (uint32_t group = 0; group < sound::AudioMixer::kGroups; ++group)
            if (!in.GetFloat(state.output.left[group]) || !in.GetFloat(state.output.right[group]))
                return Fail(error, "missing output group gain");
        if (!sound::AudioMixer::Validate(state.output)) return Fail(error, "invalid output gain state");
    }
    if (!in.GetBytes(in.Remaining(), payload)) return Fail(error, "missing service payload");
    if (audio_) {
        if (!title::ServiceState::Decode(*audio_, payload, state.services, error) ||
            !title::ServiceState::ValidateMovie(state.services, {}, error)) return false;
    } else if (!StreamState::Validate(payload, error) ||
               !StreamState::Decode(payload, state.files, error)) {
        return false;
    }
    out = std::move(state);
    return true;
}

uint64_t SessionServices::Tick(const State& state) const {
    return (audio_ ? state.services.files : state.files).streams.schedule.tick;
}

bool SessionServices::ValidateImage(const std::vector<savestate::Blob>& image, uint64_t tick,
                                    std::string* error) const {
    const savestate::Blob* found = nullptr;
    for (const auto& blob : image) if (blob.tag == kTag) {
        if (found) return Fail(error, "duplicate service section");
        found = &blob;
    }
    if (!selected_) return !found || Fail(error, "saved service owner is not selected");
    if (!found) return Fail(error, "selected files and Audio require their service section");
    State state;
    if (!Decode(*found, state, error)) return false;
    return Tick(state) == tick || Fail(error, "saved service deadline clock differs from game clock");
}

bool SessionServices::Restore(const savestate::Blob& blob, std::string* error) {
    State state;
    if (!Decode(blob, state, error)) return false;
    if (audio_) {
        title::StreamBuffers::RenewedLoans renewed;
        if (!title::ServiceState::Restore(*audio_, state.services, {}, renewed, error)) return false;
        // Output state was validated before any owner mutated. Abandoned
        // Movie PCM is the Movie owner's restore (MOVI), not this section's.
        return !output_ || output_->Restore(state.output);
    }
    return LoaderFileIo::RestoreState(state.files, error);
}

bool SessionServices::ImportFiles(const LoaderFileIo::State& files, std::string* error) {
    if (!selected_ || !StreamLibrary::Enabled()) return Fail(error, "file owner is not selected");
    if (audio_ || output_) return Fail(error, "the Audio-composed service state cannot be derived from a legacy image");
    return LoaderFileIo::RestoreState(files, error);
}

savestate::SectionSpec SessionServices::Section(std::function<void()> legacy_absent) {
    savestate::SectionSpec spec;
    spec.tag = kTag; spec.version = output_ ? kOutputVersion : kVersion;
    spec.owner = "SessionServices";
    spec.required = !legacy_absent; spec.capture_required = true;
    spec.absent = std::move(legacy_absent);
    spec.serialize = [this](std::vector<uint8_t>& out) {
        std::string error;
        if (!Capture(out, &error)) throw std::runtime_error(error);
    };
    spec.validate = [this](const savestate::Blob& blob, std::string* error) {
        State state;
        return Decode(blob, state, error);
    };
    spec.restore = [this](const savestate::Blob& blob, std::string* error) { return Restore(blob, error); };
    return spec;
}

} // namespace ps2r::app
