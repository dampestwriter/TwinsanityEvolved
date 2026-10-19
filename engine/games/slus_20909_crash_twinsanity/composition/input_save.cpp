#include "games/slus_20909_crash_twinsanity/composition/input_save.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "app/host/session_clock.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"

namespace ps2r::app {
using ::title::InputStateCodec;
using ::title::SaveStateCodec;
using savestate::Blob;

InputSave::Memory InputSave::RamAccess(MainMemory& ram, Scratchpad& spad) {
    const auto pointer = [&ram, &spad](uint32_t address, size_t size) -> uint8_t* {
        if (!address) return nullptr;
        if (Scratchpad::Contains(address)) return spad.Pointer(Scratchpad::Offset(address), size);
        return ram.Pointer(address & 0x1fffffffu, size);
    };
    return {
        [pointer](uint32_t address, void* out, size_t size) {
            const auto* bytes = pointer(address, size);
            if (!bytes || !out) return false;
            std::memcpy(out, bytes, size); return true;
        },
        [pointer](uint32_t address, const void* in, size_t size) {
            auto* bytes = pointer(address, size);
            if (!bytes || !in) return false;
            std::memcpy(bytes, in, size); return true;
        }};
}

InputSave::InputSave(uint32_t selection, Memory memory, ::title::SaveStorage::Config storage,
                     std::function<uint64_t()> boundary, ::title::Input::MotorOutput motors)
    : selection_(selection), boundary_(std::move(boundary)), input_(std::move(motors)),
      storage_(std::move(storage)), input_bindings_(input_, memory),
      save_bindings_(storage_, {memory.read, memory.write, boundary_}) {
    if ((selection & ~Checkpoint::kAll) || !boundary_) throw std::invalid_argument("Invalid Input/Save composition.");
}

bool InputSave::Install(const ::title::InputBindings::Installer& install) {
    return (!InputSelected() || input_bindings_.Install(install)) &&
        (!SavesSelected() || save_bindings_.Install(install));
}

bool InputSave::Sample(uint64_t boundary, const ::title::Input::Sample& sample) {
    if (!InputSelected()) return false;
    const auto current = input_.Capture();
    if (current.sampled && boundary == current.boundary) return true;
    std::array<::title::Input::Sample, ::title::Input::kPorts * ::title::Input::kSlots> samples{};
    samples[0] = sample;
    if (!input_.SampleBoundary(boundary, samples)) return false;
    ++samples_;
    return true;
}

bool InputSave::NeedsSample(uint64_t boundary) const {
    const auto current = input_.Capture();
    return InputSelected() && (!current.sampled || boundary > current.boundary);
}

void InputSave::Pause(bool paused) {
    if (!InputSelected()) return;
    if (paused) input_.Pause(); else input_.Resume();
}

bool InputSave::CanCheckpoint(std::string* error) const {
    if (!SavesSelected() || storage_.CanCheckpoint()) return true;
    if (error) *error = "A persistent game-save mutation must finish before reproduction capture or restore.";
    return false;
}

bool InputSave::Validate(const std::vector<Blob>& blobs, const SessionClock* clock, std::string* error) const {
    Checkpoint::Plan plan;
    if (!Checkpoint::Prepare(blobs, selection_, {}, plan, error)) return false;
    for (const auto& blob : blobs) if (clock && blob.tag == SessionClock::kTag) {
        SessionClock::Snapshot saved;
        std::string why;
        if (!SessionClock::Decode(blob, saved, why) || !clock->Validate(saved, why)) {
            if (error) *error = why;
            return false;
        }
        if ((plan.input && plan.input->boundary > saved.domain.tick) ||
            (plan.saves && plan.saves->storage.last_boundary > saved.domain.tick)) {
            if (error) *error = "Input/Save polling state is later than the saved game clock.";
            return false;
        }
    }
    return true;
}

bool InputSave::Register(savestate::SectionRegistry& registry, const SessionClock* clock) {
    if (clock && !registry.Has(SessionClock::kTag)) return false;
    savestate::SectionSpec marker;
    marker.tag = Checkpoint::kTag; marker.owner = "InputSave selection";
    marker.serialize = [this](std::vector<uint8_t>& out) {
        std::string error;
        if (!Checkpoint::EncodeSelection(selection_, out, &error)) throw std::runtime_error(error);
    };
    marker.validate = [this](const Blob& blob, std::string* error) {
        uint32_t selected = 0;
        if (!Checkpoint::DecodeSelection(blob, selected, error)) return false;
        if (selected == selection_) return true;
        if (error) *error = "Saved Input/Save selection differs from the running session.";
        return false;
    };
    marker.restore = marker.validate; // immutable provider identity
    if (!registry.Register(marker)) return false;
    if (InputSelected()) {
        savestate::SectionSpec spec;
        spec.tag = InputStateCodec::kTag; spec.owner = "Input"; spec.version = InputStateCodec::kVersion;
        spec.serialize = [this](std::vector<uint8_t>& out) {
            std::string error;
            if (!InputStateCodec::Encode(input_.Capture(), out, &error)) throw std::runtime_error(error);
        };
        spec.validate = [](const Blob& blob, std::string* error) {
            ::title::Input::Snapshot state;
            return InputStateCodec::DecodeSection(blob, state, error);
        };
        spec.restore = [this, clock](const Blob& blob, std::string* error) {
            ::title::Input::Snapshot state;
            if (!InputStateCodec::DecodeSection(blob, state, error)) return false;
            return clock && clock->Restored() ? input_.RestoreSaved(state) : input_.Restore(state, boundary_());
        };
        if (!registry.Register(spec)) return false;
    }
    if (SavesSelected()) {
        savestate::SectionSpec spec;
        spec.tag = SaveStateCodec::kTag; spec.owner = "SaveStorage";
        spec.serialize = [this](std::vector<uint8_t>& out) {
            const auto state = save_bindings_.Capture();
            if (!state) throw std::runtime_error("Persistent game-save mutation is pending.");
            std::string error;
            if (!SaveStateCodec::Encode(*state, out, &error)) throw std::runtime_error(error);
        };
        spec.validate = [](const Blob& blob, std::string* error) {
            if (blob.version == SaveStateCodec::kVersion && blob.flags == savestate::kSectionRequired)
                return SaveStateCodec::Validate(blob.payload, error);
            if (error) *error = "Invalid SaveStorage section version or flags.";
            return false;
        };
        spec.restore = [this, clock](const Blob& blob, std::string* error) {
            ::title::SaveBindings::Snapshot state;
            if (!SaveStateCodec::Decode(blob.payload, state, error)) return false;
            return save_bindings_.Restore(state,
                clock && clock->Restored() ? state.storage.last_boundary : boundary_());
        };
        if (!registry.Register(spec)) return false;
    }
    return registry.RegisterValidation("InputSave", [this, clock](const auto& blobs, std::string* error) {
        return Validate(blobs, clock, error);
    });
}

bool InputSave::PrepareBoot(const std::vector<Blob>& original, const Checkpoint::LegacyPolicy& legacy,
                            std::vector<Blob>& owned, std::string& error) const {
    Checkpoint::Plan plan;
    if (!Checkpoint::Prepare(original, selection_, legacy, plan, &error)) return false;
    auto next = original;
    if (plan.legacy_file) {
        const auto remove = [&](uint32_t tag) {
            next.erase(std::remove_if(next.begin(), next.end(), [tag](const Blob& blob) { return blob.tag == tag; }),
                       next.end());
        };
        if (plan.imported_input) {
            Blob input{InputStateCodec::kTag, InputStateCodec::kVersion, savestate::kSectionRequired, {}};
            if (!InputStateCodec::Encode(*plan.input, input.payload, &error)) return false;
            remove(savestate::TAG_PADS);
            next.push_back(std::move(input));
        }
        if (plan.imported_saves) {
            Blob saves{SaveStateCodec::kTag, SaveStateCodec::kVersion, savestate::kSectionRequired, {}};
            if (!SaveStateCodec::Encode(*plan.saves, saves.payload, &error)) return false;
            remove(savestate::TAG_MCHL);
            next.push_back(std::move(saves));
        }
        Blob marker{Checkpoint::kTag, Checkpoint::kVersion, savestate::kSectionRequired, {}};
        if (!Checkpoint::EncodeSelection(selection_, marker.payload, &error)) return false;
        next.push_back(std::move(marker));
    }
    if (!Validate(next, nullptr, &error)) return false;
    owned = std::move(next);
    return true;
}

bool InputSave::PrepareBootFile(const std::string& path, const Checkpoint::LegacyPolicy& legacy,
                                savestate::FileMeta& meta, std::vector<uint8_t>& image, std::string& error,
                                const ImageCheck& validate) const {
    if (!validate) { error = "Boot restore needs metadata and clock validation."; return false; }
    savestate::FileMeta candidate;
    std::vector<Blob> original, owned;
    if (!savestate::ReadStateFile(path, &candidate, &original, &error) ||
        !PrepareBoot(original, legacy, owned, error)) return false;
    const bool unmarked = std::none_of(original.begin(), original.end(), [](const Blob& blob) {
        return blob.tag == Checkpoint::kTag;
    });
    if (!validate(candidate, original, legacy.approved && unmarked, error)) return false;
    auto bytes = savestate::SerializeStateFile(candidate, owned);
    image = std::move(bytes);
    meta = candidate;
    return true;
}

void InputSave::Record(verify::RunMeta& meta) const {
    const auto input = input_bindings_.Counts();
    const auto saves = save_bindings_.Counts();
    // Port 0 slot 0 is the title's controller; the three boundaries attribute
    // a frame-lane difference against the model arm's pad_ready (the control
    // arm's PadProtocol staging) without a game-side probe.
    const auto milestones = input_.MilestonesOf(0, 0);
    const auto at = [](const ::title::Input::Milestone& m) { return m.reached ? std::to_string(m.boundary) : std::string("-"); };
    meta.Set("input_service", "selected=" + std::to_string(InputSelected()) +
        ",samples=" + std::to_string(samples_) + ",calls=" + std::to_string(input.calls) +
        ",reports=" + std::to_string(input.reports) + ",motors=" + std::to_string(input.motor_calls) +
        ",bad_memory=" + std::to_string(input.bad_memory) + ",opened=" + at(milestones.opened) +
        ",stable=" + at(milestones.stable) + ",setup=" + at(milestones.setup));
    meta.Set("save_storage", "selected=" + std::to_string(SavesSelected()) +
        ",calls=" + std::to_string(saves.calls) + ",submitted=" + std::to_string(saves.submitted) +
        ",published=" + std::to_string(saves.published) + ",rejected=" + std::to_string(saves.rejected) +
        ",starts=" + std::to_string(saves.starts) + ",start_failures=" + std::to_string(saves.start_failures) +
        ",committed=" + std::to_string(storage_.Committed()));
}

} // namespace ps2r::app
