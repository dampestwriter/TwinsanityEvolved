#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/ee/cpu/cpu_state.h"

namespace ps2r::ipu { class GuestMemory; }

namespace title {

// The title's screen-shape term, owned (video.aspect). TR
// renderer.cpp derives ONE value -- `g_WidescreenTv != 0 ? WideAspect :
// NarrowAspect` -- and hands it to four consumers; this model answers the
// session's selected ratio at those four NTSC entries instead of the title's
// 4:3/16:9 flag (pal_ntsc_map rows, verified against the generated bodies):
//   * kProjection 0x0026e870 RenderView::Update(f32 aspect)   TR view.cpp:40
//       -- the argument ($f12) the projection's m[0][0] = cot(fov/2) /
//          (pixelAspect * aspect) is built from; the generated body runs.
//   * kFrustum    0x001eec80 SetViewFrustum(near, far, aspect, &fov)
//       TR culling.cpp:377 -- the culling planes' aspect ($f14); the generated
//       body (ViewFrustumPlanes x2) runs, so the culling frustum follows the
//       projection exactly -- nothing pops at the widened edges.
//   * kFitPlace   0x0019ffa0 FitPlaceToScreen(inPixels, anchor, place)
//   * kFitSize    0x001a0030 FitSizeToScreen(inPixels, size)
//       TR renderer.cpp:498-514 -- the HUD/widget fit: a place squeezed
//       towards its anchor by aspect * 0.75 (left/right anchors stay pinned,
//       centres stay centred), a size squeezed by the same -- the anamorphic
//       frame stretched to the TV restores them: no stretched HUD. Native,
//       with the EE's float operations in the title's order.
// The movie picture (TR movie.cpp:226 MoviePlayer::Draw) is MovieGraphics'
// (its View takes the ratio). The title's own flag byte (0x0030a744) is never
// written: its options page stays the player's.
//
// At 4:3 the ratio IS the title's narrow term: a headless 4:3/auto leg installs
// nothing (the deterministic lanes' identity); a windowed session at 4:3 passes
// every call to the generated body untouched (counted) until a live change.
// Interior entries (a restored continuation) go to the original.
class ScreenAspect {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using RatioSource = std::function<double()>;
    // The camera the projection entry updated ('s transform source):
    // called after every RenderView::Update -- owned or passed through -- with
    // the view's world->camera matrix (TR include/game/view.h `toCamera`, 16
    // f32 at view+0x40) and its camera->screen matrix (`cameraToScreen`,
    // view+0x100: projection . clipToScreen, as MakeMatrices left it for the
    // scene's target), on the game thread.
    using CameraObserver = std::function<void(const float* to_camera16, const float* camera_to_screen16)>;

    static constexpr uint32_t kProjection = 0x0026e870, kFrustum = 0x001eec80,
                              kFitPlace = 0x0019ffa0, kFitSize = 0x001a0030;
    // TR renderer.h: g_WidescreenTv (u8), g_RendererWidth/Height (s16) -- the
    // generated bodies read 0x310000-0x58bc / -0x5878 / -0x5876.
    static constexpr uint32_t kWidescreenTv = 0x0030a744, kRendererWidth = 0x0030a788,
                              kRendererHeight = 0x0030a78a;
    static constexpr uint32_t kViewToCamera = 0x40;   // RenderView::toCamera (CHECK_OFFSET: clip 0x140, 5 matrices before it)
    static constexpr uint32_t kViewCameraToScreen = 0x100;   // RenderView::cameraToScreen (the fifth matrix)
    static constexpr float kNarrow = 1.3333334f;   // 0x3faaaaab, TR NarrowAspect
    static constexpr float kWide = 1.7777778f;     // 0x3fe38e39, TR WideAspect
    static constexpr float kInverseNarrow = 0.75f; // TR renderer.cpp InverseNarrowAspect

    struct Counters {
        std::array<uint64_t, 4> owned{};        // calls answered with a non-4:3 ratio
        std::array<uint64_t, 4> passed{};       // calls at 4:3: the generated body untouched
        uint64_t interior = 0;                  // interior entries (delegated by the installer)
    };

    ScreenAspect(ps2r::ipu::GuestMemory& memory, RatioSource ratio);
    ~ScreenAspect();

    // Installs the four entries through `install` (the root wraps each body
    // with DelegateInterior over the captured original). False = a missing
    // original or a refused install (rolled back).
    bool Install(const Capture& capture, const Installer& install);
    bool Installed() const { return installed_; }
    void SetCameraObserver(CameraObserver observer) { camera_ = std::move(observer); }
    Counters Counts() const;
    std::string Census() const;

    // The pure fits (TR renderer.cpp:498-514 with the EE's operations), for
    // the ratio `aspect`: place.x = (place.x - anchor.x) / (aspect * 0.75) +
    // anchor.x; size.x = size.x / (aspect * 0.75); then x,y scaled by the
    // renderer's pixel size when `in_pixels`.
    static void FitPlace(float aspect, bool in_pixels, float anchor_x, int16_t width, int16_t height,
                         float& place_x, float& place_y);
    static void FitSize(float aspect, bool in_pixels, int16_t width, int16_t height,
                        float& size_x, float& size_y);
    // The f32 the title would pass: 16:9 is exactly WideAspect.
    static float Term(double ratio) { return static_cast<float>(ratio); }

private:
    void Dispatch(size_t index, CpuContext& context, uint32_t entry);
    void OwnFitPlace(CpuContext& context, float aspect);
    void OwnFitSize(CpuContext& context, float aspect);
    void PublishCamera(uint32_t view);

    ps2r::ipu::GuestMemory& memory_;
    RatioSource ratio_;
    CameraObserver camera_;
    std::array<Body, 4> originals_;
    std::array<std::atomic<uint64_t>, 4> owned_{}, passed_{};
    std::atomic<uint64_t> interior_{0};
    bool installed_ = false;
};

}  // namespace title
