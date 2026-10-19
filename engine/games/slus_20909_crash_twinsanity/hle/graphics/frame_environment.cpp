#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"

namespace title {
namespace {
using Environment = FrameEnvironment;
constexpr std::array<uint32_t, Environment::kBindingCount> addresses{
    Environment::kInitDisplayAddress, Environment::kWriteFrameHeadAddress,
    Environment::kSetUpFrameAddress, Environment::kFinishSkyAddress,
    Environment::kBeginShadowsAddress, Environment::kEndShadowsAddress,
    Environment::kLinkAddress, Environment::kResetGraphAddress};
}  // namespace

FrameEnvironment::FrameEnvironment(MaterialState::Read source) : source_(std::move(source)) {}

bool FrameEnvironment::ReadSceneSource(Boundary boundary, uint32_t argument, SceneFrame& out) const {
    if (!source_) return false;
    SceneFrame value;
    uint32_t shown_bytes = 0, draw_bytes = 0, color_page = 0, depth_page = 0;
    try {
        if (!source_(MaterialState::kDisplayWidth, &value.width, 4) ||
            !source_(MaterialState::kDisplayHeight, &value.height, 4) ||
            !source_(MaterialState::kDisplayPixelBytes, &shown_bytes, 4) ||
            !source_(kDrawPixelBytes, &draw_bytes, 4) ||
            !source_(kFrameBufferPage, &color_page, 4) ||
            !source_(MaterialState::kDepthBufferPage, &depth_page, 4)) return false;
        // NTSC 0x19f1c8 is the public SetUpFrame target ABI, not the portable
        // TR FrameStart struct. +18/+1c are target.displayWidth/displayHeight.
        if (boundary == Boundary::SetUpFrame) {
            std::array<uint32_t, 2> dimensions{};
            const uint32_t segment = argument >> 29;
            if (segment != 0 && segment != 4 && segment != 5) return false;
            argument &= 0x1fffffff;
            if (!argument || argument > 0x02000000 - 0x20 ||
                !source_(argument + 0x18, dimensions.data(), sizeof(dimensions)) ||
                dimensions[0] != value.width || dimensions[1] != value.height) return false;
        }
    } catch (...) { return false; }
    if (!value.width || value.width > 2048 || value.width % 64 ||
        !value.height || value.height > 2048 || shown_bytes != 2 || draw_bytes != 4)
        return false;
    const uint64_t pixels = uint64_t(value.width) * value.height;
    // InitDisplay, frame.cpp WriteCalledPacket/StartFrame, sky.cpp FinishSky,
    // and movie/player.cpp Draw must name the same ordinary CT32 scene.
    if (color_page != pixels * shown_bytes / 8192 ||
        depth_page != pixels * (shown_bytes + draw_bytes) / 8192 ||
        uint64_t(depth_page) * 8192 + pixels * 4 > 4 * 1024 * 1024)
        return false;
    value.color_buffer = color_page * 8192;
    value.depth_buffer = depth_page * 8192;
    out = value;
    return true;
}

bool FrameEnvironment::Install(const Capture& capture, const Installer& installer) {
    if (installed_) return true;
    if (!capture || !installer) return false;
    std::array<Body, kBindingCount> bodies;
    for (size_t i = 0; i < bodies.size(); ++i) {
        bodies[i] = capture(addresses[i]);
        if (!bodies[i]) return false;
    }
    originals_ = bodies;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (installer(addresses[i], [this, i](CpuContext& c, uint32_t entry) {
                Run(static_cast<Boundary>(i), c, entry);
            })) continue;
        while (i != 0) {
            --i;
            if (!installer(addresses[i], bodies[i]))
                throw std::runtime_error("FrameEnvironment dispatch rollback failed");
        }
        originals_ = {};
        return false;
    }
    installed_ = true;
    return true;
}

void FrameEnvironment::InvalidateFrame() {
    evidence_.frame = false;
    evidence_.shadow_open = false;
    scene_valid_ = false;
    ++invalidations_;
}

void FrameEnvironment::InvalidateAll() {
    InvalidateFrame();
    submitted_valid_ = false;
    sky_environment_known_ = false;
    evidence_.dither_matrix = false;
    environment_ = {};
}

