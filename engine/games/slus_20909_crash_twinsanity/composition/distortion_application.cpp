#include "games/slus_20909_crash_twinsanity/composition/distortion_application.h"

#include <cstring>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_state.h"

namespace ps2r::app {
namespace {
using Library = ::title::DistortionLibrary;
using Pass = ::title::DistortionPass;
using Codec = ::title::DistortionState;
using Blob = savestate::Blob;
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
struct DistortionApplication::Impl {
    MainMemory& memory;
    ::title::EffectsScene* effects;
    Output output;
    std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);
    Library library;
    bool registered = false, prepared = false, absent = false, released = false;
    std::vector<uint8_t> prepared_bytes;
    Impl(MainMemory& ram, ::title::EffectsScene* scene, bool selected, Output services)
        : memory(ram), effects(scene), output(std::move(services)), library(Hooks(selected)) {
        if (selected && (!effects || !output.environment || !output.frame || !output.reserve ||
                         !output.ordered || !output.checkpoint))
            throw std::invalid_argument("Distortion selection requires the complete ordered Graphics owner");
    }
    Pass::Hooks Hooks(bool selected) {
        Pass::Hooks hooks;
        hooks.enabled = selected;
        hooks.read = [this](uint32_t at, void* out, size_t size) {
            const auto* bytes = memory.Pointer(at, size);
            if (!bytes || !out) return false;
            std::memcpy(out, bytes, size);
            return true;
        };
        hooks.environment = [this](::title::MaterialState::Environment& environment) {
            return output.environment && output.environment(environment);
        };
        hooks.frame = [this](Pass::Frame& frame) { return output.frame && output.frame(frame); };
        hooks.reserve = [this](std::shared_ptr<const Pass::Batch> batch) {
            return output.reserve && output.reserve([this, stopped = stopped, batch = std::move(batch)] {
                if (stopped->load(std::memory_order_acquire)) return batch->Finish(false);
                bool success = false;
                try {
                    success = batch->Ready() &&
                        effects->Distortion(batch->frame, batch->environment, batch->Draws()) ==
                            ::title::EffectsScene::Result::Ok;
                } catch (...) { success = false; }
                return batch->Finish(success);
            });
        };
        return hooks;
    }
    bool PreflightImage(const std::vector<Blob>& sections, std::string* error) {
        prepared = absent = false;
        prepared_bytes.clear();
        library.DiscardStaged();
        if (stopped->load()) return Refuse(error, "Stopped Distortion owner cannot stage a restore");
        Library::Snapshot snapshot;
        ::title::MaterialState::Read incoming;
        if (!Codec::Preflight(sections, library.Selected(), snapshot, incoming, error)) return false;
        for (const auto& blob : sections) if (blob.tag == Codec::kTag) prepared_bytes = blob.payload;
        absent = prepared_bytes.empty();
        if (!library.Stage(snapshot, incoming, error)) return false;
        prepared = true;
        return true;
    }
    bool Publish(std::string* error) {
        if (stopped->load() || !prepared || !library.CanCheckpoint(error))
            return Refuse(error, "Distortion restore has no staged owner or still has pending work");
        if (library.Selected()) {
            if (!output.checkpoint(error)) return false;
            if (!output.ordered([this] { effects->ResetAfterQuiescence(); return true; }))
                return Refuse(error, "Distortion restore lost the accepted Graphics owner");
        }
        if (!library.Publish(error)) return false;
        prepared = absent = false;
        prepared_bytes.clear();
        return true;
    }
};
DistortionApplication::DistortionApplication(MainMemory& memory, ::title::EffectsScene* effects,
                                             bool selected, Output output)
    : impl_(new Impl(memory, effects, selected, std::move(output))) {}
DistortionApplication::~DistortionApplication() {
    if (!StopAfterQuiescence()) std::terminate();
}
bool DistortionApplication::Install(const Capture& capture, const Installer& installer) {
    return !impl_->stopped->load() && impl_->library.Install(capture, installer);
}
bool DistortionApplication::CanCheckpoint(std::string* error) const {
    const auto& owner = *impl_;
    if (owner.stopped->load()) return Refuse(error, "Distortion owner has stopped");
    if (!owner.registered) return Refuse(error, "Distortion state participant is not registered");
    // The shared owner settles first: with an open recording this frame's
    // closed pass is admitted as one of its unlinked groups (GXPD).
    if (owner.library.Selected() && !owner.output.checkpoint(error)) return false;
    const bool unlinked_open = owner.library.Selected() && owner.output.unlinked_open && owner.output.unlinked_open();
    if (!owner.library.CanCheckpoint(error, unlinked_open)) return false;
    return true;
}
Library::Counters DistortionApplication::Counts() const { return impl_->library.Counts(); }
bool DistortionApplication::Selected() const { return impl_->library.Selected(); }
bool DistortionApplication::StopAfterQuiescence(std::string* error) {
    auto& owner = *impl_;
    if (owner.released) return true;
    owner.stopped->store(true, std::memory_order_release);
    if (owner.library.Selected() &&
        !owner.output.ordered([&] { owner.effects->ResetAfterQuiescence(); return true; }))
        return Refuse(error, "Distortion shutdown lost its Graphics owner before resource release");
    owner.library.ResetAfterQuiescence();
    owner.library.DiscardStaged();
    owner.released = true;
    return true;
}
bool DistortionApplication::Register(savestate::SectionRegistry& registry, const AddPreflight& preflight) {
    auto& owner = *impl_;
    if (owner.stopped->load() || owner.registered || !preflight || !owner.library.CanCheckpoint()) return false;
    savestate::SectionSpec spec;
    spec.tag = Codec::kTag;
    spec.version = Codec::kVersion;
    spec.owner = "Distortion blocks and settings version";
    spec.required = owner.library.Selected();
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        Library::Snapshot snapshot;
        std::string error;
        const auto& output = impl_->output;
        const bool unlinked_open = impl_->library.Selected() && output.unlinked_open && output.unlinked_open();
        if (!CanCheckpoint(&error) || !impl_->library.CaptureState(snapshot, &error, unlinked_open) ||
            !Codec::Encode(snapshot, bytes)) throw std::runtime_error("Distortion capture: " + error);
    };
    spec.validate = [this](const Blob& blob, std::string* error) {
        Library::Snapshot snapshot;
        if (blob.version != Codec::kVersion || !Codec::Decode(blob.payload, snapshot) ||
            snapshot.selected != impl_->library.Selected() ||
            (snapshot.selected && !(blob.flags & savestate::kSectionRequired)) ||
            (blob.flags & ~savestate::kSectionRequired))
            return Refuse(error, "DSTN selection, version, flags or bytes are invalid");
        return true;
    };
    spec.restore = [this](const Blob& blob, std::string* error) {
        if (!impl_->prepared || impl_->absent || blob.payload != impl_->prepared_bytes)
            return Refuse(error, "DSTN publication differs from its unfiltered preflight");
        return impl_->Publish(error);
    };
    spec.absent = [this] {
        std::string error;
        if (impl_->library.Selected() || !impl_->prepared || !impl_->absent || !impl_->Publish(&error))
            throw std::runtime_error("Distortion absent state has no proven model-only owner: " + error);
    };
    if (!registry.Register(spec)) return false;
    preflight([this](const auto& sections, auto* error) { return impl_->PreflightImage(sections, error); });
    owner.registered = true;
    return true;
}
}  // namespace ps2r::app
