#include "games/slus_20909_crash_twinsanity/composition/screen_application.h"

#include <atomic>
#include <cstring>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_report.h"

namespace ps2r::app {
namespace {
using Pass = ::title::ScreenPass;
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
struct ScreenApplication::Impl {
    MainMemory& memory;
    ::title::EffectsScene* effects;
    Output output;
    std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);
    Pass pass;
    bool installed = false, released = false;
    Impl(MainMemory& ram, ::title::EffectsScene* scene, bool selected, Output services)
        : memory(ram), effects(scene), output(std::move(services)), pass(Read(), Hooks(selected)) {
        if (selected && (!effects || !output.frame || !output.reserve || !output.ordered || !output.checkpoint))
            throw std::invalid_argument("Screen selection requires the complete ordered Graphics owner");
    }
    Pass::Read Read() {
        return [this](uint32_t at, void* out, size_t size) {
            const auto* bytes = memory.Pointer(at, size);
            if (!bytes || !out) return false;
            std::memcpy(out, bytes, size);
            return true;
        };
    }
    Pass::Hooks Hooks(bool selected) {
        Pass::Hooks hooks;
        hooks.enabled = selected;
        hooks.frame = [this](Pass::Frame& frame) { return output.frame && output.frame(frame); };
        hooks.reserve = [this](std::shared_ptr<const Pass::Batch> batch) {
            // The closure owns the reservation: when the root drops it
            // unexecuted (GraphicsOutput::CancelOwned after a GraphicsBridge::
            // Discard, or a declined RecordOwned) the batch is cancelled -- no
            // credit, no fault -- so a closed boundary never waits for a
            // delivery that will not come. A no-op once Finish has run.
            struct Reservation {
                std::shared_ptr<const Pass::Batch> batch;
                explicit Reservation(std::shared_ptr<const Pass::Batch> owned) : batch(std::move(owned)) {}
                Reservation(const Reservation&) = delete;
                Reservation& operator=(const Reservation&) = delete;
                ~Reservation() { if (batch) batch->Cancel(); }
            };
            // Constructed in place: a copied/moved-from temporary would Cancel the
            // live reservation in its own destructor (RootScreenFilterLevel red:
            // every owned slot refused, passes=0).
            auto reservation = std::make_shared<Reservation>(batch);
            return output.reserve && output.reserve([this, stopped = stopped, batch = std::move(batch), reservation] {
                if (stopped->load(std::memory_order_acquire)) return batch->Finish(false);
                bool success = false;
                try {
                    success = batch->Ready() &&
                        effects->Screen(batch->frame, batch->work) == ::title::EffectsScene::Result::Ok;
                } catch (...) { success = false; }
                return batch->Finish(success);
            });
        };
        return hooks;
    }
};
ScreenApplication::ScreenApplication(MainMemory& memory, ::title::EffectsScene* effects, bool selected,
                                     Output output)
    : impl_(std::make_unique<Impl>(memory, effects, selected, std::move(output))) {}
ScreenApplication::~ScreenApplication() {
    if (!StopAfterQuiescence()) std::terminate();
}
bool ScreenApplication::Install(const Capture& capture, const Installer& installer) {
    if (impl_->installed || impl_->stopped->load() || !capture || !installer) return false;
    impl_->installed = impl_->pass.Install(capture, installer);
    return impl_->installed;
}
bool ScreenApplication::Selected() const { return impl_->pass.Selected(); }
bool ScreenApplication::CanCheckpoint(std::string* error) const {
    const auto& owner = *impl_;
    if (owner.stopped->load()) return Refuse(error, "Screen owner has stopped");
    // The shared owner settles first: with an open recording this frame's
    // reserved slot is admitted as one of its unlinked items (GXPD), exactly
    // as the shadow and distortion passes' closed brackets are.
    if (owner.pass.Selected() && !owner.output.checkpoint(error)) return false;
    const bool unlinked_open = owner.pass.Selected() && owner.output.unlinked_open && owner.output.unlinked_open();
    if (owner.pass.CanCheckpoint(unlinked_open)) return true;
    return Refuse(error, unlinked_open
        ? "screen effects boundary is executing"
        : "screen effects boundary is executing or a reserved frame is undelivered at a closed boundary");
}
bool ScreenApplication::StopAfterQuiescence(std::string* error) {
    auto& owner = *impl_;
    if (owner.released) return true;
    owner.stopped->store(true, std::memory_order_release);
    if (owner.pass.Selected() &&
        !owner.output.ordered([&] { owner.effects->ResetAfterQuiescence(); return true; }))
        return Refuse(error, "Screen shutdown lost its Graphics owner before resource release");
    owner.pass.ResetAfterQuiescence();
    owner.released = true;
    return true;
}
::title::ScreenPass::Counters ScreenApplication::Counts() const { return impl_->pass.Counts(); }
void ScreenApplication::Record(verify::RunMeta& meta) const {
    const bool selected = Selected();
    ::title::EffectsReport::Screen(meta, selected, impl_->pass.Counts(),
        selected && impl_->effects ? impl_->effects->Counts() : ::title::EffectsScene::Counters{},
        selected && impl_->effects ? impl_->effects->ScreenCounts() : ::title::ScreenRenderer::Counters{},
        selected && impl_->effects ? impl_->effects->WaveCounts() : ::title::WaveRenderer::Counters{});
}
}  // namespace ps2r::app