void FrameEnvironment::EstablishFlatUi() {
    // TR materials.cpp:457-502 and draw2d.cpp:48-73,163-178 describe the flat
    // UI operation: the vertex colours mixed over the frame, no fog or texture.
    // Its native raster policy is full scanlines and ordinary Mix, without
    // per-pixel blend gating. These values are NOT inferred GS reset bits.
    environment_ = {};
    environment_.colour_clamp = true;
    environment_.per_pixel_alpha = false;
    environment_.scan_mask = 0;
    environment_.dither = 0;
    // Source frame.cpp:106's matrix, inactive in this undithered UI pass. It
    // need not be recovered from a restored model or witnessed at startup.
    environment_.dither_matrix = 0x6071243571603524ull;
    evidence_.frame = true;
    sky_environment_known_ = true;
    ++evidence_.completed_frame_boundaries;
}

void FrameEnvironment::Run(Boundary boundary, CpuContext& context, uint32_t entry) {
    const size_t index = static_cast<size_t>(boundary);
    const bool full_entry = entry == 0 || entry == addresses[index];
    const uint32_t argument = context.cpuRegs.GPR.r[4].UL[0];
    const bool primary_link = boundary == Boundary::Link &&
        (argument & 0x1fffffff) == kFrameBuckets &&
        (argument >> 29 == 0 || argument >> 29 == 4 || argument >> 29 == 5);
    const SceneFrame submitted = scene_;
    const bool can_submit = full_entry && primary_link && scene_valid_ &&
        CurrentStatus() == Status::Ready;
    if (!full_entry) {
        ++evidence_.interior_entries;
        InvalidateAll();
    } else if (boundary == Boundary::InitDisplay || boundary == Boundary::ResetGraph) {
        InvalidateAll();
    } else if (boundary == Boundary::Link) {
        InvalidateFrame();
    } else if (boundary == Boundary::SetUpFrame || boundary == Boundary::FinishSky) {
        evidence_.frame = false;
        scene_valid_ = false;
    } else if (boundary == Boundary::WriteFrameHead) {
        scene_valid_ = false;
        ++invalidations_;
    } else if (boundary == Boundary::BeginShadows) {
        evidence_.shadow_open = true;
    }
    const uint64_t before = invalidations_;
    SceneFrame candidate;
    const bool setup = boundary == Boundary::SetUpFrame || boundary == Boundary::FinishSky;
    const bool captured = full_entry && setup && ReadSceneSource(boundary, argument, candidate);
    // Nested/interior source operations cannot certify this producer. The
    // complete leaf SetUpFrame uses this tuple before recording bucket zero.
    const auto previous_setup = setup_scene_;
    const bool previous_valid = setup_valid_;
    const uint64_t previous_invalidations = setup_invalidations_;
    setup_valid_ = captured && boundary == Boundary::SetUpFrame && !active_operations_ &&
        !evidence_.shadow_open && source_epoch_ != std::numeric_limits<uint64_t>::max();
    if (setup_valid_) {
        setup_scene_ = candidate;
        setup_scene_.source_frame = source_frame_;
        setup_scene_.epoch = source_epoch_ + 1;
        setup_invalidations_ = invalidations_;
    }
    const bool bracket = full_entry &&
        (boundary == Boundary::BeginShadows || boundary == Boundary::EndShadows);
    ++active_operations_;
    if (bracket) ++shadow_bracket_operations_;
    try {
        originals_[index](context, entry);
    } catch (...) {
        --active_operations_;
        if (bracket) --shadow_bracket_operations_;
        setup_valid_ = false;
        InvalidateAll();
        throw;
    }
    --active_operations_;
    if (bracket) --shadow_bracket_operations_;
    setup_scene_ = previous_setup;
    setup_valid_ = previous_valid;
    setup_invalidations_ = previous_invalidations;
    if (!full_entry) {
        // A complete nested callee cannot certify the unfinished outer frame.
        InvalidateAll();
        return;
    }
    switch (boundary) {
    case Boundary::WriteFrameHead:
        // TR frame.cpp:104-106,130; NTSC 0x19aaa8. This is the completed
        // frame-head producer, not evidence that its DMA has already drained.
        environment_.dither_matrix = 0x6071243571603524ull;
        // The source head clamps colours and ends with dithering disabled.
        // Full scanlines/ordinary alpha remain the title frame intent, not
        // values read from retained device state.
        environment_.colour_clamp = true;
        environment_.dither = 0;
        evidence_.dither_matrix = true;
        sky_environment_known_ = before == invalidations_;
        break;
    case Boundary::SetUpFrame:
    case Boundary::FinishSky:
        // TR frame.cpp:187,199 / sky.cpp:176,182: bucket-zero frame setup
        // disables dithering and clamps colours, independently of clear flags.
        EstablishFlatUi();
        sky_environment_known_ = before == invalidations_;
        scene_valid_ = false;
        if (captured && before == invalidations_ &&
            source_epoch_ != std::numeric_limits<uint64_t>::max()) {
            candidate.source_frame = source_frame_;
            candidate.epoch = ++source_epoch_;
            scene_ = candidate;
            scene_valid_ = true;
        }
        break;
    case Boundary::Link:
        scene_valid_ = false;
        // Match GraphicsService's primary FrameBuckets identity, including
        // cached/uncached RAM aliases. Foreign small buckets are not frames.
        if (primary_link) {
            submitted_valid_ = can_submit && before == invalidations_;
            if (submitted_valid_) submitted_scene_ = submitted;
            ++source_frame_;
        }
        break;
    case Boundary::EndShadows:
        // TR shadows.cpp:192 disables clamp; :261 restores it in bucket 21.
        // UI is bucket 24. The paired operation must finish before claiming
        // this UI environment, even if a Strip is called during shadow setup.
        evidence_.shadow_open = false;
        environment_.colour_clamp = true;
        break;
    case Boundary::InitDisplay:
        scene_valid_ = false;
        ++evidence_.complete_display_initializations;
        break;
    case Boundary::ResetGraph:
        scene_valid_ = false;
        break;
    default:
        break;
    }
}

