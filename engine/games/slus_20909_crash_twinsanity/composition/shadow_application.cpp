#include "games/slus_20909_crash_twinsanity/composition/shadow_application.h"

#include <cstring>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_state.h"

namespace ps2r::app {
namespace {
using Library = ::title::ShadowLibrary;
using Pass = ::title::ShadowPass;
using Codec = ::title::ShadowState;
using Blob = savestate::Blob;
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
struct ShadowApplication::Impl {
    MainMemory& memory;
    ::title::EffectsScene* effects;
    Output output;
    std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);
    ::title::GraphicsRefusals refusals; // Shadows family; before the library that sinks into it
    Library library;
    bool registered = false, prepared = false, absent = false, released = false;
    std::vector<uint8_t> prepared_bytes;
    Impl(MainMemory& ram, ::title::EffectsScene* scene, bool selected, Output services)
        : memory(ram), effects(scene), output(std::move(services)), library(Hooks(selected)) {
        if (selected && (!effects || !output.frame || !output.reserve ||
                         !output.ordered || !output.checkpoint))
            throw std::invalid_argument("Shadow selection requires the complete ordered Graphics owner");
        // Refusal sites carry the source frame (the Link ordinal of the checked
        // bucket-21 scene) when one is capturable: inside a bracket it is the
        // bracket's; a load-time refusal outside any bracket reads 0.
        refusals.SetClocks({}, [this]() -> uint64_t {
            Pass::Frame frame;
            try { return output.frame && output.frame(frame) ? frame.source_frame : 0; }
            catch (...) { return 0; }
        });
    }
    Pass::Hooks Hooks(bool selected) {
        Pass::Hooks hooks;
        hooks.enabled = selected;
        hooks.read = [this](uint32_t at, void* out, size_t size) {
            if (output.read) return output.read(at, out, size);
            const auto* bytes = memory.Pointer(at, size);
            if (!bytes || !out) return false;
            std::memcpy(out, bytes, size);
            return true;
        };
        hooks.refuse = refusals.Refuser(::title::GraphicsRefusals::Family::Shadows);
        hooks.write = [this](const std::vector<::title::ShadowSource::Write>& writes) {
            std::vector<uint8_t*> targets;
            targets.reserve(writes.size());
            for (const auto& write : writes) {
                auto* target = memory.Pointer(write.address, write.bytes.size());
                if (!target) return false;
                targets.push_back(target);
            }
            for (size_t i = 0; i < writes.size(); ++i)
                std::memcpy(targets[i], writes[i].bytes.data(), writes[i].bytes.size());
            return true;
        };
        hooks.frame = [this](Pass::Frame& frame) { return output.frame && output.frame(frame); };
        hooks.reserve = [this](std::shared_ptr<const Pass::Batch> batch) {
            return output.reserve && output.reserve([this, stopped = stopped, batch = std::move(batch)] {
                if (stopped->load(std::memory_order_acquire)) return batch->Finish(false);
                bool success = false;
                try {
                    success = batch->Ready() &&
                        effects->Shadows(batch->frame, batch->Draws()) == ::title::EffectsScene::Result::Ok;
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
        if (stopped->load()) return Refuse(error, "Stopped Shadow owner cannot stage a restore");
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
            return Refuse(error, "Shadow restore has no staged owner or still has pending work");
        if (library.Selected()) {
            if (!output.checkpoint(error)) return false;
            if (!output.ordered([this] { effects->ResetAfterQuiescence(); return true; }))
                return Refuse(error, "Shadow restore lost the accepted Graphics owner");
        }
        if (!library.Publish(error)) return false;
        prepared = absent = false;
        prepared_bytes.clear();
        return true;
    }
};
ShadowApplication::ShadowApplication(MainMemory& memory, ::title::EffectsScene* effects,
                                      bool selected, Output output)
    : impl_(new Impl(memory, effects, selected, std::move(output))) {}
ShadowApplication::~ShadowApplication() {
    // Root keeps the ordered output and scene alive past this owner. Explicit
    // session shutdown normally releases first; this also covers startup exit.
    if (!StopAfterQuiescence()) std::terminate();
}
bool ShadowApplication::Install(const Capture& capture, const Installer& installer) {
    return !impl_->stopped->load() && impl_->library.Install(capture, installer);
}
bool ShadowApplication::CanCheckpoint(std::string* error) const {
    const auto& owner = *impl_;
    if (owner.stopped->load()) return Refuse(error, "Shadow owner has stopped");
    if (!owner.registered) return Refuse(error, "Shadow state participant is not registered");
    // The shared owner settles first: with an open recording this frame's
    // closed pass is admitted as one of its unlinked groups (GXPD).
    if (owner.library.Selected() && !owner.output.checkpoint(error)) return false;
    const bool unlinked_open = owner.library.Selected() && owner.output.unlinked_open && owner.output.unlinked_open();
    if (!owner.library.CanCheckpoint(error, unlinked_open)) return false;
    return true;
}
Library::Counters ShadowApplication::Counts() const { return impl_->library.Counts(); }
::title::EffectsScene::Counters ShadowApplication::SceneCounts() const {
    return impl_->effects ? impl_->effects->Counts() : ::title::EffectsScene::Counters{};
}
const ::title::GraphicsRefusals& ShadowApplication::Refusals() const { return impl_->refusals; }
bool ShadowApplication::Selected() const { return impl_->library.Selected(); }
bool ShadowApplication::StopAfterQuiescence(std::string* error) {
    auto& owner = *impl_;
    if (owner.released) return true;
    owner.stopped->store(true, std::memory_order_release);
    if (owner.library.Selected() &&
        !owner.output.ordered([&] { owner.effects->ResetAfterQuiescence(); return true; }))
        return Refuse(error, "Shadow shutdown lost its Graphics owner before resource release");
    owner.library.DiscardStaged();
    owner.released = true;
    return true;
}
bool ShadowApplication::Register(savestate::SectionRegistry& registry, const AddPreflight& preflight) {
    auto& owner = *impl_;
    if (owner.stopped->load() || owner.registered || !preflight || !owner.library.CanCheckpoint()) return false;
    savestate::SectionSpec spec;
    spec.tag = Codec::kTag;
    spec.version = Codec::kVersion;
    spec.owner = "Shadow source and assets";
    spec.required = owner.library.Selected();
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        Library::Snapshot snapshot;
        std::string error;
        const auto& output = impl_->output;
        const bool unlinked_open = impl_->library.Selected() && output.unlinked_open && output.unlinked_open();
        if (!CanCheckpoint(&error) || !impl_->library.CaptureState(snapshot, &error, unlinked_open) ||
            !Codec::Encode(snapshot, bytes)) throw std::runtime_error("Shadow capture: " + error);
    };
    spec.validate = [this](const Blob& blob, std::string* error) {
        Library::Snapshot snapshot;
        if (blob.version != Codec::kVersion || !Codec::Decode(blob.payload, snapshot) ||
            snapshot.selected != impl_->library.Selected() ||
            (snapshot.selected && !(blob.flags & savestate::kSectionRequired)) ||
            (blob.flags & ~savestate::kSectionRequired))
            return Refuse(error, "SHDW selection, version, flags or bytes are invalid");
        return true;
    };
    spec.restore = [this](const Blob& blob, std::string* error) {
        if (!impl_->prepared || impl_->absent || blob.payload != impl_->prepared_bytes)
            return Refuse(error, "SHDW publication differs from its unfiltered preflight");
        return impl_->Publish(error);
    };
    spec.absent = [this] {
        std::string error;
        if (impl_->library.Selected() || !impl_->prepared || !impl_->absent || !impl_->Publish(&error))
            throw std::runtime_error("Shadow absent state has no proven model-only owner: " + error);
    };
    if (!registry.Register(spec)) return false;
    preflight([this](const auto& sections, auto* error) { return impl_->PreflightImage(sections, error); });
    owner.registered = true;
    return true;
}
}  // namespace ps2r::app
