#include "games/slus_20909_crash_twinsanity/composition/effects_application.h"

#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_producers.h"

namespace ps2r::app {
EffectsApplication::EffectsApplication(MainMemory& memory, VideoStack* video, Selection selection,
                                       Services services) {
    if (selection.shadows || selection.distortion || selection.screen) {
        if (!video || !video->Scene() || !video->Resources() || !video->Pipe() || !services.capture_scene ||
            !services.reserve || !services.ordered || !services.checkpoint)
            throw std::invalid_argument("Effect selection requires the live shared Graphics owner");
        scene_ = std::make_unique<::title::EffectsScene>(*video->Scene(), *video->Resources(), *video->Pipe());
    }
    count_calls_ = services.count_calls;
    const auto capture = services.capture_scene;
    const auto reserve = services.reserve;
    shadows_ = std::make_unique<ShadowApplication>(memory, scene_.get(), selection.shadows, ShadowApplication::Output{
        services.read,
        [capture](auto& frame) { return capture && capture(21, frame); },
        [reserve](Work work) { return reserve && reserve(21, std::move(work)); },
        services.ordered, services.checkpoint, services.unlinked_open});
    auto environment = services.environment;
    if (!environment) environment = [](::title::MaterialState::Environment&) { return false; };
    distortion_ = std::make_unique<DistortionApplication>(memory, scene_.get(), selection.distortion,
        DistortionApplication::Output{std::move(environment),
            [capture](auto& frame) { return capture && capture(23, frame); },
            [reserve](Work work) { return reserve && reserve(23, std::move(work)); },
            services.ordered, services.checkpoint, services.unlinked_open});
    screen_ = std::make_unique<ScreenApplication>(memory, scene_.get(), selection.screen, ScreenApplication::Output{
        [capture](auto& frame) { return capture && capture(4, frame); },
        [reserve](Work work) { return reserve && reserve(4, std::move(work)); },
        services.ordered, services.checkpoint, services.unlinked_open});
}
EffectsApplication::~EffectsApplication() = default;
bool EffectsApplication::Selected() const {
    return shadows_->Selected() || distortion_->Selected() || screen_->Selected();
}
bool EffectsApplication::Install(const Capture& capture, const Installer& install) {
    if (!capture || !install) return false;
    const auto shadows = shadows_->Selected(), distortion = distortion_->Selected(), screen = screen_->Selected();
    if (!(shadows_->Install(capture, [&](uint32_t at, Body body) { return install(at, std::move(body), shadows); }) &&
          distortion_->Install(capture, [&](uint32_t at, Body body) { return install(at, std::move(body), distortion); }) &&
          screen_->Install(capture, [&](uint32_t at, Body body) { return install(at, std::move(body), screen); })))
        return false;
    // The cloth producers' execution census: the title's
    // four cloth entries counted at the dispatch through the root's audited
    // CountCalls wrapper, the original body unchanged (exact-entry support kept).
    // No wrapper (a unit composition) or no generated body (another image) leaves
    // the row at `installed=0`; it never refuses the leg.
    const uint32_t entries[4] = {::title::ClothProducers::kType17Packet, ::title::ClothProducers::kType17Update,
                                 ::title::ClothProducers::kType1APacket, ::title::ClothProducers::kType1AUpdate};
    cloth_census_installed_ = count_calls_ != nullptr;
    for (size_t i = 0; i < 4 && count_calls_; ++i) {
        cloth_calls_[i] = std::make_shared<std::atomic<uint64_t>>(0);
        Body original = capture(entries[i]);
        if (!original || !install(entries[i], count_calls_(entries[i], std::move(original), cloth_calls_[i]), true))
            cloth_census_installed_ = false;
    }
    return true;
}
bool EffectsApplication::Register(savestate::SectionRegistry& registry, const AddPreflight& preflight) {
    return shadows_->Register(registry, preflight) && distortion_->Register(registry, preflight);
}
bool EffectsApplication::CanCheckpoint(std::string* error) const {
    return shadows_->CanCheckpoint(error) && distortion_->CanCheckpoint(error) && screen_->CanCheckpoint(error);
}
void EffectsApplication::RegisterConditions(
    const std::function<void(const char*, std::function<bool(std::string*)>)>& add) {
    add("graphics_shadows", [this](std::string* reason) { return shadows_->CanCheckpoint(reason); });
    add("graphics_distortion", [this](std::string* reason) { return distortion_->CanCheckpoint(reason); });
    add("graphics_screen", [this](std::string* reason) { return screen_->CanCheckpoint(reason); });
}
void EffectsApplication::Record(verify::RunMeta& meta) const {
    shadows_->Record(meta);
    distortion_->Record(meta);
    screen_->Record(meta);
    const auto count = [this](size_t i) { return cloth_calls_[i] ? cloth_calls_[i]->load() : 0u; };
    meta.Set("graphics_cloth_producer", std::string("installed=") + (cloth_census_installed_ ? "1" : "0") +
                                            ",type17_packet=" + std::to_string(count(0)) +
                                            ",type17_update=" + std::to_string(count(1)) +
                                            ",type1a_packet=" + std::to_string(count(2)) +
                                            ",type1a_update=" + std::to_string(count(3)));
}
bool EffectsApplication::StopAfterQuiescence(std::string* error) {
    // Every owner stops before the shared scene is released with this object.
    const bool shadows = shadows_->StopAfterQuiescence(error);
    const bool distortion = distortion_->StopAfterQuiescence(error);
    const bool screen = screen_->StopAfterQuiescence(error);
    return shadows && distortion && screen;
}
}  // namespace ps2r::app