FrameEnvironment::Status FrameEnvironment::CurrentStatus() const {
    if (active_operations_ || evidence_.shadow_open) return Status::IncompleteOperation;
    if (!evidence_.frame) return Status::NoFrameBoundary;
    return Status::Ready;
}

bool FrameEnvironment::Read(MaterialState::Environment& out) const {
    if (CurrentStatus() != Status::Ready) return false;
    out = environment_;
    // Canonical inactive colour is valid only for MaterialState's fog-off case.
    out.fog_r = out.fog_g = out.fog_b = 0;
    return true;
}

bool FrameEnvironment::SceneBucket(uint32_t bucket) {
    // TR renderer.h:431: 0 sky (CaptureSky), 1 and 5 unused, 25 unused.
    return bucket != 0 && bucket != 1 && bucket != 5 && bucket != 25 && bucket < 28;
}

bool FrameEnvironment::CaptureScene(uint32_t bucket, SceneFrame& out) const {
    if (!SceneBucket(bucket) || !scene_valid_ || !evidence_.frame) return false;
    if (bucket == 21) {
        // TR shadows.cpp:83/218 record bucket 21 inside the bracket; the
        // bracket bodies themselves are the only admitted nested operation.
        if (active_operations_ != shadow_bracket_operations_) return false;
    } else if (CurrentStatus() != Status::Ready) {
        return false;
    }
    out = scene_;
    return true;
}

bool FrameEnvironment::EffectBucket(uint32_t bucket) {
    return bucket == 4 || bucket == 21 || bucket == 23;
}

bool FrameEnvironment::CaptureEffects(uint32_t bucket, SceneFrame& frame,
                                      MaterialState::Environment& environment) const {
    if (!EffectBucket(bucket)) return false;
    SceneFrame scene;
    if (!CaptureScene(bucket, scene)) return false;
    auto state = environment_;
    state.fog_r = state.fog_g = state.fog_b = 0; // material state, not frame state (header)
    frame = scene;
    environment = state;
    return true;
}

bool FrameEnvironment::MeshBucket(uint32_t bucket) {
    return bucket == 2 || bucket == 3 || (bucket >= 6 && bucket <= 19);
}

bool FrameEnvironment::CaptureMesh(uint32_t bucket, SceneFrame& frame,
                                   MaterialState::Environment& environment) const {
    if (!MeshBucket(bucket)) return false;
    SceneFrame scene;
    if (!CaptureScene(bucket, scene)) return false;
    auto state = environment_;
    state.fog_r = state.fog_g = state.fog_b = 0; // the material's own (header)
    frame = scene;
    environment = state;
    return true;
}

bool FrameEnvironment::CaptureSubmittedScene(SceneFrame& out) const {
    if (active_operations_ || !submitted_valid_) return false;
    out = submitted_scene_;
    return true;
}

bool FrameEnvironment::Linked() const { return source_frame_ != 0; }

bool FrameEnvironment::CaptureSetup(SceneFrame& out) const {
    if (!setup_valid_ || active_operations_ != 1 || setup_invalidations_ != invalidations_ ||
        setup_scene_.source_frame != source_frame_ ||
        source_epoch_ == std::numeric_limits<uint64_t>::max() || setup_scene_.epoch != source_epoch_ + 1)
        return false;
    out = setup_scene_;
    return true;
}

bool FrameEnvironment::CaptureSky(SceneFrame& out, MaterialState::Environment& environment) const {
    if (active_operations_ || evidence_.shadow_open || !sky_environment_known_ ||
        source_epoch_ == std::numeric_limits<uint64_t>::max() || !source_) return false;
    uint32_t target = 0;
    try {
        if (!source_(MaterialState::kRenderTarget, &target, sizeof(target))) return false;
    } catch (...) { return false; }
    SceneFrame candidate;
    if (!ReadSceneSource(Boundary::SetUpFrame, target, candidate)) return false;
    const uint64_t pixels = uint64_t(candidate.width) * candidate.height;
    if (candidate.width % 128 || candidate.height % 2 ||
        (pixels * 10 / 8192) * 8192 + pixels > 4 * 1024 * 1024) return false;
    candidate.source_frame = source_frame_;
    candidate.epoch = source_epoch_ + 1;
    auto state = environment_;
    state.dither = 0; // StartSkyBuffer's explicit source intent.
    state.fog_r = state.fog_g = state.fog_b = 0; // accepted type-A is fog-off.
    out = candidate;
    environment = state;
    return true;
}

FrameEnvironment::Evidence FrameEnvironment::Observed() const { return evidence_; }

bool FrameEnvironment::CaptureState(Snapshot& out, std::string* error) const {
    if (active_operations_ || evidence_.shadow_open) {
        if (error) *error = "Graphics source frame boundary is incomplete";
        return false;
    }
    Snapshot snapshot;
    snapshot.next_frame = source_frame_;
    snapshot.source_epoch = source_epoch_;
    snapshot.frame = evidence_.frame;
    snapshot.dither_known = evidence_.dither_matrix;
    snapshot.sky_known = sky_environment_known_;
    snapshot.scene_valid = scene_valid_;
    snapshot.submitted_valid = submitted_valid_;
    if (scene_valid_) snapshot.scene = scene_;
    if (submitted_valid_) snapshot.submitted = submitted_scene_;
    if (!FrameState::Validate(snapshot, error)) return false;
    out = snapshot;
    return true;
}
bool FrameEnvironment::RestoreState(const Snapshot& snapshot, std::string* error) {
    if (active_operations_ || evidence_.shadow_open) {
        if (error) *error = "Graphics source frame restore requires quiescence";
        return false;
    }
    if (!FrameState::Validate(snapshot, error)) return false;
    InvalidateAll();
    source_frame_ = snapshot.next_frame;
    // Future captured generations cannot collide with the abandoned timeline.
    source_epoch_ = std::max(source_epoch_, snapshot.source_epoch);
    scene_ = snapshot.scene; submitted_scene_ = snapshot.submitted;
    scene_valid_ = snapshot.scene_valid; submitted_valid_ = snapshot.submitted_valid;
    evidence_.frame = snapshot.frame; evidence_.dither_matrix = snapshot.dither_known;
    sky_environment_known_ = snapshot.sky_known;
    if (snapshot.frame || snapshot.dither_known || snapshot.sky_known) {
        environment_.colour_clamp = true;
        environment_.dither_matrix = 0x6071243571603524ull;
    }
    return true;
}

void FrameEnvironment::ResetAfterQuiescence() {
    if (active_operations_) throw std::logic_error("FrameEnvironment reset during a source operation");
    InvalidateAll();
    source_frame_ = 0;
}

}  // namespace title
