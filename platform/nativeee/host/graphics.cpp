// platform/nativeee (host side) -- Platform::Graphics over NativeEE's typed-draw surface (engine ps2e_draw.h), the
// slice: the renderer's start, the frame (StartFrame's clear, the 28 buckets' order, Present, the vertical blank the
// game thread waits for), the UI's 2D sprites and strips, the fonts' glyph quads (from the wasm side's Font::Draw) and the
// textures (decoded from the game's resources by gs_texture.cpp). Everything the game hands over arrives as a u32 guest
// address; materials are read from guest memory in the te_gfx_layout.h layout the wasm side writes.
//
// Where graphics.h expects packet building (Present's DMA chain, AllocFrameMemory, the particle blocks, SetUpRenderTarget's
// packet) the function is implemented by its MEANING over typed draws. The scene
// (culling, rigid/placed models, meshes, LOD, sky, shadows) is graphics_scene.cpp over the shared pieces of
// graphics_internal.h; the later slices (skins, particles, decals, screen models, shader animation, screen effects) are
// explicit no-ops here that log once and return the value that keeps the game going;
// the particles (graphics_particles.cpp) and the decals (graphics_decals.cpp) have their own modules.
//
// Environment (judges, CI):   TE_GFX_HEADLESS=1   hidden window (presentation still renders for capture)
//                             TE_GFX_DUMP_DIR=d   write presented frames as PPM into d ...
//                             TE_GFX_DUMP_EVERY=n ... every n-th present (default 0 = none)
//                             TE_GFX_DUMP_AT=k    ... and the k-th present
//                             TE_GFX_DUMP_FROM=a / TE_GFX_DUMP_TO=b  ... the every-n dumps limited to pictures a..b
//                             TE_GFX_LOG=1        log every seam call once
//                             TE_GFX_SUMMARY_EVERY=n  print the counters every n presents (default 600; 0 = off)
//                             PS2E_DRAW_STENCIL_DIAG=1  the engine's stencil histogram at the shadow clear / composite
//                             TE_GFX_NO_SHADOWS=1 the volumes counted but never composited (the caster judge's reference)
//                             TE_GFX_NO_DECALS=1  the decal pass records nothing (the decal judge's reference; graphics_decals.cpp)
//                             TE_GFX_PRESENT_CT16=1 the retail present: the frame shown as the dithered 16-bit copy (--present-ct16)
//                             TE_DECAL_DIAG=1     the decal view / every aged decal (TE_DECAL_DIAG_FROM=present) / the quads drawn
//                             TE_GFX_SHADOW_NO_DATE=1 the volumes counted without the frame-alpha test (the shadow-caster A/B)
//                             TE_GFX_INTERP=0/1   the frames between two pictures at 120/144/240/uncapped interpolated (1) or the
//                                                 newest picture repeated (0); TE_GFX_DUMP_INTERP=1 dumps the interpolated frames
//                                                 too (frame_<picture>_i<fraction in thousandths>.ppm) beside the pictures' dumps
//
// Pictures and presents. A PICTURE is a frame the game produced (one per Present with draws; always 60 a second in play
// mode: the game's tick is the FieldPacer's). A PRESENT is a frame shown on the window. At 60 they are the same frame; at 30
// every other picture is shown (steady); at 120 / 144 / 240 the render thread presents at that cadence (the engine's
// PresentPacer on the host's finest timer) and the frames between two pictures are interpolated by the engine's
// SceneInterpolator (or the newest picture repeated when interpolation is off); uncapped presents as fast as it can with
// the swap interval forced off. The pictures' hashes and dumps are the same at every rate: a picture the cadence does not
// show is RESOLVED (ps2e_draw_capture_frame) when a judge's capture is armed. Deterministic and synchronous runs present
// every picture whatever the setting (there is no pacer clock to interpolate on).
#include <algorithm>
#include <atomic>
#include <type_traits>
#if defined(__vita__)
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
#endif
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "engine/platform/host/platform.h"
#include "graphics_internal.h"
#include "gs_texture.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_gfx_layout.h"
#include "te_display.h"
#include "te_graphics_hooks.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_services.h"

namespace te::gfx { void RegisterStateSection(); }

namespace {

// ---- the game's structs as they lie in guest memory (include/platform/graphics.h, include/game/*.h) -----------------
struct FrameStart {
    int32_t x, y, width, height, screenWidth, screenHeight;
    uint8_t red, green, blue;
    uint8_t clearColor, clearDepth;
};
struct Rectangle {
    float x, y, width, height;
};
struct Vector2 {
    float x, y;
};
using te::gfx::RenderTargetDescription;
using te::gfx::MaterialView;
using te::gfx::Draw;
using te::gfx::DeferredBuild;
// Retail globals the PS2 renderer read (game/symbol_addrs.txt, PAL SLES-52568): the current render target pointer
// (G_FontRendererRel) and the game's screen size (D_00309AE4/8).
constexpr uint32_t kRenderTargetPointer = 0x00309BDC;
constexpr uint32_t kScreenWidth = 0x00309AE4, kScreenHeight = 0x00309AE8;
// renderer.h g_DisplayWidth/g_DisplayHeight (retail G_HorizontalResolution/G_VerticalResolution): the PS2 arm's
// InitDisplay wrote them at StartRenderer; game code reads them (movie.cpp's picture area, particles.cpp's aspect).
constexpr uint32_t kDisplayWidth = 0x0030AAE4, kDisplayHeight = 0x0030AAE8;
// GS coordinates: the screen's middle at 2048 pixels, 16 sub-pixels
constexpr float kGsMiddle = 2048.0f;
constexpr float kGsDepthFront = 16777215.0f;   // the 2D draws' GsFrontDepth, clamped to Z24

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T)));
}

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
long EnvLong(const char* name, long fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atol(v) : fallback;
}

struct Gfx {
    ps2e_draw* dev = nullptr;
    ps2e_draw_presentation presentation{};   // the display settings, set before or after the device exists
    bool create_failed = false;
    bool pal = false;
    int display_w = 512, display_h = 448;
    FrameStart frame{};
    bool have_frame = false;
    // The frame's buckets: `used` draws of `draws` are this frame's; the rest keep their vertex storage for the
    // next frames (ClearBuckets only resets `used`), so a steady frame records without allocating
    struct Bucket {
        std::vector<Draw> draws;
        size_t used = 0;
    };
    Bucket buckets[TE_GFX_BUCKET_COUNT];
    // The frame's deferred builds (ReserveDeferred): `deferred_used` of `deferred` are this frame's; they travel with the
    // buckets at the hand-over (their addresses are what the placeholders and the workers hold)
    std::vector<std::unique_ptr<DeferredBuild>> deferred;
    size_t deferred_used = 0;
    std::unordered_map<uint32_t, uint32_t> textures;   // GameTexture guest address -> ps2e_draw texture
    // The texture resources as the game handed them (header + upload packet): what a save state carries so a loaded
    // process can decode them again (the game frees the packet's disk-manager block after the upload, as the PS2 did).
    struct TextureSource { te_gfx_texture_header header; std::vector<uint8_t> packet; };
    std::unordered_map<uint32_t, TextureSource> texture_sources;
    // The counters the render thread writes are atomic: the summary and the save state read them from the game thread
    std::atomic<uint64_t> presents{0}, dropped_draws{0}, draws_submitted{0}, presents_with_draws{0}, frame_draws{0},
        frame_triangles{0};
    uint64_t vblanks = 0, draws = 0, texture_failures = 0;
    uint64_t draws_merged = 0;   // draws appended to the bucket's last draw (draws_submitted: handed to the backend)
    double last_submit_ms = 0, last_present_ms = 0, last_pump_ms = 0;   // the last Present's phases (the clock's slow-tick line reads them)
    // The render thread: in PLAY mode with a window the frame's draws are submitted and presented off the game
    // thread -- the game thread hands over its buckets at Present (the two sets of bucket storage swap: no copy) and
    // continues; the GL work (backend submit ~2.5 ms, the swap, and the window server's stalls under host load) is the
    // render thread's. One frame in flight: a Present that finds the previous frame still drawing waits for it. Every
    // other GL call (textures, resize, captures) runs on the render thread through OnRender, synchronously.
    // Deterministic / headless / dumping runs keep the synchronous path (their frames are judged by present number).
    struct Frame {
        Bucket buckets[TE_GFX_BUCKET_COUNT];
        std::vector<std::unique_ptr<DeferredBuild>> deferred;
        size_t deferred_used = 0;
        bool send = false;
        int screen_w = 0, screen_h = 0;
        uint64_t harness_key = 0;                 // the boundary this present precedes (te_harness.h's key)
        uint64_t picture = 0;                     // the picture's ordinal (1-based, as the game produced them)
        bool have_edge = false;                   // the pacer edge of the field this picture follows (play mode)
        int64_t edge_ns = 0;
        uint64_t field = 0;
        ps2r::ps2e::InterpCamera camera;          // the game's camera this picture was drawn with (the interpolator's)
        uint32_t camera_object = 0;               // the game's camera object: another one = a cut
        std::vector<ps2r::ps2e::InterpItem> items; // the picture's draws as the interpolator reads them (built on the render thread)
        std::vector<uint32_t> retire;              // native meshes retired before this picture: destroyed when its slot is free
    };
    struct RenderThread {
#if defined(__vita__)
        static constexpr int kSlots = 3;           // the Vita (no interpolation, a fixed heap): the two pictures kept + the one
                                                   // the game thread hands over while the newest is drawn
#else
        static constexpr int kSlots = 5;           // the two pictures the presenter keeps (newest and the one before, for the
                                                   // frames between them) + one frame drawing + up to two queued: a late frame
                                                   // (a swap the window server held, a preempted render thread) is absorbed
#endif
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake, done;
        bool enabled = false, started = false, stop = false;
        bool drawing = false;
        Frame slots[kSlots];
        std::vector<int> queue, free;              // slot indices: queued in order; free for the game thread
        int cur = -1, prev = -1;                   // the presenter's two retained pictures (slot indices; -1 none)
        std::vector<std::function<void()>> jobs;   // OnRender's, run before the next frame / when idle
        uint64_t frames = 0, waits = 0;            // frames drawn; Presents that found no free slot and waited
        double wait_ms = 0, wait_max_ms = 0;       // the game thread's time in those waits
        uint64_t jobs_posted = 0, jobs_done = 0;
        // The presenter: the cadence (te_display's frame rate, taken at the next picture), the pacer, its census
        ps2r::ps2e::PresentPacer pacer;
        int pacer_rate = -1;                       // the rate the pacer is set to (-1: not yet)
        bool interpolate = true;                   // TE_GFX_INTERP (the frames between two pictures; off = the newest repeated)
        bool dump_interp = false;                  // TE_GFX_DUMP_INTERP
        bool probe_half = false;                   // TE_GFX_INTERP_PROBE: at every picture's arrival the half-way frame of (previous,
                                                   // newest) is built and dumped as frame_<picture>_i500.ppm (the quality judge's
                                                   // triplets, independent of the pacer's slots and the host's speed)
        uint64_t interp_frames = 0, snaps = 0;     // frames interpolated; frames shown as the newest picture because of a cut
        double latency_sum_ms = 0, latency_max_ms = 0;   // present instant - the shown content's time, over the presents
        uint64_t latency_n = 0;
        uint64_t interp_bytes = 0;                 // the interpolated frame's vertex storage (the one extra scene copy)
        int target_slot = -1;                      // what the frame target holds: a picture's slot at fraction 1, or an
        double target_fraction = -1;               // interpolated frame (a re-present of the same content skips the submit)
        uint64_t represents = 0;                   // presents that re-showed the frame target as it stood (no submit)
        ps2r::ps2e::SceneInterpolator interpolator;   // the engine's: the frame between two pictures
        std::vector<ps2r::ps2e::InterpOutput> interp_out;
        bool interp_diag = false;                  // TE_GFX_INTERP_DIAG: the matrices' consistency and the census per frame
        uint64_t last_noted_picture = 0;
        double interp_ms_sum = 0, submit_ms_sum = 0, present_ms_sum = 0;   // the interpolated frame's phases (the census)
        uint64_t phase_n = 0;
        // The pictures' arrival ages (arrival instant - the picture's tick edge, ns) over the last 64 pictures, and the
        // DELAY the fraction's clock runs behind: a picture is usable for the frames between it and the one before only
        // once it has arrived, so the shown content runs one tick plus `delay` behind the newest edge, with delay = the
        // ages' 90th percentile (how long after its edge a picture arrives: the game thread's frame work), capped at a tick
        int64_t arrival_ages[64] = {};
        int arrival_count = 0;
        int64_t delay_ns = 0;
        double delay_ms_max = 0;
        ~RenderThread() {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; }
            wake.notify_all();
            if (thread.joinable()) thread.join();
        }
    } rt;
    std::atomic<int> frame_rate{1};                // te_display's setting (0 = 30 .. 4 = 240, 5 = uncapped), read by the render thread
    std::atomic<bool> interp_active{false};        // the presenter interpolates (slot-paced + interpolation on): draws of different tags stay apart
    ps2r::ps2e::InterpTag tag;                     // the tag OpenDraw stamps (SetDrawTag / ClearDrawTag; the game thread's)
    ps2r::ps2e::InterpTransform tag_transform{};
    bool tag_has_transform = false;
    std::atomic<uint64_t> pictures{0};             // game pictures taken by the presenter (every Present with draws), shown or not
    uint64_t pictures_handed = 0;                  // game pictures handed over (the game thread's count: the ordinals)
    std::chrono::steady_clock::time_point first_present{};
    bool have_first_present = false;
    uint64_t start_frames = 0, clears = 0, sprites = 0, turned_sprites = 0, strips = 0, text_quads = 0, textures_loaded = 0;
    uint64_t scene_mark[4] = {0, 0, 0, 0};
    bool summary_armed = false;
    // Options, read from the environment on the first seam call (not at load: a harness may set them in main)
    bool options_read = false;
    bool headless = false, log = false;
    bool present_ct16 = false;   // the dithered 16-bit present (te::Settings / TE_GFX_PRESENT_CT16; the hook toggles it)
    std::string dump_dir;
    long dump_every = 0, dump_at = 0, summary_every = 600;
    // TE_HARNESS_KEYS=a,b,c: the harness leg hashes only the pictures preceding these boundaries (the capture armed for
    // them alone) -- a paced run's cadence is then unperturbed by the per-present read-back; the other keys log hash 0
    std::vector<uint64_t> harness_keys;
    long dump_from = 0, dump_to = 0;   // TE_GFX_DUMP_FROM / TE_GFX_DUMP_TO: the pictures dumped (every dump_every) lie in [from, to]; 0 = unbounded
    std::vector<uint32_t> mesh_retired;   // native meshes retired since the last hand-over (RetireMesh)
    int native_meshes = -1;               // -1 unknown, 0 off (setting or a device without them), 1 on
} g;

void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used);
void ExpandBucket(Gfx::Bucket* buckets, int b);

void ReadOptions();
std::string PresentCensus();
void Once(const char* what) {
    static std::unordered_map<std::string, bool> said;
    ReadOptions();
    if (!g.log) return;
    bool& s = said[what];
    if (!s) {
        s = true;
        std::fprintf(stderr, "[te.gfx] %s\n", what);
    }
}

void Summary() {
    std::fprintf(stderr,
                 "[te.gfx] summary: presents %llu (with draws %llu), vblanks %llu, StartFrame %llu (clears %llu), sprites %llu, "
                 "turned %llu, strips %llu, text quads %llu, draws %llu (merged %llu, submitted %llu, dropped %llu), textures %llu (failed %llu); scene: rigid %llu, "
                 "placed %llu, sky %llu, shadows %llu, triangles %llu, models decoded %llu (refused %llu, missing %llu), culls %llu; "
                 "skins %llu (blend %llu, triangles %llu, decoded %llu, refused %llu; %.0f ms); shadow passes %llu (odd colours %llu; %.0f ms); particle blocks %llu (triangles %llu, refused %llu); decals aged %llu (culled %llu, blocks %llu, quads %llu, refused %llu); screen models %llu (triangles %llu); material passes %llu; "
                 "render thread %s (frames %llu, waits %llu, %.1f ms, max %.1f)\n",
                 (unsigned long long)g.presents.load(), (unsigned long long)g.presents_with_draws.load(), (unsigned long long)g.vblanks,
                 (unsigned long long)g.start_frames, (unsigned long long)g.clears, (unsigned long long)g.sprites,
                 (unsigned long long)g.turned_sprites, (unsigned long long)g.strips, (unsigned long long)g.text_quads,
                 (unsigned long long)g.draws, (unsigned long long)g.draws_merged, (unsigned long long)g.draws_submitted.load(),
                 (unsigned long long)g.dropped_draws.load(), (unsigned long long)g.textures_loaded,
                 (unsigned long long)g.texture_failures, (unsigned long long)te::gfx::Scene().rigid_models,
                 (unsigned long long)te::gfx::Scene().placed_models, (unsigned long long)te::gfx::Scene().sky_models,
                 (unsigned long long)te::gfx::Scene().shadow_meshes, (unsigned long long)te::gfx::Scene().model_triangles,
                 (unsigned long long)te::gfx::Scene().models_decoded, (unsigned long long)te::gfx::Scene().models_refused,
                 (unsigned long long)te::gfx::Scene().submodels_missing, (unsigned long long)te::gfx::Scene().culls,
                 (unsigned long long)te::gfx::Scene().skins, (unsigned long long)te::gfx::Scene().blend_skins,
                 (unsigned long long)te::gfx::Scene().skin_triangles, (unsigned long long)te::gfx::Scene().skins_decoded,
                 (unsigned long long)te::gfx::Scene().skins_refused, te::gfx::Scene().skin_ms, (unsigned long long)te::gfx::Scene().shadow_passes,
                 (unsigned long long)te::gfx::Scene().shadow_odd_colours, te::gfx::Scene().shadow_ms,
                 (unsigned long long)te::gfx::Scene().particle_blocks, (unsigned long long)te::gfx::Scene().particle_triangles,
                 (unsigned long long)te::gfx::Scene().particle_refused,
                 (unsigned long long)te::gfx::Scene().decals_aged, (unsigned long long)te::gfx::Scene().decals_culled,
                 (unsigned long long)te::gfx::Scene().decal_blocks, (unsigned long long)te::gfx::Scene().decal_quads,
                 (unsigned long long)te::gfx::Scene().decal_refused,
                 (unsigned long long)te::gfx::Scene().screen_models, (unsigned long long)te::gfx::Scene().screen_model_triangles,
                 (unsigned long long)te::gfx::Scene().material_passes,
                 g.rt.enabled ? "on" : "off", (unsigned long long)g.rt.frames, (unsigned long long)g.rt.waits, g.rt.wait_ms,
                 g.rt.wait_max_ms);
    std::fprintf(stderr, "[te.gfx] %s\n", PresentCensus().c_str());
}

// ---- the device --------------------------------------------------------------------------------------------------------
void ReadOptions() {
    if (g.options_read) return;
    te::gfx::RegisterStateSection();
    g.options_read = true;
    g.headless = EnvFlag("TE_GFX_HEADLESS");
    g.log = EnvFlag("TE_GFX_LOG");
    g.dump_dir = std::getenv("TE_GFX_DUMP_DIR") ? std::getenv("TE_GFX_DUMP_DIR") : "";
    g.dump_every = EnvLong("TE_GFX_DUMP_EVERY", 0);
    g.dump_at = EnvLong("TE_GFX_DUMP_AT", 0);
    g.dump_from = EnvLong("TE_GFX_DUMP_FROM", 0);
    if (const char* k = std::getenv("TE_HARNESS_KEYS")) {
        for (const char* p = k; *p;) {
            char* end = nullptr;
            const long long v = std::strtoll(p, &end, 10);
            if (end == p) break;
            if (v > 0) g.harness_keys.push_back(uint64_t(v));
            p = *end == ',' ? end + 1 : end;
        }
    }
    g.dump_to = EnvLong("TE_GFX_DUMP_TO", 0);
    g.summary_every = EnvLong("TE_GFX_SUMMARY_EVERY", 600);
    // the render thread is on in play mode with a window and no frame dumps; TE_GFX_RENDER_THREAD=0/1 decides
    const bool play = !te::GetSettings().deterministic;
    g.rt.enabled = play && !g.headless && g.dump_dir.empty();
    if (const char* v = std::getenv("TE_GFX_RENDER_THREAD")) g.rt.enabled = v[0] == '1';
    g.present_ct16 = te::GetSettings().present_ct16 || EnvFlag("TE_GFX_PRESENT_CT16");
    if (const char* v = std::getenv("TE_GFX_INTERP")) g.rt.interpolate = v[0] != '0';
    g.rt.dump_interp = EnvFlag("TE_GFX_DUMP_INTERP");
    g.rt.probe_half = EnvFlag("TE_GFX_INTERP_PROBE");
    g.rt.interp_diag = EnvFlag("TE_GFX_INTERP_DIAG");
    {
        // the interpolator's displacement check (two passes over every vertex per frame) runs for the judges
        // (TE_GFX_INTERP_CHECK=1, or any diagnosis), not for a player's frame budget
        auto opt = g.rt.interpolator.GetOptions();
        opt.check_displacement = EnvFlag("TE_GFX_INTERP_CHECK") || g.rt.interp_diag;
        opt.trace_violations = g.rt.interp_diag && EnvFlag("TE_GFX_INTERP_TRACE");
        g.rt.interpolator.SetOptions(opt);
    }
}

// ---- the render thread --------------------------------------------------------------------------------------------
enum class Show { Present, Capture, None };   // a frame shown; resolved for the armed capture only; dropped
void RenderFrame(Gfx::Frame& f, Show show);
void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns);
void ProbeHalf(Gfx::Frame& prev, Gfx::Frame& cur);
using Steady = std::chrono::steady_clock;
int64_t NowNs() { return ps2r::ps2e::PresentPacer::NowNs(); }
bool CaptureArmed() { return !g.dump_dir.empty() || (te::harness::Armed() && g.harness_keys.empty()); }
// A keyed harness (TE_HARNESS_KEYS): this picture is one of the wanted ones
bool HarnessWants(uint64_t key) {
    if (g.harness_keys.empty()) return te::harness::Armed();
    for (uint64_t k : g.harness_keys) if (k == key) return true;
    return false;
}
using Rate = ps2r::ps2e::PresentPacer::Rate;

// The presenter's picture bookkeeping on the render thread (the mutex held): the arrived slot becomes the newest
// picture, the one before it the previous; the one before that goes back to the game thread.
void RetainPicture(int slot) {
    auto& rt = g.rt;
    if (rt.prev >= 0) {
        // no picture the presenter can still show draws a mesh retired before this one
        for (uint32_t mesh : rt.slots[rt.prev].retire) ps2e_draw_mesh_destroy(g.dev, mesh);
        rt.slots[rt.prev].retire.clear();
        rt.free.push_back(rt.prev);
    }
    rt.prev = rt.cur;
    rt.cur = slot;
}

// The shown content's time (the pacer's clock) against the present instant: the latency census
void NoteLatency(int64_t content_ns, int64_t now_ns) {
    auto& rt = g.rt;
    const double ms = double(now_ns - content_ns) / 1e6;
    rt.latency_sum_ms += ms;
    rt.latency_n++;
    if (ms > rt.latency_max_ms) rt.latency_max_ms = ms;
}

void RenderLoop() {
    auto& rt = g.rt;
    // with the game thread: interactive work (the engine's per-platform arm: macOS QoS, Windows SetThreadPriority)
    if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-render", ps2r::platform::host::ThreadRole::Graphics);
    if (!ps2e_draw_bind_thread(g.dev, 1)) std::fprintf(stderr, "[te.gfx] render thread: the GL context did not bind\n");
    ps2r::host::RaiseTimerResolution();   // the slot trains at 120 / 144 / 240 (Windows: the 1 ms period + the waitable timer)
    for (;;) {
        std::unique_lock<std::mutex> lock(rt.mutex);
        // The cadence: the setting as it stands at this picture (the pacer re-set when it changed)
        const int rate_index = g.frame_rate.load(std::memory_order_relaxed);
        if (rate_index != rt.pacer_rate) {
            rt.pacer_rate = rate_index;
            rt.pacer.SetRate(static_cast<Rate>(rate_index));
            g.interp_active.store(rt.interpolate && ps2r::ps2e::PresentPacer::PacedBySlots(static_cast<Rate>(rate_index)), std::memory_order_relaxed);
            rt.interpolator.ResetHistory();
            std::fprintf(stderr, "[te.gfx] presenter: frame rate %s%s\n", ps2r::ps2e::PresentPacer::Name(static_cast<Rate>(rate_index)),
                         ps2r::ps2e::PresentPacer::PacedBySlots(static_cast<Rate>(rate_index)) ? (rt.interpolate ? " (the frames between two pictures interpolated)" : " (the newest picture repeated between pictures)") : "");
        }
        const Rate rate = static_cast<Rate>(rt.pacer_rate);
        const bool slots = ps2r::ps2e::PresentPacer::PacedBySlots(rate) && rt.cur >= 0 && rt.slots[rt.cur].have_edge;
        // Wait for work: a job, a picture, or (slot-paced) the next slot's time less a millisecond
        if (slots) {
            const int64_t slot = rt.pacer.NextSlotNs(NowNs());
            const int64_t wake_at = slot - 1000000;
            rt.wake.wait_until(lock, Steady::time_point(std::chrono::nanoseconds(wake_at)),
                               [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty() || NowNs() >= wake_at; });
        } else {
            rt.wake.wait(lock, [&] { return rt.stop || !rt.queue.empty() || !rt.jobs.empty(); });
        }
        if (rt.stop) break;   // (the slot cadence would otherwise present forever)
        if (!rt.jobs.empty()) {
            std::vector<std::function<void()>> jobs = std::move(rt.jobs);
            rt.jobs.clear();
            lock.unlock();
            for (auto& job : jobs) job();
            lock.lock();
            rt.jobs_done += jobs.size();
            rt.done.notify_all();
            continue;
        }
        if (!rt.queue.empty()) {
            // A picture arrived
            const int slot = rt.queue.front();
            rt.queue.erase(rt.queue.begin());
            rt.drawing = true;
            RetainPicture(slot);
            Gfx::Frame& f = rt.slots[slot];
            if (f.have_edge) {   // its age, and the delay the fraction's clock keeps (the 90th percentile over the last 64)
                rt.arrival_ages[rt.arrival_count % 64] = NowNs() - f.edge_ns;
                rt.arrival_count++;
                const int n = std::min(rt.arrival_count, 64);
                int64_t sorted[64];
                std::copy(rt.arrival_ages, rt.arrival_ages + n, sorted);
                std::sort(sorted, sorted + n);
                const int64_t period = 1000000000LL / 60;
                const int64_t p90 = sorted[(n * 9) / 10 >= n ? n - 1 : (n * 9) / 10];
                // the delay = the arrival age (its 90th percentile): the shown content then runs exactly one picture plus the
                // arrival behind the newest EDGE, so every slot between two arrivals falls inside the pair (no frame shown twice)
                // -- the one tick of latency the interpolation costs, and no more (clamped at a period)
                rt.delay_ns = std::clamp<int64_t>(p90, 0, period);
                rt.delay_ms_max = std::max(rt.delay_ms_max, double(rt.delay_ns) / 1e6);
            }
            lock.unlock();
            // the frame's builds, done by the workers meanwhile: a picture presented as it arrives takes them bucket by
            // bucket as it submits (SubmitBuckets), the others here
            static const bool xNoLazy = std::getenv("TE_X_NO_LAZY") != nullptr;   // XBISECT
            const bool lazy = !xNoLazy && !ps2r::ps2e::PresentPacer::PacedBySlots(rate) && rt.pacer.ShowsPicture(f.picture);
            if (!lazy) ExpandDeferred(f.buckets, f.deferred, f.deferred_used);
            if (ps2r::ps2e::PresentPacer::PacedBySlots(rate)) {
                // the cadence is the slot train's: the picture is resolved for a judge's capture only (its hash and dump are
                // the same at every rate); the slots show it, and the frames between it and the previous one
                if (f.have_edge) rt.pacer.OnTickEdge(f.edge_ns);
                RenderFrame(f, CaptureArmed() ? Show::Capture : Show::None);
                if (rt.probe_half && rt.prev >= 0 && rt.slots[rt.prev].send && f.send) ProbeHalf(rt.slots[rt.prev], f);
            } else if (rt.pacer.ShowsPicture(f.picture)) {
                // 60 (every picture) and the shown half of 30: the picture presented as it arrives
                const int64_t now = NowNs();
                RenderFrame(f, Show::Present);
                ExpandDeferred(f.buckets, f.deferred, f.deferred_used);   // any left (a picture not sent)
                if (f.have_edge) NoteLatency(f.edge_ns, now);
            } else {
                RenderFrame(f, CaptureArmed() ? Show::Capture : Show::None);   // 30: the picture between two shown ones
            }
            lock.lock();
            rt.drawing = false;
            rt.frames++;
            rt.done.notify_all();
            continue;
        }
        if (slots) {
            const int64_t now = NowNs();
            const int64_t slot_ns = rt.pacer.NextSlotNs(now);
            if (now < slot_ns) {
                lock.unlock();
                ps2r::ps2e::PresentPacer::SleepUntilNs(slot_ns);   // the last millisecond, finely
                lock.lock();
                if (!rt.queue.empty() || !rt.jobs.empty() || rt.stop) continue;   // a picture arrived meanwhile: it first
            }
            // The frame for this slot: between the previous picture and the newest at the fraction of the pacer's clock --
            // fraction = (now - edge of the newest) / period, clamped to [0, 1]: the shown content is one tick behind the
            // newest picture (the latency the interpolation costs), never ahead of it
            const int cur = rt.cur, prev = rt.prev;
            rt.drawing = true;
            lock.unlock();
            const int64_t t = NowNs();
            const int64_t period = 1000000000LL / 60;   // the game's tick (the FieldPacer's hz is the clock's; 60 by charter)
            double fraction = 1.0;
            if (rt.interpolate && prev >= 0 && rt.slots[prev].have_edge && rt.slots[cur].send && rt.slots[prev].send) {
                fraction = double(t - rt.delay_ns - rt.slots[cur].edge_ns) / double(period);
                if (fraction < 0.0) fraction = 0.0;
                if (fraction > 1.0) fraction = 1.0;
            }
            if (rt.interp_diag && rt.slots[cur].picture % 200 == 0)
                std::fprintf(stderr, "[te.gfx.interp] slot: picture %llu edge %s age %.2f ms fraction %.3f prev %d send %d/%d\n", (unsigned long long)rt.slots[cur].picture,
                             rt.slots[cur].have_edge ? "yes" : "NO", rt.slots[cur].have_edge ? double(t - rt.slots[cur].edge_ns) / 1e6 : -1.0, fraction, prev,
                             int(prev >= 0 && rt.slots[prev].send), int(rt.slots[cur].send));
            if (fraction >= 0.97) fraction = 1.0;   // within a third of a millisecond of the newest picture: the picture itself
            RenderBetween(prev >= 0 ? rt.slots[prev] : rt.slots[cur], rt.slots[cur], fraction, slot_ns);
            lock.lock();
            rt.pacer.SlotServed(slot_ns, t);
            rt.drawing = false;
            rt.done.notify_all();
            continue;
        }
        if (rt.stop) break;
    }
    ps2e_draw_bind_thread(g.dev, 0);
}
bool OnRenderThread() { return g.rt.started && std::this_thread::get_id() == g.rt.thread.get_id(); }
// Runs fn on the render thread and waits for it (inline when there is none, or when already on it)
void OnRender(const std::function<void()>& fn) {
    auto& rt = g.rt;
    if (!rt.started || OnRenderThread()) { fn(); return; }
    uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(rt.mutex);
        rt.jobs.push_back(fn);
        ticket = ++rt.jobs_posted;
    }
    rt.wake.notify_one();
    std::unique_lock<std::mutex> lock(rt.mutex);
    rt.done.wait(lock, [&] { return rt.jobs_done >= ticket; });
}
void StartRenderThread() {
    auto& rt = g.rt;
    if (!rt.enabled || rt.started || !g.dev) return;
    if (!ps2e_draw_bind_thread(g.dev, 0)) {   // no context to hand over (a headless device without GL): stay synchronous
        rt.enabled = false;
        return;
    }
    for (int i = 0; i < Gfx::RenderThread::kSlots; ++i) rt.free.push_back(i);
    rt.started = true;
    rt.thread = std::thread(RenderLoop);
}

void PrimeThreadSync();
bool EnsureDevice() {
    ReadOptions();
    if (!g.summary_armed) {
        g.summary_armed = true;
        std::atexit(Summary);
    }
    if (g.dev) return true;
    if (g.create_failed) return false;
    ps2e_draw_config cfg{};
    cfg.frame_width = g.display_w;
    cfg.frame_height = g.display_h;
    // twice the frame's lines in the TV's 4:3 (the shape `auto` shows in a window); the window fits it to the screen
    cfg.window_height = g.display_h * 2;
    cfg.window_width = (cfg.window_height * 4 + 1) / 3;
    cfg.headless = g.headless ? 1 : 0;
    cfg.title = "TwinsanityEvolved";
    // the threads' locks made before the device's memory is (the Vita: below)
    PrimeThreadSync();
    // PS2E_DRAW_STENCIL_DIAG=1 (the cryscave flicker diagnosis): the engine logs the stencil's histogram at the shadow bracket's
    // clear and composite. The toggle lives here because the engine reads no environment outside its config loader.
    cfg.stencil_diag = EnvFlag("PS2E_DRAW_STENCIL_DIAG") ? 1 : 0;
    g.dev = ps2e_draw_create(&cfg);
    if (!g.dev) {
        g.create_failed = true;
        std::fprintf(stderr, "[te.gfx] ps2e_draw_create failed: no window / GL context; graphics calls are dropped\n");
        return false;
    }
    te::display::Apply();   // the display settings read before the device existed
    std::fprintf(stderr, "[te.gfx] %s\n", te::display::Describe().c_str());
    StartRenderThread();
    if (g.present_ct16) te_graphics_set_present_ct16(1);
    // the judges' dumps / the harness leg read the present back (a keyed harness arms the capture per picture instead)
    if (!g.dump_dir.empty() || (te::harness::Armed() && g.harness_keys.empty())) OnRender([] { ps2e_draw_arm_capture(g.dev, 1); });
    std::fprintf(stderr, "[te.gfx] renderer started: frame %dx%d (%s), %s, render thread %s\n", g.display_w, g.display_h,
                 g.pal ? "PAL" : "NTSC", g.headless ? "headless" : "window", g.rt.started ? "on" : "off");
    return true;
}

// The frame's screen size the game told StartFrame (the frame target), and the GS origin in frame pixels
int ScreenW() { return g.have_frame && g.frame.screenWidth > 0 ? g.frame.screenWidth : g.display_w; }
int ScreenH() { return g.have_frame && g.frame.screenHeight > 0 ? g.frame.screenHeight : g.display_h; }
float GsOriginX() { return kGsMiddle - float(ScreenW() >> 1); }
float GsOriginY() { return kGsMiddle - float(ScreenH() >> 1); }

// The current render target (the game's g_RenderTarget) or null
const RenderTargetDescription* CurrentTarget() {
    const uint32_t* p = Guest<uint32_t>(kRenderTargetPointer);
    if (!p || !*p) return nullptr;
    const auto* t = Guest<RenderTargetDescription>(*p);
    if (!t || t->displayWidth <= 0 || t->displayWidth > 4096 || t->displayHeight <= 0 || t->displayHeight > 4096) return nullptr;
    return t;
}
// draw2d.cpp FrameCorner: the frame's corner along a side, in GS pixels
float FrameCornerX(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayWidth : g.frame.screenWidth;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
float FrameCornerY(int offset) {
    const RenderTargetDescription* t = CurrentTarget();
    const int display = t ? t->displayHeight : g.frame.screenHeight;
    return float(int(kGsMiddle - float(display >> 1)) + offset);
}
int TargetOffsetX() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetX : 0;
}
int TargetOffsetY() {
    const RenderTargetDescription* t = CurrentTarget();
    return t ? t->offsetY : 0;
}
// The game's g_ScreenWidth/Height (the UI's scale), the frame's when unset
float GameScreenW() {
    const int32_t* p = Guest<int32_t>(kScreenWidth);
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenW());
}
float GameScreenH() {
    const int32_t* p = Guest<int32_t>(kScreenHeight);
    return p && *p > 0 && *p <= 4096 ? float(*p) : float(ScreenH());
}

// ---- materials -> render state --------------------------------------------------------------------------------------------
MaterialView ReadMaterial(uint32_t address) {
    MaterialView v;
    v.material = Guest<te_gfx_material>(address);
    if (!v.material || v.material->magic != TE_GFX_MATERIAL_MAGIC) {
        v.material = nullptr;
        return v;
    }
    v.bucket = v.material->bucket < TE_GFX_BUCKET_COUNT ? v.material->bucket : TE_GFX_BUCKET_UI;
    if (v.material->shaderCount && v.material->shaders[0]) {
        v.shader = Guest<te_gfx_shader>(v.material->shaders[0]);
        if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    }
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        auto it = g.textures.find(v.shader->texture);
        if (it != g.textures.end()) v.texture_handle = it->second;
    }
    return v;
}

MaterialView ShaderView(const MaterialView& m, uint32_t k) {
    MaterialView v;
    v.material = m.material;
    v.bucket = m.bucket;
    if (!v.material || k >= v.material->shaderCount || k >= TE_GFX_MAX_SHADERS || !v.material->shaders[k]) return v;
    v.shader = Guest<te_gfx_shader>(v.material->shaders[k]);
    if (v.shader && v.shader->magic != TE_GFX_SHADER_MAGIC) v.shader = nullptr;
    if (v.shader && v.shader->texture) {
        v.texture = Guest<te_gfx_texture>(v.shader->texture);
        auto it = g.textures.find(v.shader->texture);
        if (it != g.textures.end()) v.texture_handle = it->second;
    }
    return v;
}

void AlphaPreset(uint32_t preset, ps2e_draw_state& st) {
    // InitAlphaPresets (shadersettings.cpp): (A - B) * C + D over Cs=0 Cd=1 Zero=2 / As=0 Ad=1 FIX=2
    switch (preset) {
        case TE_GFX_PRESET_MIX: st.blend_a = 0; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_ADD: st.blend_a = 0; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SUBTRACT: st.blend_a = 2; st.blend_b = 0; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_BRIGHTEN: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_DARKEN: st.blend_a = 2; st.blend_b = 1; st.blend_c = 0; st.blend_d = 1; break;
        case TE_GFX_PRESET_SCALE: st.blend_a = 1; st.blend_b = 2; st.blend_c = 0; st.blend_d = 2; break;
        default: st.blend_a = 0; st.blend_b = 2; st.blend_c = 2; st.blend_d = 2; st.blend_fix = 0x80; break;   // the source as it is
    }
}

// The first shader's settings as the draw state (2D: the texture when the shader has one)
// A decoded texture as a device texture with its levels (on the drawing thread)
uint32_t CreateTexture(const te::gfx::DecodedTexture& decoded) {
    const uint32_t handle = ps2e_draw_texture_create_gpu_alpha(g.dev, decoded.width, decoded.height, decoded.rgba.data());
    static const bool xNoMips = std::getenv("TE_X_NO_MIPS") != nullptr;   // XBISECT
    if (handle && !decoded.mips.empty() && !xNoMips) {
        std::vector<const void*> levels;
        for (const auto& level : decoded.mips) levels.push_back(level.data());
        ps2e_draw_texture_set_mips(g.dev, handle, int(levels.size()), levels.data());
    }
    return handle;
}

// The shader types whose settings minify through a texture's mips (shadersettings.cpp: the standard styles and the 2D
// shader's own; the unfiltered ones, the sky and type 0 do not)
bool MinifiesThroughMips(uint32_t type) {
    switch (type) {
        case 0x01: case 0x02: case 0x04: case 0x0D: case 0x14: case 0x15: case 0x16: case 0x17: case 0x19:
        case 0x1A: case 0x1B: case 0x1E: case 0x1F: case 0x20:
            return true;
        default:
            return false;
    }
}

ps2e_draw_state StateOf(const MaterialView& m) {
    ps2e_draw_state st{};
    st.gouraud = 1;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 0;
    st.tcc = 1;
    if (!m.shader) return st;
    const uint64_t s = m.shader->settings;
    st.gouraud = TE_GFX_S_GOURAUD(s) ? 1 : 0;
    st.depth_test = int(TE_GFX_S_DEPTH_TEST(s));
    st.depth_write = TE_GFX_S_NO_DEPTH_WRITES(s) ? 0 : 1;
    st.blend = TE_GFX_S_BLENDS(s) ? 1 : 0;
    if (st.blend) {
        if (TE_GFX_S_OWN_ALPHA(s)) {
            st.blend_a = uint8_t(TE_GFX_S_ALPHA_A(s));
            st.blend_b = uint8_t(TE_GFX_S_ALPHA_B(s));
            st.blend_c = uint8_t(TE_GFX_S_ALPHA_C(s));
            st.blend_d = uint8_t(TE_GFX_S_ALPHA_D(s));
            st.blend_fix = uint8_t(TE_GFX_S_ALPHA_FIX(s));
        } else {
            AlphaPreset(TE_GFX_S_PRESET(s), st);
        }
    }
    st.alpha_test = TE_GFX_S_ALPHA_TEST(s) ? 1 : 0;
    st.alpha_method = int(TE_GFX_S_ALPHA_METHOD(s));
    st.alpha_reference = uint8_t(TE_GFX_S_ALPHA_REFERENCE(s));
    st.alpha_fail = int(TE_GFX_S_ALPHA_FAIL(s));
    st.linear = TE_GFX_S_LINEAR(s) ? 1 : 0;
    // the material's FBA (shadersettings.cpp FrameBufferAlpha: FBA = !noFba) marks the frame's alpha top bit --
    // the shadows' volumes are counted only where it is 0 (StartShadows' DATE), so a caster material with FBA is never
    // darkened (most casters write alpha 0x80 anyway; the lit world writes 0x7E)
    st.fba = TE_GFX_S_NO_FBA(s) ? 0 : 1;
    // TE_GFX_DIAG_FBA=1: each shader's settings once (type, FBA, blend, DATE) -- which materials receive shadows
    static const bool diagFba = std::getenv("TE_GFX_DIAG_FBA") != nullptr;
    if (diagFba) {
        static std::unordered_map<const te_gfx_shader*, bool> said;
        if (!said[m.shader]) {
            said[m.shader] = true;
            std::fprintf(stderr, "[te.gfx] shader %p type %#x settings %#llx: fba %d blends %d preset %llu date %llu/%llu textured %d tcc %d\n",
                         (const void*)m.shader, unsigned(m.shader->type), (unsigned long long)s, st.fba, st.blend,
                         (unsigned long long)TE_GFX_S_PRESET(s), (unsigned long long)TE_GFX_S_DESTINATION_TEST(s),
                         (unsigned long long)TE_GFX_S_DESTINATION_MODE(s), TE_GFX_S_TEXTURED(s) ? 1 : 0,
                         m.texture ? (m.texture->texture.hasAlpha ? 1 : 0) : -1);
        }
    }
    if (TE_GFX_S_TEXTURED(s) && m.texture_handle) {
        st.texture = m.texture_handle;
        st.tfx = m.texture ? (m.texture->texture.function & 3) : PS2E_DRAW_TFX_MODULATE;
        st.tcc = m.texture ? (m.texture->texture.hasAlpha ? 1 : 0) : 1;
        st.wrap_u = st.wrap_v = PS2E_DRAW_WRAP_REPEAT;
        // TEX1 as shadersettings.cpp wrote it: a mipmapped texture minifies LINEAR_MIPMAP_NEAREST through its levels
        // (MXL its last, the shader's K and L, the LOD per pixel from Q) under the standard settings and the 2D
        // shader's; the unfiltered, sky and type-0 settings minify with the magnifying filter, level 0 only
        if (m.texture && m.texture->texture.levels > 1 && MinifiesThroughMips(m.shader->type)) {
            st.mip_max_level = m.texture->texture.levels - 1;
            st.mip_k = m.shader->lodK;
            st.mip_l = m.shader->lodL;
            st.mip_minify = 4;   // GS_TEX_LINEAR_MIPMAP_NEAREST
            if (m.shader->type == 0x0D) st.linear = 1;   // the 2D shader magnifies linear whatever its settings say
        }
    }
    return st;
}

ps2e_draw_vertex Vertex(float x, float y, float u, float v, uint32_t colour) {
    ps2e_draw_vertex o{};
    o.x = x;
    o.y = y;
    o.z = kGsDepthFront;
    o.u = u;
    o.v = v;
    o.r = uint8_t(colour);
    o.g = uint8_t(colour >> 8);
    o.b = uint8_t(colour >> 16);
    o.a = uint8_t(colour >> 24);
    return o;
}

// two draws merge when every field the backend reads agrees (field-wise: the struct has padding)
bool SameState(const ps2e_draw_state& a, const ps2e_draw_state& b) {
    if (a.stencil != b.stencil) return false;
    if (a.fba != b.fba || a.dest_alpha_test != b.dest_alpha_test) return false;
    if (a.mip_max_level != b.mip_max_level || a.mip_k != b.mip_k || a.mip_l != b.mip_l || a.mip_minify != b.mip_minify) return false;
    return a.texture == b.texture && a.gouraud == b.gouraud && a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
           a.blend == b.blend && a.blend_a == b.blend_a && a.blend_b == b.blend_b && a.blend_c == b.blend_c &&
           a.blend_d == b.blend_d && a.blend_fix == b.blend_fix && a.alpha_test == b.alpha_test &&
           a.alpha_method == b.alpha_method && a.alpha_reference == b.alpha_reference && a.alpha_fail == b.alpha_fail &&
           a.linear == b.linear && a.wrap_u == b.wrap_u && a.wrap_v == b.wrap_v && a.tfx == b.tfx && a.tcc == b.tcc &&
           a.scissor_x0 == b.scissor_x0 && a.scissor_y0 == b.scissor_y0 && a.scissor_x1 == b.scissor_x1 &&
           a.scissor_y1 == b.scissor_y1 && a.no_colour_write == b.no_colour_write;
}

Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame = false) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    g.draws++;
    // a draw asking for a fresh frame copy never joins the previous one (its copy would predate that draw's triangles)
    if (b.used && state.blend == 0 && !copy_frame) {
        Draw& last = b.draws[b.used - 1];
        if (last.verts.size() >= 3 && SameState(last.state, state)) {
            // while the presenter interpolates, a draw joins the previous one only within the same object (a rigid object's
            // submodels: one identity) -- two objects in one draw could not be moved apart
            const bool same_object = !g.interp_active.load(std::memory_order_relaxed) ||
                                     (last.tag.id == g.tag.id && last.tag.kind == g.tag.kind &&
                                      (g.tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == g.tag.part));
            if (same_object) {
                g.draws_merged++;
                return last;
            }
        }
    }
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    d.state = state;
    d.verts.clear();
    d.copy_frame = copy_frame;
    d.pending = nullptr;
    d.mesh = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    return d;
}

void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) {
    g.tag = tag;
    g.tag_has_transform = world16 && screen16;
    if (g.tag_has_transform) {
        std::memcpy(g.tag_transform.world, world16, sizeof g.tag_transform.world);
        std::memcpy(g.tag_transform.screen, screen16, sizeof g.tag_transform.screen);
    }
}
void ClearDrawTag() {
    g.tag = ps2r::ps2e::InterpTag{};
    g.tag_has_transform = false;
}

void CloseDraw(uint32_t bucket) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (!b.used) return;
    Draw& d = b.draws[b.used - 1];
    d.verts.resize(d.verts.size() / 3 * 3);   // whole triangles only (a merged tail must not shift the next draw's)
    if (d.verts.size() < 3) {
        d.verts.clear();
        b.used--;
        g.draws--;
    }
}

void Record(uint32_t bucket, Draw&& d) {
    if (d.verts.size() < 3) return;
    Draw& open = OpenDraw(bucket, d.state);
    open.verts.insert(open.verts.end(), d.verts.begin(), d.verts.end());
    CloseDraw(bucket);
}

// ---- the builds off the game thread (graphics_internal.h DeferredBuild) ---------------------------------------------------
// A few workers (TE_GFX_BUILD_THREADS; by default the logical processors less the game, render and audio threads, from one
// to four, none on a machine of two) take the posted builds in order.
// The build pool's lock and its two conditions. On the Vita they are the kernel's lightweight mutex and conditions with
// their work areas IN the pool (a static, in .bss): vitaGL maps the whole newlib heap into the GPU's view, and Vita3K's
// tracking of the GPU's pages made the heap page under a pthread mutex unreadable mid-game (the pool's lock faulted at
// the hub's loading). Elsewhere the standard ones.
#if defined(__vita__)
class PoolMutex {
public:
    PoolMutex() { sceKernelCreateLwMutex(&work_, "te-build-pool", 0, 0, nullptr); }
    ~PoolMutex() { sceKernelDeleteLwMutex(&work_); }
    PoolMutex(const PoolMutex&) = delete;
    PoolMutex& operator=(const PoolMutex&) = delete;
    void lock() { sceKernelLockLwMutex(&work_, 1, nullptr); }
    void unlock() { sceKernelUnlockLwMutex(&work_, 1); }
    bool try_lock() { return sceKernelTryLockLwMutex(&work_, 1) == 0; }
    SceKernelLwMutexWork* Work() { return &work_; }
private:
    alignas(8) SceKernelLwMutexWork work_;
};
class PoolCondition {
public:
    explicit PoolCondition(PoolMutex& m) { sceKernelCreateLwCond(&work_, "te-build-pool-cond", 0, m.Work(), nullptr); }
    ~PoolCondition() { sceKernelDeleteLwCond(&work_); }
    PoolCondition(const PoolCondition&) = delete;
    PoolCondition& operator=(const PoolCondition&) = delete;
    void wait(std::unique_lock<PoolMutex>&) { sceKernelWaitLwCond(&work_, nullptr); }
    template <typename Pred>
    void wait(std::unique_lock<PoolMutex>& lock, Pred pred) { while (!pred()) wait(lock); }
    void wait_for(std::unique_lock<PoolMutex>&, std::chrono::milliseconds ms) {
        SceUInt32 us = SceUInt32(ms.count() * 1000);
        sceKernelWaitLwCond(&work_, &us);
    }
    void notify_one() { sceKernelSignalLwCond(&work_); }
    void notify_all() { sceKernelSignalLwCondAll(&work_); }
private:
    alignas(8) SceKernelLwCondWork work_;
};
#else
using PoolMutex = std::mutex;
class PoolCondition : public std::condition_variable {
public:
    explicit PoolCondition(PoolMutex&) {}
};
#endif

struct BuildPool {
    std::vector<std::thread> threads;
    PoolMutex mutex;
    PoolCondition wake{mutex}, finished{mutex};
    std::deque<std::function<void()>> jobs;
    size_t active = 0;
    bool stop = false, started = false;
    std::atomic<int64_t> waited_ns{0}, built_ns{0};   // the waiting thread's time in Wait / Drain; the builds' own time
    void Start() {
        started = true;
        int n;
        if (const char* v = std::getenv("TE_GFX_BUILD_THREADS")) {
            n = std::max(0, std::min(8, std::atoi(v)));
        } else {
            int cpus = int(std::thread::hardware_concurrency());
            n = cpus >= 3 ? std::max(1, std::min(4, cpus - 3)) : 0;
#if defined(__vita__)
            // the three cores an application gets, shared by the game thread, the render thread and two builders (the
            // builders take what the other two leave)
            n = 2;
#endif
        }
        for (int i = 0; i < n; ++i) threads.emplace_back([this, i] { Run(i); });
        if (n) std::fprintf(stderr, "[te.gfx] geometry builds: %d worker thread%s\n", n, n == 1 ? "" : "s");
    }
    void Run(int index) {
        char name[24];
        std::snprintf(name, sizeof name, "te-build-%d", index);
        if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive(name, ps2r::platform::host::ThreadRole::Graphics);
        std::unique_lock<PoolMutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stop || !jobs.empty(); });
            if (stop) return;
            std::function<void()> job = std::move(jobs.front());
            jobs.pop_front();
            ++active;
            lock.unlock();
            Timed(job);
            lock.lock();
            --active;
            finished.notify_all();
        }
    }
    void Timed(std::function<void()>& job) {
        const auto t0 = std::chrono::steady_clock::now();
        job();
        built_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                           std::memory_order_relaxed);
    }
    // A thread that needs the builds done runs the queued ones itself rather than sleep while the workers catch up;
    // it sleeps only once nothing is left to start (or when it may not run them: `help` false). `ready` is checked with
    // the lock held.
    template <typename Ready>
    void HelpUntil(Ready ready, bool help) {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<PoolMutex> lock(mutex);
        while (!ready()) {
            if (jobs.empty() || !help) {
                finished.wait(lock);
                continue;
            }
            std::function<void()> job = std::move(jobs.front());
            jobs.pop_front();
            ++active;
            lock.unlock();
            Timed(job);
            lock.lock();
            --active;
            finished.notify_all();
        }
        waited_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                            std::memory_order_relaxed);
    }
    void Post(std::function<void()> job) {
        { std::lock_guard<PoolMutex> lock(mutex); jobs.push_back(std::move(job)); }
        wake.notify_one();
    }
    void Wait(const DeferredBuild& build) {
        if (build.done.load(std::memory_order_acquire)) return;
        // the render thread only waits: a build run deep in its frame's call chain outgrew a console thread's stack
        HelpUntil([&] { return build.done.load(std::memory_order_acquire); }, !OnRenderThread());
    }
    void Drain() {
        if (threads.empty()) return;
        HelpUntil([&] { return jobs.empty() && active == 0; }, !OnRenderThread());
    }
    ~BuildPool() {
        { std::lock_guard<PoolMutex> lock(mutex); stop = true; }
        wake.notify_all();
        for (auto& t : threads) t.join();
    }
};
BuildPool& Builds() {
    static BuildPool pool;
    if (!pool.started) pool.Start();
    return pool;
}
// The Vita's pthread locks are made at their first use (a small allocation behind each). Made after vitaGL has set up
// its memory, one can land beside memory the GPU reads, where Vita3K's tracking of the GPU's pages fails the lock's
// exclusive store (the build pool's mutex did, mid-game). The render and build threads' locks are therefore used once
// here, before the device exists; elsewhere this costs a lock and a few notifies.
void PrimeThreadSync() {
    const auto prime = [](auto& m, auto& a, auto& b) {
        std::unique_lock<std::remove_reference_t<decltype(m)>> lock(m);
        a.wait_for(lock, std::chrono::milliseconds(0));
        b.wait_for(lock, std::chrono::milliseconds(0));
        lock.unlock();
        a.notify_all();
        b.notify_all();
    };
    BuildPool& pool = Builds();
    prime(pool.mutex, pool.wake, pool.finished);
    prime(g.rt.mutex, g.rt.wake, g.rt.done);
}

bool DeferringBuilds() { return !Builds().threads.empty(); }

DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    d.state = state;
    d.verts.clear();
    d.copy_frame = false;
    d.mesh = 0;
    d.tag = g.tag;
    d.has_transform = g.tag_has_transform;
    if (g.tag_has_transform) d.transform = g.tag_transform;
    if (g.deferred_used == g.deferred.size()) g.deferred.push_back(std::make_unique<DeferredBuild>());
    DeferredBuild& build = *g.deferred[g.deferred_used++];
    build.state = state;
    build.verts.clear();
    build.segments.clear();
    build.alternate.clear();
    build.done.store(false, std::memory_order_relaxed);
    d.pending = &build;
    return build;
}

void PostBuild(DeferredBuild& build, std::function<void()> job) {
    std::fenv_t env;
    std::fegetenv(&env);
    BuildPool& pool = Builds();
    pool.Post([&pool, &build, env, job = std::move(job)] {
        std::fenv_t saved;
        std::fegetenv(&saved);
        std::fesetenv(&env);
        job();
        std::fesetenv(&saved);
        { std::lock_guard<PoolMutex> lock(pool.mutex); build.done.store(true, std::memory_order_release); }
        pool.finished.notify_all();
    });
}

void DrainBuilds() { Builds().Drain(); }

// OpenDraw's joining rule for a draw of this state and tag appended after `out`'s last draw
bool Joins(const Gfx::Bucket& out, const ps2e_draw_state& state, bool copy_frame, const ps2r::ps2e::InterpTag& tag, bool interp) {
    if (!out.used || state.blend != 0 || copy_frame) return false;
    const Draw& last = out.draws[out.used - 1];
    if (last.verts.size() < 3 || !SameState(last.state, state)) return false;
    return !interp || (last.tag.id == tag.id && last.tag.kind == tag.kind &&
                       (tag.kind != ps2r::ps2e::InterpKind::Vertices || last.tag.part == tag.part));
}
Draw& NextDraw(Gfx::Bucket& out) {
    if (out.used == out.draws.size()) out.draws.emplace_back();
    return out.draws[out.used++];
}

// The frame's placeholders replaced by what their builds recorded, every bucket replayed through the joining rule (the draw
// list the game thread would have recorded building them itself); waits for the frame's builds
void ExpandDeferred(Gfx::Bucket* buckets, std::vector<std::unique_ptr<DeferredBuild>>& deferred, size_t& used) {
    if (!used) return;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) ExpandBucket(buckets, b);
    for (size_t i = 0; i < used; ++i) Builds().Wait(*deferred[i]);
    used = 0;
}

// One bucket's placeholders replaced by their builds' draws, each build waited for as it is reached (the drawing thread
// draws a bucket whose builds are done while the workers finish the next one's)
void ExpandBucket(Gfx::Bucket* buckets, int b) {
    const bool interp = g.interp_active.load(std::memory_order_relaxed);
    thread_local Gfx::Bucket scratch[TE_GFX_BUCKET_COUNT];   // each bucket's own: storage does not wander between buckets
    {
        Gfx::Bucket& in = buckets[b];
        Gfx::Bucket& out = scratch[b];
        bool any = false;
        for (size_t i = 0; i < in.used && !any; ++i) any = in.draws[i].pending != nullptr;
        if (!any) return;
        out.used = 0;
        for (size_t i = 0; i < in.used; ++i) {
            Draw& d = in.draws[i];
            if (!d.pending) {
                if (!d.mesh && Joins(out, d.state, d.copy_frame, d.tag, interp)) {
                    Draw& last = out.draws[out.used - 1];
                    last.verts.insert(last.verts.end(), d.verts.begin(), d.verts.end());
                } else {
                    std::swap(NextDraw(out), d);
                }
                continue;
            }
            const DeferredBuild& build = *d.pending;
            Builds().Wait(build);
            size_t at = 0;
            for (size_t k = 0; k < build.segments.size(); ++k) {
                const uint32_t count = build.segments[k];
                const ps2e_draw_state& state = k < build.alternate.size() && build.alternate[k] ? build.alt_state : build.state;
                const auto first = build.verts.begin() + std::ptrdiff_t(at), end = first + std::ptrdiff_t(count);
                at += count;
                if (Joins(out, state, false, d.tag, interp)) {
                    Draw& last = out.draws[out.used - 1];
                    last.verts.insert(last.verts.end(), first, end);
                } else {
                    Draw& o = NextDraw(out);
                    o.state = state;
                    o.verts.assign(first, end);
                    o.copy_frame = false;
                    o.pending = nullptr;
                    o.tag = d.tag;
                    o.has_transform = d.has_transform;
                    if (d.has_transform) o.transform = d.transform;
                }
            }
            d.pending = nullptr;
        }
        std::swap(in.draws, out.draws);
        std::swap(in.used, out.used);
    }
}

// A quad of GS-pixel corners (the GS origin taken off) as two triangles
// A sprite showing a whole texture along an axis samples it clamped there: drawn above the PS2's resolution its edge pixels
// fall between texels, and a repeating texture's filter then mixes in the opposite edge -- the lines between the tiles of
// the loading pictures. At the native resolution a pixel's centre is a texel's and the clamp changes nothing.
void ClampWholeTexture(ps2e_draw_state& st, float u0, float v0, float u1, float v1) {
    if (std::min(u0, u1) == 0.f && std::max(u0, u1) == 1.f) st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    if (std::min(v0, v1) == 0.f && std::max(v0, v1) == 1.f) st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
}

void RecordQuad(uint32_t bucket, const ps2e_draw_state& st, float gx0, float gy0, float gx1, float gy1, float u0, float v0,
                float u1, float v1, uint32_t colour) {
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = gx0 - ox, y0 = gy0 - oy, x1 = gx1 - ox, y1 = gy1 - oy;
    Draw d;
    d.state = st;
    ClampWholeTexture(d.state, u0, v0, u1, v1);
    d.verts = {Vertex(x0, y0, u0, v0, colour), Vertex(x1, y0, u1, v0, colour), Vertex(x0, y1, u0, v1, colour),
               Vertex(x1, y0, u1, v0, colour), Vertex(x1, y1, u1, v1, colour), Vertex(x0, y1, u0, v1, colour)};
    ::Record(bucket, std::move(d));
}

// ---- the frame boundary -------------------------------------------------------------------------------------------------
void PumpWindow() {
    const auto t0 = std::chrono::steady_clock::now();
    const bool closed = g.dev && ps2e_draw_pump(g.dev);
    g.last_pump_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (closed) {
        std::fprintf(stderr, "[te.gfx] window closed: exit\n");
        te::RequestExit(0);
    }
}

// One vertical blank: THE frame boundary, where the one game thread meets the clock (te::Clock::Boundary, the clock contract:
// the pace park at the pacer's field rate, the field tick, the EmuDomain settle, the boundary hooks -- the pads' sample,
// later the movie's present callback and the scheduler drain). Exactly one Boundary() per WaitVSync / Present /
// PresentFromInterrupt, in the place sceGsSyncV had (the services' boundary contract). The window is pumped here
// too. TE_GFX_UNPACED only matters when the clock is deterministic/unpaced itself (te::Settings): the pacing is the
// clock's, not the graphics side's.
void VBlank() {
    g.vblanks++;
    PumpWindow();
    te::Services::Get().clock().Boundary();
}

// The storage kept for the next frames is held to what the frame just drawn needed: the k-th draw of a bucket holds a
// different draw every frame (a merged run of a whole scene one frame, a sprite the next), so storage left at its largest
// grows towards (draws x the largest draw ever recorded) in every frame set -- the title's logo took the sets past 1 GB,
// and the Vita's heap with them. An oversized buffer is replaced by fresh storage of the size it held (its content is
// spent); the draws past the frame's own keep nothing, and only a few of them are kept at all.
template <typename Vertices>
void TrimStorage(Vertices& verts, size_t keep) {
    if (verts.capacity() <= keep * 2 + 64) return;
    Vertices fresh;
    fresh.reserve(keep);
    verts.swap(fresh);
}

void ClearBuckets() {
    constexpr size_t kSpareDraws = 64;
    for (auto& b : g.buckets) {
        if (b.draws.size() > b.used + kSpareDraws) b.draws.resize(b.used + kSpareDraws);
        for (size_t i = 0; i < b.draws.size(); ++i) TrimStorage(b.draws[i].verts, i < b.used ? b.draws[i].verts.size() : 0);
        b.used = 0;   // the storage stays
    }
    for (size_t i = 0; i < g.deferred.size(); ++i) {
        DeferredBuild& build = *g.deferred[i];
        const bool held = i < g.deferred_used;
        TrimStorage(build.verts, held ? build.verts.size() : 0);
        TrimStorage(build.segments, held ? build.segments.size() : 0);
    }
    g.deferred_used = 0;
}

// The pictures' dumps, numbered by PICTURE (= by present in a synchronous run: the frozen judges' numbering)
void MaybeDump(uint64_t picture) {
    if (g.dump_dir.empty() || !g.dev) return;
    const bool at = g.dump_at > 0 && picture == uint64_t(g.dump_at);
    const bool in_window = (g.dump_from <= 0 || picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || picture <= uint64_t(g.dump_to));
    const bool every = g.dump_every > 0 && in_window && picture % uint64_t(g.dump_every) == 0;
    if (!at && !every) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g.dump_dir.c_str(), static_cast<unsigned long long>(picture));
    // The frame's own counts follow its path (the judges' draws-per-frame floor reads them)
    if (ps2e_draw_write_ppm(g.dev, path))
        std::fprintf(stderr, "[te.gfx] frame %llu -> %s (draws %llu, triangles %llu, scene rigid %llu placed %llu sky %llu shadows %llu)\n",
                     static_cast<unsigned long long>(picture), path, (unsigned long long)g.frame_draws.load(), (unsigned long long)g.frame_triangles.load(),
                     (unsigned long long)(te::gfx::Scene().rigid_models - g.scene_mark[0]), (unsigned long long)(te::gfx::Scene().placed_models - g.scene_mark[1]),
                     (unsigned long long)(te::gfx::Scene().sky_models - g.scene_mark[2]), (unsigned long long)(te::gfx::Scene().shadow_meshes - g.scene_mark[3]));
    g.scene_mark[0] = te::gfx::Scene().rigid_models;
    g.scene_mark[1] = te::gfx::Scene().placed_models;
    g.scene_mark[2] = te::gfx::Scene().sky_models;
    g.scene_mark[3] = te::gfx::Scene().shadow_meshes;
}

// The frame's GL work: the buckets in their order (bucket 0 holds StartFrame's clear), then the scan-out -- shown
// (Present), resolved for the armed capture only (Capture: a picture the cadence does not show), or dropped (None: no GL
// work at all). On the render thread when there is one, else the game thread's.
void BeginFrameTarget(int screen_w, int screen_h) {
    int fw = 0, fh = 0;
    ps2e_draw_frame_size(g.dev, &fw, &fh);
    if (g.have_frame && (fw != screen_w || fh != screen_h)) ps2e_draw_resize_frame(g.dev, screen_w, screen_h);
    ps2e_draw_begin_frame(g.dev, nullptr);
}

// A picture's draws as the interpolator reads them (bucket order), built once per picture (the vector is the slot's)
void BuildInterpItems(Gfx::Frame& f) {
    if (!f.items.empty()) return;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
        for (size_t i = 0; i < f.buckets[b].used; ++i) {
            const Draw& d = f.buckets[b].draws[i];
            ps2r::ps2e::InterpItem it;
            it.state = &d.state;
            it.verts = d.verts.data();
            it.count = uint32_t(d.verts.size());
            it.copy_frame = d.copy_frame;
            it.tag = d.tag;
            it.transform = d.has_transform ? &d.transform : nullptr;
            f.items.push_back(it);
        }
    }
}

uint64_t SubmitBuckets(Gfx::Bucket* buckets, int screen_w, int screen_h, bool count_as_picture) {
    BeginFrameTarget(screen_w, screen_h);
    uint64_t submitted = 0;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) submitted += buckets[b].used;
    if (submitted && count_as_picture) g.presents_with_draws++;
    g.frame_draws = submitted;
    g.draws_submitted += submitted;
    uint64_t triangles = 0;
    // TE_GFX_DIAG_SUBMITS=N logs every submit of picture N (the frame dumped as frame_N) across all buckets: the state, the
    // vertex count, the screen box, the mean colour (a darkener is found by its box); =N,K stops the frame after K submits
    static long diagPresent = -1, diagStop = -1;
    static bool diagRead = false;
    if (!diagRead) {
        diagRead = true;
        if (const char* p = std::getenv("TE_GFX_DIAG_SUBMITS")) {
            diagPresent = std::atol(p);
            if (const char* c = std::strchr(p, ',')) diagStop = std::atol(c + 1);
        }
    }
    const bool diag = count_as_picture && diagPresent >= 0 && g.pictures.load() + 1 == uint64_t(diagPresent);
    long submitIndex = 0;
    static long xLogFrom = -2, xLogTo = -1;   // XBISECT: TE_X_DRAWLOG=A,B lists every draw of pictures A..B
    if (xLogFrom == -2) {
        xLogFrom = -1;
        if (const char* v = std::getenv("TE_X_DRAWLOG")) { xLogFrom = std::atol(v); const char* c = std::strchr(v, ','); xLogTo = c ? std::atol(c + 1) : xLogFrom; }
    }
    const long xPic = long(g.pictures.load()) + 1;
    const bool xLog = count_as_picture && xLogFrom >= 0 && xPic >= xLogFrom && xPic <= xLogTo;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
        ExpandBucket(buckets, b);   // its builds, when the picture came with them still running
        if (xLog) {
            for (size_t i = 0; i < buckets[b].used; ++i) {
                const Draw& d = buckets[b].draws[i];
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& v : d.verts) { x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); }
                std::fprintf(stderr, "XD %ld %d %u t%u bl%d(%d%d%d%d) zt%d zw%d at%d m%u v%u box %.0f %.0f %.0f %.0f\n", xPic, b, unsigned(i), d.state.texture, d.state.blend,
                             d.state.blend_a, d.state.blend_b, d.state.blend_c, d.state.blend_d, d.state.depth_test, d.state.depth_write, d.state.alpha_test,
                             d.mesh, unsigned(d.verts.size()), x0, y0, x1, y1);
            }
        }
        for (size_t i = 0; i < buckets[b].used; ++i) {
            const Draw& d = buckets[b].draws[i];
            if (diag) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e38f, z1 = -1e38f;
                double r = 0, gg = 0, bb = 0, a = 0;
                for (const auto& v : d.verts) {
                    x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
                    z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
                    r += v.r; gg += v.g; bb += v.b; a += v.a;
                }
                const double n = double(std::max<size_t>(1, d.verts.size()));
                const auto& st = d.state;
                std::fprintf(stderr, "[te.gfx.submit] present %ld #%ld bucket %d verts %zu tex %u gouraud %d ztest %d zwrite %d blend %d(%d %d %d %d fix %d) atest %d(m%d ref %d fail %d) tfx %d tcc %d nocol %d stencil %d scissor (%d %d)-(%d %d) box (%.0f %.0f)-(%.0f %.0f) z %.3g..%.3g rgba (%.0f %.0f %.0f %.0f)\n",
                             diagPresent, submitIndex, b, d.verts.size(), st.texture, st.gouraud, st.depth_test, st.depth_write, st.blend, st.blend_a, st.blend_b, st.blend_c, st.blend_d, st.blend_fix,
                             st.alpha_test, st.alpha_method, st.alpha_reference, st.alpha_fail, st.tfx, st.tcc, st.no_colour_write,
                             st.stencil,
                             st.scissor_x0, st.scissor_y0, st.scissor_x1, st.scissor_y1, x0, y0, x1, y1, z0, z1, r / n, gg / n, bb / n, a / n);
            }
            submitIndex++;
            if (diag && diagStop >= 0 && submitIndex > diagStop) continue;
            if (d.copy_frame) ps2e_draw_copy_frame(g.dev);   // the screen copy this draw samples
            if (d.mesh) {
                if (ps2e_draw_submit_mesh(g.dev, &d.state, d.mesh, &d.mesh_params) < 0) g.dropped_draws++;
                continue;
            }
            if (ps2e_draw_submit(g.dev, &d.state, d.verts.data(), int(d.verts.size())) < 0) g.dropped_draws++;
            triangles += d.verts.size() / 3;
        }
    }
    g.frame_triangles = triangles;
    return submitted;
}

// The pace over the presents since the last summary: the rate they were shown at, the longest gap between two of them
// and how many gaps were longer than a 60 Hz field (the running average from the first present hides a slow stretch)
struct Pace {
    Steady::time_point start, last;
    uint64_t frames = 0, slow = 0;
    double longest_ms = 0, submit_ms = 0, present_ms = 0;
    int64_t waited_ns = 0, built_ns = 0;   // the build pool's counters at the window's start
};
Pace g_pace;

void NotePace(uint64_t presents) {
    const auto now = Steady::now();
    BuildPool& pool = Builds();
    if (g_pace.frames == 0) {
        g_pace.start = g_pace.last = now;
        g_pace.waited_ns = pool.waited_ns.load(std::memory_order_relaxed);
        g_pace.built_ns = pool.built_ns.load(std::memory_order_relaxed);
    } else {
        const double gap = std::chrono::duration<double, std::milli>(now - g_pace.last).count();
        g_pace.longest_ms = std::max(g_pace.longest_ms, gap);
        if (gap > 1000.0 / 60.0 + 1.0) ++g_pace.slow;
        g_pace.last = now;
        g_pace.submit_ms += g.last_submit_ms;
        g_pace.present_ms += g.last_present_ms;
    }
    ++g_pace.frames;
    if (g.summary_every > 0 && presents % uint64_t(g.summary_every) == 0 && g_pace.frames > 1) {
        const double s = std::chrono::duration<double>(g_pace.last - g_pace.start).count();
        const double gaps = double(g_pace.frames - 1);
        const double frame_ms = s * 1000.0 / gaps;
        // the frame's time on the drawing thread: the draws' submission, the present (the GPU's finish + the swap); the
        // rest is the game's own (and, synchronous, the window pump and the pacing park). The geometry builds: their own
        // time on every thread together, and the drawing / game thread's time on them (helping or waiting)
        const double built = double(pool.built_ns.load(std::memory_order_relaxed) - g_pace.built_ns) / 1e6 / gaps;
#if defined(__vita__)
        // the newlib heap (the Vita's fixed block): in use and the most it has reached
        const struct mallinfo heap = mallinfo();
        std::fprintf(stderr, "[te.gfx] heap: %.1f MB in use, %.1f MB reached\n", heap.uordblks / 1048576.0, heap.arena / 1048576.0);
#endif
        const double waited = double(pool.waited_ns.load(std::memory_order_relaxed) - g_pace.waited_ns) / 1e6 / gaps;
        std::fprintf(stderr, "[te.gfx] pace: presents %llu-%llu at %.1f fps, longest gap %.1f ms, %llu of %llu gaps over a field; "
                     "a frame %.2f ms = submit %.2f + present %.2f + the rest %.2f; builds %.2f ms (waited on %.2f)\n",
                     (unsigned long long)(presents - g_pace.frames + 1), (unsigned long long)presents,
                     s > 0 ? gaps / s : 0.0, g_pace.longest_ms, (unsigned long long)g_pace.slow,
                     (unsigned long long)(g_pace.frames - 1), frame_ms, g_pace.submit_ms / gaps, g_pace.present_ms / gaps,
                     std::max(0.0, frame_ms - (g_pace.submit_ms + g_pace.present_ms) / gaps), built, waited);
        g_pace = Pace{};
    }
}

// A shown frame's bookkeeping: the count, the pace, the periodic summary
void NotePresented() {
    const uint64_t presents = ++g.presents;
    if (!g.have_first_present) {
        g.have_first_present = true;
        g.first_present = Steady::now();
    }
    NotePace(presents);
    // The runner's --run-seconds watchdog leaves with _exit (no atexit): a periodic summary keeps the counts visible
    if (g.summary_every > 0 && presents % uint64_t(g.summary_every) == 0) Summary();
}

void RenderFrame(Gfx::Frame& f, Show show) {
    const auto t0 = Steady::now();
    ++g.pictures;
    const uint64_t picture = f.picture ? f.picture : g.pictures.load();   // (the synchronous path has no hand-over: the count)
    auto& rt = g.rt;
    if (show == Show::None && !g.harness_keys.empty() && te::harness::Armed() && HarnessWants(f.harness_key)) show = Show::Capture;
    if (show == Show::None) {
        g.frame_draws = 0;
        g.frame_triangles = 0;
        rt.target_slot = -1;   // the frame target does not hold this picture
        if (te::harness::Armed()) te::harness::OnPresentSkipped(f.harness_key);
        return;
    }
    if (f.send) {
        SubmitBuckets(f.buckets, f.screen_w, f.screen_h, true);
    } else {
        g.frame_draws = 0;
        g.frame_triangles = 0;
    }
    rt.target_slot = int(&f - rt.slots);
    rt.target_fraction = 1.0;
    const auto t1 = Steady::now();
    const bool keyed = !g.harness_keys.empty() && te::harness::Armed();
    const bool wanted = keyed && HarnessWants(f.harness_key);
    if (wanted) ps2e_draw_arm_capture(g.dev, 1);   // this picture alone is read back
    if (show == Show::Present) ps2e_draw_present(g.dev);
    else ps2e_draw_capture_frame(g.dev);
    const auto t2 = Steady::now();
    if (!OnRenderThread()) {
        g.last_submit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        g.last_present_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }
    // The harness leg (te_harness.h): this picture's hash + descriptor under the key of the boundary it precedes
    if (!keyed || wanted) te::harness::OnPresent(g.dev, f.harness_key);
    else te::harness::OnPresentSkipped(f.harness_key);
    if (wanted && g.dump_dir.empty()) ps2e_draw_arm_capture(g.dev, 0);
    if (show == Show::Present) NotePresented();
    MaybeDump(picture);
}

// A frame between two pictures (the slot trains): at fraction 1 the newest picture as it is; below it the interpolated
// scene when the interpolator is on (the engine's SceneInterpolator), else the newest picture again.
void RenderBetween(Gfx::Frame& prev, Gfx::Frame& cur, double fraction, int64_t slot_ns) {
    (void)prev;
    (void)slot_ns;
    auto& rt = g.rt;
    const int64_t period = 1000000000LL / 60;
    const int cur_slot = int(&cur - rt.slots);
    // The latency is noted only while the newest picture is fresh (within two ticks): a load screen or a frame the game
    // did not present keeps the old picture on the window, which is not the presenter's latency
    auto note = [&](double shown_fraction) {
        const int64_t now = NowNs();
        if (cur.have_edge && now - cur.edge_ns <= 2 * period) NoteLatency(cur.edge_ns - period + int64_t(shown_fraction * double(period)), now);
    };
    if (fraction >= 1.0 || !rt.interpolate) {
        // the newest picture as it is: when the frame target already holds it (it was resolved at its arrival, or shown at
        // the previous slot) the scan-out is re-presented without a submit
        if (rt.target_slot == cur_slot && rt.target_fraction == 1.0) {
            rt.represents++;
        } else {
            if (cur.send) SubmitBuckets(cur.buckets, cur.screen_w, cur.screen_h, false);
            rt.target_slot = cur_slot;
            rt.target_fraction = 1.0;
        }
        ps2e_draw_present(g.dev);
        NotePresented();
        note(1.0);
        if (rt.interpolate && rt.last_noted_picture != cur.picture) {   // the displacement check measures the next frame from this picture
            BuildInterpItems(cur);
            ps2r::ps2e::InterpScene sc;
            sc.items = cur.items.data();
            sc.count = cur.items.size();
            sc.ordinal = cur.picture;
            rt.interpolator.NoteShownPicture(sc);
            rt.last_noted_picture = cur.picture;
        }
        return;
    }
    // The interpolated frame: the two pictures as the interpolator reads them (built once per picture), the frame at the
    // fraction, submitted in the newest picture's draw order
    BuildInterpItems(prev);
    BuildInterpItems(cur);
    auto scene_of = [](const Gfx::Frame& f) {
        ps2r::ps2e::InterpScene sc;
        sc.items = f.items.data();
        sc.count = f.items.size();
        sc.camera = f.camera;
        sc.off_x = kGsMiddle - float(f.screen_w >> 1);
        sc.off_y = kGsMiddle - float(f.screen_h >> 1);
        sc.screen_scale = 16.f;   // the title's toScreen lands on the GS's 12.4 raster (the mesh builder divides by 16)
        sc.ordinal = f.picture;
        return sc;
    };
    ps2r::ps2e::InterpScene sp = scene_of(prev), sc = scene_of(cur);
    sc.cut = prev.camera_object != cur.camera_object;   // another camera object: a cut (the game switched cameras)
    const auto ti0 = Steady::now();
    const bool interpolated = rt.interpolator.Interpolate(sp, sc, fraction, rt.interp_out);
    const auto ti1 = Steady::now();
    if (!interpolated) rt.snaps++;
    else rt.interp_frames++;
    BeginFrameTarget(cur.screen_w, cur.screen_h);
    uint64_t submitted = 0;
    for (const auto& o : rt.interp_out) {
        if (o.count < 3) continue;
        if (o.copy_frame) ps2e_draw_copy_frame(g.dev);
        if (ps2e_draw_submit(g.dev, o.state, o.verts, int(o.count)) < 0) g.dropped_draws++;
        submitted++;
    }
    g.draws_submitted += submitted;
    rt.target_slot = cur_slot;
    rt.target_fraction = interpolated ? fraction : 1.0;
    const auto ti2 = Steady::now();
    ps2e_draw_present(g.dev);
    const auto ti3 = Steady::now();
    rt.interp_ms_sum += std::chrono::duration<double, std::milli>(ti1 - ti0).count();
    rt.submit_ms_sum += std::chrono::duration<double, std::milli>(ti2 - ti1).count();
    rt.present_ms_sum += std::chrono::duration<double, std::milli>(ti3 - ti2).count();
    rt.phase_n++;
    NotePresented();
    note(interpolated ? fraction : 1.0);
    // the one extra scene copy: the previous picture's vertex storage kept beside the newest, and the interpolator's scratch
    uint64_t prev_bytes = 0;
    for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b)
        for (size_t i = 0; i < prev.buckets[b].used; ++i) prev_bytes += prev.buckets[b].draws[i].verts.size() * sizeof(ps2e_draw_vertex);
    rt.interp_bytes = std::max<uint64_t>(rt.interp_bytes, prev_bytes + rt.interpolator.ScratchBytes());
    if (rt.dump_interp && !g.dump_dir.empty()) {
        const bool in_window = (g.dump_from <= 0 || cur.picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || cur.picture <= uint64_t(g.dump_to));
        if (in_window && interpolated) {
            char path[1024];
            std::snprintf(path, sizeof path, "%s/frame_%06llu_i%03d.ppm", g.dump_dir.c_str(), (unsigned long long)cur.picture, int(fraction * 1000.0 + 0.5));
            ps2e_draw_write_ppm(g.dev, path);
        }
    }
    if (rt.interp_diag && cur.picture % 100 == 0 && interpolated) {
        // fraction 1 on this very pair must reproduce the newest picture: the worst vertex error per kind
        std::vector<ps2r::ps2e::InterpOutput> check;
        ps2r::ps2e::SceneInterpolator probe;
        probe.Interpolate(sp, sc, 1.0, check);
        double worst = 0; size_t worst_item = 0; uint32_t worst_v = 0; size_t compared = 0;
        for (size_t i = 0; i < check.size() && i < cur.items.size(); ++i) {
            if (check[i].verts == cur.items[i].verts) continue;
            for (uint32_t v = 0; v < check[i].count; ++v) {
                const double e = std::hypot(double(check[i].verts[v].x - cur.items[i].verts[v].x), double(check[i].verts[v].y - cur.items[i].verts[v].y));
                compared++;
                if (e > worst) { worst = e; worst_item = i; worst_v = v; }
            }
        }
        std::fprintf(stderr, "[te.gfx.interp] picture %llu fraction-1 check: %zu vertices re-projected, worst %.3f px (item %zu v %u", (unsigned long long)cur.picture, compared, worst, worst_item, worst_v);
        if (compared && worst_item < cur.items.size() && worst_v < check[worst_item].count) {
            const auto& a = cur.items[worst_item].verts[worst_v];
            const auto& b = check[worst_item].verts[worst_v];
            std::fprintf(stderr, ": drawn (%.2f %.2f z %.0f q %.5f) got (%.2f %.2f z %.0f)", a.x, a.y, a.z, a.q, b.x, b.y, b.z);
            if (const auto* tr = cur.items[worst_item].transform) {
                // the drawn vertex back through S^-1: what w does the inverse give, and does S forward reproduce it
                double S[16], Sinv[16];
                for (int c = 0; c < 16; ++c) S[c] = tr->screen[c];
                if (ps2r::ps2e::SceneInterpolator::Invert4(S, Sinv)) {
                    const double X = (a.x + sc.off_x) * 16.0, Y = (a.y + sc.off_y) * 16.0, Z = a.z;
                    double m[4] = {0, 0, 0, 0};
                    for (int c = 0; c < 4; ++c) m[c] = X * Sinv[c] + Y * Sinv[4 + c] + Z * Sinv[8 + c] + Sinv[12 + c];
                    std::fprintf(stderr, " | S^-1 -> w %.5f model (%.3f %.3f %.3f); tag %llx count %u", m[3], m[0] / m[3], m[1] / m[3], m[2] / m[3],
                                 (unsigned long long)cur.items[worst_item].tag.id, cur.items[worst_item].count);
                    double W[16], t[3], q[4], scl[3], R[16];
                    for (int c = 0; c < 16; ++c) W[c] = tr->world[c];
                    const bool dec = ps2r::ps2e::SceneInterpolator::DecomposeTrs(W, t, q, scl);
                    double rt_err = -1;
                    if (dec) {
                        ps2r::ps2e::SceneInterpolator::ComposeTrs(t, q, scl, R);
                        rt_err = 0;
                        for (int c = 0; c < 16; ++c) rt_err = std::max(rt_err, std::fabs(R[c] - W[c]));
                    }
                    std::fprintf(stderr, " | world rows (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g) TRS round trip err %.4g scale (%.3g %.3g %.3g)",
                                 W[0], W[1], W[2], W[3], W[4], W[5], W[6], W[7], W[8], W[9], W[10], W[11], W[12], W[13], W[14], W[15], rt_err, scl[0], scl[1], scl[2]);
                    // world . world->screen (the interpolator's camera) vs the drawn S
                    double ws[16], prod[16], cerr = 0;
                    double cam[16], c2s[16];
                    for (int c = 0; c < 16; ++c) { cam[c] = cur.camera.to_camera[c]; c2s[c] = cur.camera.camera_to_screen[c]; }
                    ps2r::ps2e::SceneInterpolator::Multiply4(cam, c2s, ws);
                    ps2r::ps2e::SceneInterpolator::Multiply4(W, ws, prod);
                    for (int c = 0; c < 16; ++c) cerr = std::max(cerr, std::fabs(prod[c] - S[c]));
                    std::fprintf(stderr, " | world.cam.c2s vs drawn S err %.4g (camera valid %d)", cerr, int(cur.camera.valid));
                }
            }
        }
        std::fprintf(stderr, ")\n");
    }
    if (rt.interp_diag) {
        const auto& st = rt.interpolator.Stats();
        size_t tagged_rigid = 0, tagged_vertices = 0, untagged = 0;
        for (const auto& it : cur.items) {
            if (it.tag.id == 0 || it.tag.kind == ps2r::ps2e::InterpKind::Newest) untagged++;
            else if (it.tag.kind == ps2r::ps2e::InterpKind::Rigid) tagged_rigid++;
            else tagged_vertices++;
        }
        std::fprintf(stderr, "[te.gfx.interp] picture %llu items: rigid %zu vertex runs %zu untagged %zu (splitting %d)\n", (unsigned long long)cur.picture, tagged_rigid, tagged_vertices, untagged, int(g.interp_active.load()));
        if (cur.picture % 300 == 0)
            for (const auto& it : cur.items)
                if (it.tag.kind == ps2r::ps2e::InterpKind::Vertices)
                    std::fprintf(stderr, "[te.gfx.interp]   vertex run %llx part %u count %u\n", (unsigned long long)it.tag.id, it.tag.part, it.count);
        std::fprintf(stderr, "[te.gfx.interp] picture %llu fraction %.3f %s: rigid %llu vertices %llu newest %llu unmatched %llu snaps %llu unprojectable %llu; displacement checks %llu violations %llu\n",
                     (unsigned long long)cur.picture, fraction, interpolated ? "interpolated" : "SNAPPED",
                     (unsigned long long)st.rigid_items, (unsigned long long)st.vertex_items, (unsigned long long)st.newest_items,
                     (unsigned long long)st.unmatched, (unsigned long long)st.object_snaps, (unsigned long long)st.unprojectable,
                     (unsigned long long)st.displacement_checks, (unsigned long long)st.displacement_violations);
    }
}

// The quality judge's probe (TE_GFX_INTERP_PROBE): the half-way frame between two pictures built and dumped at the newest's
// arrival, whatever the pacer's slots do -- frame_<picture>_i500.ppm beside the pictures' dumps. The frame target then
// holds the probe, not a picture (the next slot re-submits).
void ProbeHalf(Gfx::Frame& prev, Gfx::Frame& cur) {
    auto& rt = g.rt;
    if (g.dump_dir.empty()) return;
    const bool in_window = (g.dump_from <= 0 || cur.picture >= uint64_t(g.dump_from)) && (g.dump_to <= 0 || cur.picture <= uint64_t(g.dump_to));
    if (!in_window) return;
    BuildInterpItems(prev);
    BuildInterpItems(cur);
    auto scene_of = [](const Gfx::Frame& f) {
        ps2r::ps2e::InterpScene sc;
        sc.items = f.items.data();
        sc.count = f.items.size();
        sc.camera = f.camera;
        sc.off_x = kGsMiddle - float(f.screen_w >> 1);
        sc.off_y = kGsMiddle - float(f.screen_h >> 1);
        sc.screen_scale = 16.f;
        sc.ordinal = f.picture;
        return sc;
    };
    ps2r::ps2e::InterpScene sp = scene_of(prev), sc = scene_of(cur);
    sc.cut = prev.camera_object != cur.camera_object;
    static ps2r::ps2e::SceneInterpolator probe;   // its own history: the presenter's displacement census is untouched
    static std::vector<ps2r::ps2e::InterpOutput> out;
    const bool interpolated = probe.Interpolate(sp, sc, 0.5, out);
    if (!interpolated) return;   // a cut: no half frame to judge
    BeginFrameTarget(cur.screen_w, cur.screen_h);
    for (const auto& o : out) {
        if (o.count < 3) continue;
        if (o.copy_frame) ps2e_draw_copy_frame(g.dev);
        ps2e_draw_submit(g.dev, o.state, o.verts, int(o.count));
    }
    ps2e_draw_capture_frame(g.dev);
    char path[1024];
    std::snprintf(path, sizeof path, "%s/frame_%06llu_i500.ppm", g.dump_dir.c_str(), (unsigned long long)cur.picture);
    ps2e_draw_write_ppm(g.dev, path);
    rt.target_slot = -1;
}

// Present: the frame handed to the render thread (the bucket storage swapped, one frame in flight) or drawn here, then
// the blank
void PresentFrame(bool send) {
    if (!EnsureDevice()) {
        ClearBuckets();
        return;
    }
    auto& rt = g.rt;
    // The harness leg's key: the boundary this present precedes (read on the game thread, before any hand-over)
    const uint64_t harness_key = te::Services::Get().clock().BoundaryCount() + 1;
    te::gfx::FlushNativeMeshes();   // the meshes this picture's models asked for, ready for the next picture's draws
    if (rt.started) {
        const auto t0 = Steady::now();
        {
            std::unique_lock<std::mutex> lock(rt.mutex);
            if (rt.free.empty()) {   // one drawing and two queued: the render thread is more than two frames behind
                rt.waits++;
                rt.done.wait(lock, [&] { return !rt.free.empty(); });
                const double waited = std::chrono::duration<double, std::milli>(Steady::now() - t0).count();
                rt.wait_ms += waited;
                rt.wait_max_ms = std::max(rt.wait_max_ms, waited);
            }
            const int slot = rt.free.back();
            rt.free.pop_back();
            Gfx::Frame& f = rt.slots[slot];
            for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
                std::swap(g.buckets[b].draws, f.buckets[b].draws);
                std::swap(g.buckets[b].used, f.buckets[b].used);
            }
            std::swap(g.deferred, f.deferred);
            std::swap(g.deferred_used, f.deferred_used);
            f.retire.insert(f.retire.end(), g.mesh_retired.begin(), g.mesh_retired.end());
            g.mesh_retired.clear();
            f.send = send;
            f.screen_w = ScreenW();
            f.screen_h = ScreenH();
            f.harness_key = harness_key;
            f.items.clear();   // this picture's draws for the interpolator are listed on the render thread when needed
            f.picture = ++g.pictures_handed;   // its ordinal (the game thread's count: the render thread's g.pictures lags it)
            // the pacer edge of the field this picture follows (the presenter's clock for the frames between pictures)
            f.have_edge = te::Services::Get().clock().LatestFieldEdge(&f.edge_ns, &f.field);
            if (rt.interp_diag && f.picture % 200 == 0)
                std::fprintf(stderr, "[te.gfx.interp] hand-over: picture %llu boundary %llu field %llu edge age %.2f ms, work since the release %.2f ms\n", (unsigned long long)f.picture,
                             (unsigned long long)harness_key - 1, (unsigned long long)f.field, f.have_edge ? double(NowNs() - f.edge_ns) / 1e6 : -1.0,
                             te::Services::Get().clock().MsSinceRelease());
            f.camera = ps2r::ps2e::InterpCamera{};
            f.camera_object = 0;
            te::gfx::CurrentCamera(f.camera, &f.camera_object);
            rt.queue.push_back(slot);
        }
        rt.wake.notify_one();
        // The phases the slow-tick line reports: no submit on this thread; "present" = the hand-over (the wait, if any)
        g.last_submit_ms = 0;
        g.last_present_ms = std::chrono::duration<double, std::milli>(Steady::now() - t0).count();
    } else {
        // synchronous (deterministic / headless / dumping): every picture presented whatever the frame-rate setting
        static Gfx::Frame sync;
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
        }
        sync.send = send;
        sync.screen_w = ScreenW();
        sync.screen_h = ScreenH();
        sync.harness_key = harness_key;
        sync.picture = 0;
        ExpandDeferred(sync.buckets, g.deferred, g.deferred_used);
        RenderFrame(sync, Show::Present);
        for (uint32_t mesh : g.mesh_retired) ps2e_draw_mesh_destroy(g.dev, mesh);   // drawn for the last time just now
        g.mesh_retired.clear();
        for (int b = 0; b < TE_GFX_BUCKET_COUNT; ++b) {
            std::swap(g.buckets[b].draws, sync.buckets[b].draws);
            std::swap(g.buckets[b].used, sync.buckets[b].used);
        }
    }
    ClearBuckets();
    te::gfx::ResetDrawOrdinals();   // the next picture's draw identities count from the start
    VBlank();
}

// The census row the runner prints at exit and the harness records:
//   present=<rate>:<shown fps>:<interpolated frames>:<snaps>:<latency ms>:<late presents>
std::string PresentCensus() {
    auto& rt = g.rt;
    double shown_fps = 0;
    if (g.have_first_present) {
        const double s = std::chrono::duration<double>(Steady::now() - g.first_present).count();
        if (s > 0.5) shown_fps = double(g.presents.load() - 1) / s;
    }
    const auto c = rt.pacer.Count();
    char buf[2048];
    const auto& st = rt.interpolator.Stats();
    std::snprintf(buf, sizeof buf, "present=%s:%.1f:%llu:%llu:%.2f:%llu (pictures %llu, presents %llu, re-presented %llu, latency max %.2f ms, delay %.2f ms (max %.2f), interpolated frame %.2f + %.2f + %.2f ms (interpolate, submit, present), interpolated scene %llu bytes; rigid %llu vertex runs %llu newest %llu unmatched %llu object snaps %llu unprojectable %llu count mismatches %llu run jumps %llu; displacement checks %llu violations %llu)",
                  ps2r::ps2e::PresentPacer::Name(static_cast<Rate>(g.frame_rate.load())), shown_fps,
                  (unsigned long long)rt.interp_frames, (unsigned long long)rt.snaps,
                  rt.latency_n ? rt.latency_sum_ms / double(rt.latency_n) : 0.0, (unsigned long long)c.late,
                  (unsigned long long)g.pictures.load(), (unsigned long long)g.presents.load(), (unsigned long long)rt.represents,
                  rt.latency_max_ms, double(rt.delay_ns) / 1e6, rt.delay_ms_max,
                  rt.phase_n ? rt.interp_ms_sum / double(rt.phase_n) : 0.0, rt.phase_n ? rt.submit_ms_sum / double(rt.phase_n) : 0.0, rt.phase_n ? rt.present_ms_sum / double(rt.phase_n) : 0.0,
                  (unsigned long long)rt.interp_bytes,
                  (unsigned long long)st.rigid_items, (unsigned long long)st.vertex_items, (unsigned long long)st.newest_items,
                  (unsigned long long)st.unmatched, (unsigned long long)st.object_snaps, (unsigned long long)st.unprojectable,
                  (unsigned long long)st.vertex_count_mismatch, (unsigned long long)st.vertex_run_jumps, (unsigned long long)st.displacement_checks, (unsigned long long)st.displacement_violations);
    return buf;
}

}  // namespace

// ---- shared with graphics_scene.cpp (graphics_internal.h) ----------------------------------------------------------------
namespace te::gfx {
bool EnsureDevice() { return ::EnsureDevice(); }
MaterialView ReadMaterial(uint32_t address) { return ::ReadMaterial(address); }
ps2e_draw_state StateOf(const MaterialView& m) { return ::StateOf(m); }
Draw& OpenDraw(uint32_t bucket, const ps2e_draw_state& state, bool copy_frame) { return ::OpenDraw(bucket, state, copy_frame); }
MaterialView ShaderView(const MaterialView& m, uint32_t k) { return ::ShaderView(m, k); }
void CloseDraw(uint32_t bucket) { ::CloseDraw(bucket); }
void SetDrawTag(const ps2r::ps2e::InterpTag& tag, const float* world16, const float* screen16) { ::SetDrawTag(tag, world16, screen16); }
void ClearDrawTag() { ::ClearDrawTag(); }
bool Interpolating() { return g.interp_active.load(std::memory_order_relaxed); }
bool NativeMeshes() {
    if (g.native_meshes < 0) {
        const char* v = std::getenv("TE_GFX_NATIVE_MESH");
        g.native_meshes = (v && v[0] == '0') ? 0 : 1;
    }
    return g.native_meshes == 1;
}
void CreateMeshes(std::vector<MeshRequest>& requests) {
    if (requests.empty() || !::EnsureDevice()) return;
    bool declined = false;
    OnRender([&] {
        for (MeshRequest& r : requests) {
            r.handle = ps2e_draw_mesh_create(g.dev, r.vertices.data(), int(r.vertices.size()));
            if (!r.handle && r.vertices.size() >= 3) declined = true;
        }
    });
    if (declined) g.native_meshes = 0;   // a device without the native transform: the CPU builds from here on
}
void RecordMesh(uint32_t bucket, const ps2e_draw_state& state, uint32_t mesh, const ps2e_draw_mesh_params& params) {
    auto& b = g.buckets[bucket < TE_GFX_BUCKET_COUNT ? bucket : TE_GFX_BUCKET_UI];
    g.draws++;
    if (b.used == b.draws.size()) b.draws.emplace_back();
    Draw& d = b.draws[b.used++];
    d.state = state;
    d.verts.clear();
    d.copy_frame = false;
    d.pending = nullptr;
    d.tag = g.tag;
    d.has_transform = false;
    d.mesh = mesh;
    d.mesh_params = params;
}
void RetireMesh(uint32_t mesh) {
    if (mesh) g.mesh_retired.push_back(mesh);
}
void Record(uint32_t bucket, Draw&& d) { ::Record(bucket, std::move(d)); }
bool DeferringBuilds() { return ::DeferringBuilds(); }
DeferredBuild& ReserveDeferred(uint32_t bucket, const ps2e_draw_state& state) { return ::ReserveDeferred(bucket, state); }
void PostBuild(DeferredBuild& build, std::function<void()> job) { ::PostBuild(build, std::move(job)); }
void DrainBuilds() { ::DrainBuilds(); }
float GsOriginX() { return ::GsOriginX(); }
float GsOriginY() { return ::GsOriginY(); }
int ScreenW() { return ::ScreenW(); }
int ScreenH() { return ::ScreenH(); }
const RenderTargetDescription* CurrentTarget() { return ::CurrentTarget(); }
bool LogEnabled() { ReadOptions(); return g.log; }
SceneCounters& Scene() { static SceneCounters counters; return counters; }
}  // namespace te::gfx

// Blanks the graphics side passed through (one per WaitVSync / Present): diagnostics; the clock is te::Clock's.
extern "C" uint64_t te_graphics_vblanks() { return g.vblanks; }
// The last Present's phases on the game thread (the clock's `tick:` line, when a tick is slow): the draws' submission to
// the backend and the scan-out/swap; the rest of the tick is the game's own work
extern "C" void te_graphics_last_present_phases_ms(double* submit, double* present, double* pump) {
    if (submit) *submit = g.last_submit_ms;
    if (present) *present = g.last_present_ms;
    if (pump) *pump = g.last_pump_ms;
}
extern "C" uint64_t te_graphics_presents() { return g.presents.load(); }
// The presenter's census row (the runner's exit lines / the harness): present=rate:shown_fps:interp_frames:snaps:latency_ms:late_presents
extern "C" void te_graphics_present_census(char* out, size_t size) {
    const std::string row = PresentCensus();
    if (!out || !size) return;
    std::snprintf(out, size, "%s", row.c_str());
}

// ---- save state (savestate.h, kTagGraphics): the renderer's host-side state the game cannot rebuild -- the textures
// (decoded again from their kept header + packet), the display mode, the counters. The frame's buckets are empty at a
// boundary (Present cleared them before the blank), so nothing of the frame is state.
namespace te::gfx {
void RegisterStateSection() {
    static const bool once = [] {
        savestate::Register(savestate::kTagGraphics, "graphics host", false,
            [](std::vector<uint8_t>& out) {
                savestate::Put(out, uint32_t(5));
                savestate::Put(out, uint8_t(g.pal ? 1 : 0));
                savestate::Put(out, int32_t(g.display_w)); savestate::Put(out, int32_t(g.display_h));
                savestate::Put(out, uint8_t(g.have_frame ? 1 : 0));
                savestate::PutBytes(out, &g.frame, sizeof g.frame);
                savestate::Put(out, g.presents.load()); savestate::Put(out, g.vblanks); savestate::Put(out, g.draws);
                savestate::Put(out, uint32_t(g.texture_sources.size()));
                for (const auto& [address, source] : g.texture_sources) {
                    savestate::Put(out, address);
                    savestate::PutBytes(out, &source.header, sizeof source.header);
                    savestate::Put(out, uint32_t(source.packet.size()));
                    savestate::PutBytes(out, source.packet.data(), source.packet.size());
                }
                // version 2: the scene's decoded models' packets (graphics_scene.cpp keeps the bytes: the game released them)
                const auto models = ModelSources();
                savestate::Put(out, uint32_t(models.size()));
                for (const ModelSource& m : models) {
                    savestate::Put(out, m.model); savestate::Put(out, m.index); savestate::Put(out, m.count); savestate::Put(out, m.vertices);
                    savestate::Put(out, uint32_t(m.packet.size()));
                    savestate::PutBytes(out, m.packet.data(), m.packet.size());
                }
                // version 3: the skins' submodel packets, the same shape
                const auto skins = SkinSources();
                savestate::Put(out, uint32_t(skins.size()));
                for (const ModelSource& m : skins) {
                    savestate::Put(out, m.model); savestate::Put(out, m.index); savestate::Put(out, m.count); savestate::Put(out, m.vertices);
                    savestate::Put(out, uint32_t(m.packet.size()));
                    savestate::PutBytes(out, m.packet.data(), m.packet.size());
                }
                // version 4: the screen models (the skid marks' strips the game holds handles to)
                const auto screen = ScreenModelState();
                savestate::Put(out, uint32_t(screen.size()));
                savestate::PutBytes(out, screen.data(), screen.size());
                // version 5: the pictures' count (the dumps' numbering; = presents in a synchronous run)
                savestate::Put(out, g.pictures.load());
            },
            [](const savestate::Blob& blob, std::string* err) {
                savestate::PayloadReader in(blob.payload);
                uint32_t version = 0, count = 0;
                uint8_t pal = 0, have_frame = 0;
                int32_t w = 0, h = 0;
                FrameStart frame{};
                uint64_t presents = 0, vblanks = 0, draws = 0;
                auto fail = [&](const char* why) { if (err) *err = std::string("graphics: ") + why; return false; };
                if (!in.Get(&version) || version < 2 || version > 5 || !in.Get(&pal) || !in.Get(&w) || !in.Get(&h) || !in.Get(&have_frame) ||
                    !in.GetBytes(&frame, sizeof frame) || !in.Get(&presents) || !in.Get(&vblanks) || !in.Get(&draws) || !in.Get(&count) ||
                    count > 65536)
                    return fail("payload");
                g.pal = pal != 0;
                g.display_w = w; g.display_h = h;
                g.have_frame = have_frame != 0;
                g.frame = frame;
                g.presents = presents; g.vblanks = vblanks; g.draws = draws;
                g.pictures = presents;   // a state before version 5: every present was a picture
                g.pictures_handed = presents;
                OnRender([] { for (auto& [address, handle] : g.textures) if (g.dev) ps2e_draw_texture_destroy(g.dev, handle); });
                g.textures.clear();
                g.texture_sources.clear();
                if (g.have_frame && EnsureDevice()) {
                    OnRender([] {
                        int fw = 0, fh = 0;
                        ps2e_draw_frame_size(g.dev, &fw, &fh);
                        if (fw != ScreenW() || fh != ScreenH()) ps2e_draw_resize_frame(g.dev, ScreenW(), ScreenH());
                    });
                }
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t address = 0;
                    Gfx::TextureSource source;
                    if (!in.Get(&address) || !in.GetBytes(&source.header, sizeof source.header) ||
                        !savestate::GetVector(in, source.packet, 64u << 20))
                        return fail("payload truncated (textures)");
                    te::gfx::DecodedTexture decoded;
                    std::string error;
                    if (!EnsureDevice()) continue;   // the bytes are consumed: the models follow them in the payload
                    if (!te::gfx::DecodeTextureResource(source.header, source.packet.data(), source.packet.size(), decoded, &error)) {
                        std::fprintf(stderr, "[te.gfx] savestate: texture %#x: decode failed: %s\n", address, error.c_str());
                        continue;
                    }
                    uint32_t handle = 0;
                    OnRender([&] { handle = CreateTexture(decoded); });
                    if (!handle) continue;
                    g.textures[address] = handle;
                    g.texture_sources[address] = std::move(source);
                }
                uint32_t model_count = 0;
                if (!in.Get(&model_count) || model_count > 1u << 20) return fail("payload truncated (models)");
                std::vector<ModelSource> models(model_count);
                for (ModelSource& m : models)
                    if (!in.Get(&m.model) || !in.Get(&m.index) || !in.Get(&m.count) || !in.Get(&m.vertices) ||
                        !savestate::GetVector(in, m.packet, 64u << 20))
                        return fail("payload truncated (models)");
                ReplayModelSources(models);
                std::vector<ModelSource> skins;
                if (version >= 3) {
                    uint32_t skin_count = 0;
                    if (!in.Get(&skin_count) || skin_count > 1u << 20) return fail("payload truncated (skins)");
                    skins.resize(skin_count);
                    for (ModelSource& m : skins)
                        if (!in.Get(&m.model) || !in.Get(&m.index) || !in.Get(&m.count) || !in.Get(&m.vertices) ||
                            !savestate::GetVector(in, m.packet, 64u << 20))
                            return fail("payload truncated (skins)");
                }
                ReplaySkinSources(skins);
                if (version >= 4) {
                    std::vector<uint8_t> screen;
                    if (!savestate::GetVector(in, screen, 64u << 20) || !RestoreScreenModelState(screen.data(), screen.size()))
                        return fail("payload truncated (screen models)");
                } else {
                    RestoreScreenModelState(nullptr, 0);
                }
                if (version >= 5) {
                    uint64_t pictures = 0;
                    if (!in.Get(&pictures)) return fail("payload truncated (pictures)");
                    g.pictures = pictures;
                    g.pictures_handed = pictures;
                }
                return true;
            });
        return true;
    }();
    (void)once;
}

void SetPresentation(const ps2e_draw_presentation& p) {
    g.presentation = p;
    if (g.dev) ps2e_draw_set_presentation(g.dev, &g.presentation);   // thread-safe: the pump / the next frame apply it
}

bool Headless() {
    ReadOptions();
    return g.headless;
}

void SetFrameRate(int rate) {
    g.frame_rate.store(std::clamp(rate, 0, 5), std::memory_order_relaxed);
    if (g.rt.started) g.rt.wake.notify_one();   // the render thread re-sets its pacer at its next pass
}

}  // namespace te::gfx

extern "C" {

// ---- devices / display -------------------------------------------------------------------------------------------------
void ps2e_graphics_reset_devices(void) { Once("ResetDevices: no hardware to reset"); }
void ps2e_graphics_reset_path(void) { Once("ResetPath: no GIF path"); }
void ps2e_graphics_wait_v_sync(void) { EnsureDevice(); VBlank(); }
ps2e_s32 ps2e_graphics_is_pal_display(void) { return g.pal ? 1 : 0; }
void ps2e_graphics_wait_idle(void) { Once("WaitIdle: draws are synchronous"); }

void ps2e_graphics_start_renderer(ps2e_s32 height, ps2e_s32 pal) {
    g.pal = pal != 0;
    g.display_w = 512;
    g.display_h = height > 0 && height <= 1024 ? height : (g.pal ? 512 : 448);
    // The game's g_DisplayWidth/g_DisplayHeight, as the PS2 arm's InitDisplay set them (the movie's picture area)
    if (uint32_t* w = GuestMutable<uint32_t>(kDisplayWidth)) *w = uint32_t(g.display_w);
    if (uint32_t* h = GuestMutable<uint32_t>(kDisplayHeight)) *h = uint32_t(g.display_h);
    if (g.dev) {
        OnRender([] { ps2e_draw_resize_frame(g.dev, g.display_w, g.display_h); });
    } else {
        EnsureDevice();
    }
}
void ps2e_graphics_finish_renderer_start(void) { Once("FinishRendererStart"); }
// MoveDisplay (the graphic options' "centre screen" page: the screen position): the picture moved on the display by an
// offset between -1 and 1 each way -- the PS2 moved DISPLAY DX/DY by up to 256 VCK (64 frame pixels) across and 32 lines
// down; the port moves the presented picture by the same frame pixels and persists it (ps2r.ini).
void ps2e_graphics_move_display(ps2e_u32 offset) {
    const Vector2* o = Guest<Vector2>(offset);
    if (!o) return;
    te::display::MoveDisplay(o->x, o->y);
}
// (StepAnimations: the wasm side, graphics_resources.cpp -- shader animation)

// frame.cpp SetUpRenderTarget: the target's clip-to-screen matrix and its corner in GS pixels; the packet it kept is not
// needed (the draws carry their own state).
void ps2e_graphics_set_up_render_target(ps2e_u32 target) {
    auto* t = GuestMutable<RenderTargetDescription>(target);
    if (!t) return;
    const float left = kGsMiddle - float(t->displayWidth) * 0.5f + float(t->offsetX);
    const float top = kGsMiddle - float(t->displayHeight) * 0.5f + float(t->offsetY);
    const float height = float(t->height), width = float(t->width);
    for (auto& row : t->clipToScreen)
        for (float& v : row) v = 0.f;
    t->clipToScreen[0][0] = width * 8.0f;
    t->clipToScreen[1][1] = -height * 8.0f;
    t->clipToScreen[2][2] = -8388607.0f;
    t->clipToScreen[3][0] = (left + width * 0.5f) * 16.0f;
    t->clipToScreen[3][1] = (top + height * 0.5f) * 16.0f;
    t->clipToScreen[3][2] = 8388607.0f;
    t->clipToScreen[3][3] = 1.0f;
    t->x = int32_t(left);
    t->y = int32_t(top);
}

// ---- present ---------------------------------------------------------------------------------------------------------
void ps2e_graphics_present(ps2e_u32 commands) { PresentFrame(commands != 0); }
void ps2e_graphics_present_from_interrupt(ps2e_u32 commands) {
    (void)commands;
    PresentFrame(true);
}
void ps2e_graphics_wait_sent(void) { Once("WaitSent: draws are synchronous"); }
// SubmitBuckets is wasm-side (graphics_resources.cpp): the frame's memory starts over there; the buckets are drawn at Present
void ps2e_graphics_finish_frame(void) { Once("FinishFrame"); }
void ps2e_graphics_finish_scene(ps2e_s32 effects) {
    (void)effects;
    Once("FinishScene: screen effects are not drawn yet; 2D draws are already in their buckets");
}
// ResetBuckets (wasm side) -> the frame's buckets start over
void ps2e_gfx2_frame_reset(ps2e_s32 movie) {
    (void)movie;
    ClearBuckets();
}

// StartFrame: the frame's clear as the first draw of bucket 0 -- the strips of the frame's rectangle in the clear colour
// (colour and/or depth), as frame.cpp wrote them.
void ps2e_graphics_start_frame(ps2e_u32 frame) {
    const FrameStart* f = Guest<FrameStart>(frame);
    if (!f) return;
    g.frame = *f;
    g.have_frame = true;
    g.start_frames++;
    EnsureDevice();
    if (!f->clearColor && !f->clearDepth) return;
    g.clears++;
    ps2e_draw_state st{};
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = f->clearDepth ? 1 : 0;
    st.no_colour_write = f->clearColor ? 0 : 1;
    const uint32_t colour = uint32_t(f->red) | uint32_t(f->green) << 8 | uint32_t(f->blue) << 16 | 0x80u << 24;
    const float ox = GsOriginX(), oy = GsOriginY();
    const float x0 = float(f->x) - ox, y0 = float(f->y) - oy, x1 = float(f->x + f->width) - ox, y1 = float(f->y + f->height) - oy;
    Draw d;
    d.state = st;
    auto V = [&](float x, float y) {
        ps2e_draw_vertex v = Vertex(x, y, 0, 0, colour);
        v.z = 0.f;   // the GS far plane: the depth clear
        return v;
    };
    d.verts = {V(x0, y0), V(x1, y0), V(x0, y1), V(x1, y0), V(x1, y1), V(x0, y1)};
    ::Record(TE_GFX_BUCKET_SKY, std::move(d));
}

void ps2e_graphics_use_helper_programs(ps2e_s32 set, ps2e_s32 wait) {
    (void)set;
    (void)wait;
    Once("UseHelperPrograms: VU0 microcode sets have no meaning here (Platform::Math is native)");
}

// ---- 2D (draw2d.cpp) -------------------------------------------------------------------------------------------------
void ps2e_graphics_draw_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 place, ps2e_u32 texture) {
    const Rectangle* p = Guest<Rectangle>(place);
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!p || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = p->x * sw + FrameCornerX(TargetOffsetX());
    const float top = p->y * sh + FrameCornerY(TargetOffsetY());
    const float right = left + p->width * sw, bottom = top + p->height * sh;
    g.sprites++;
    if (g.log && g.sprites <= 64)
        std::fprintf(stderr, "[te.gfx] sprite #%llu material %#x kind %u bucket %u shader %#x settings %#llx tex %#x handle %u colour %#x "
                     "place (%.3f %.3f %.3f %.3f) -> gs (%.1f %.1f)-(%.1f %.1f) origin (%.0f %.0f) uv (%.2f %.2f %.2f %.2f)\n",
                     (unsigned long long)g.sprites, material, m.material ? m.material->kind : 99u, m.bucket,
                     m.material ? m.material->shaders[0] : 0u, m.shader ? (unsigned long long)m.shader->settings : 0ull,
                     m.shader ? m.shader->texture : 0u, m.texture_handle, uint32_t(colour), p->x, p->y, p->width, p->height,
                     left, top, right, bottom, GsOriginX(), GsOriginY(), t->x, t->y, t->width, t->height);
    RecordQuad(m.bucket, ::StateOf(m), left, top, right, bottom, t->x, t->y, t->x + t->width, t->y + t->height, uint32_t(colour));
}

void ps2e_graphics_draw_turned_sprite(ps2e_u32 material, ps2e_s32 colour, ps2e_u32 corners, ps2e_u32 texture) {
    const Vector2* c = reinterpret_cast<const Vector2*>(te::GuestPointer(corners, sizeof(Vector2) * 4));
    const Rectangle* t = Guest<Rectangle>(texture);
    if (!c || !t) return;
    const MaterialView m = ::ReadMaterial(material);
    const float sw = GameScreenW(), sh = GameScreenH();
    const float left = FrameCornerX(0), top = FrameCornerY(0);
    const float ox = GsOriginX(), oy = GsOriginY();
    // The corners show the texture area's corner, the corner a height from it, the corner a width from it and the opposite
    const Vector2 uv[4] = {{t->x, t->y}, {t->x, t->y + t->height}, {t->x + t->width, t->y}, {t->x + t->width, t->y + t->height}};
    ps2e_draw_vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = Vertex(c[i].x * sw + left - ox, c[i].y * sh + top - oy, uv[i].x, uv[i].y, uint32_t(colour));
    g.turned_sprites++;
    Draw d;
    d.state = ::StateOf(m);
    ClampWholeTexture(d.state, t->x, t->y, t->x + t->width, t->y + t->height);
    d.verts = {v[0], v[1], v[2], v[1], v[3], v[2]};   // the strip 0 1 2 3 as triangles
    ::Record(m.bucket, std::move(d));
}

void ps2e_graphics_draw_strip(ps2e_u32 material, ps2e_s32 count, ps2e_u32 places, ps2e_u32 colours) {
    if (count < 3 || count > 4096) return;
    const Vector2* p = reinterpret_cast<const Vector2*>(te::GuestPointer(places, sizeof(Vector2) * size_t(count)));
    const uint32_t* c = reinterpret_cast<const uint32_t*>(te::GuestPointer(colours, 4 * size_t(count)));
    if (!p || !c) return;
    const MaterialView m = ::ReadMaterial(material);
    const float left = FrameCornerX(TargetOffsetX()) - GsOriginX(), top = FrameCornerY(TargetOffsetY()) - GsOriginY();
    g.strips++;
    Draw d;
    d.state = ::StateOf(m);
    d.state.texture = 0;
    d.verts.reserve(size_t(count - 2) * 3);
    for (int i = 2; i < count; ++i) {
        // a strip's triangle i: vertices i-2, i-1, i
        for (int k = i - 2; k <= i; ++k) d.verts.push_back(Vertex(p[k].x + left, p[k].y + top, 0, 0, c[k]));
    }
    ::Record(m.bucket, std::move(d));
}

// Font::Draw (wasm side) -> the text item about to be expanded into quads: the string, where (screen fractions) and how it is
// aligned. Kept for the text-draw census only (the diag line of the quads that follow names the string).
struct TextItemCensus { uint32_t text = 0; float x = 0, y = 0; int alignment = 0; };
static TextItemCensus g_text_item;

void ps2e_gfx2_text_item(ps2e_u32 text, ps2e_f32 x, ps2e_f32 y, ps2e_s32 alignment) {
    g_text_item.text = text;
    g_text_item.x = x;
    g_text_item.y = y;
    g_text_item.alignment = alignment;
}

// Font::Draw (wasm side) -> the glyph quads of one page material: GS pixel corners, texels of the page
void ps2e_gfx2_text_quads(ps2e_u32 material, ps2e_u32 quads, ps2e_s32 count) {
    if (count <= 0 || count > 4096) return;
    const auto* q = reinterpret_cast<const te_gfx_text_quad*>(te::GuestPointer(quads, sizeof(te_gfx_text_quad) * size_t(count)));
    if (!q) return;
    const MaterialView m = ::ReadMaterial(material);
    ps2e_draw_state st = ::StateOf(m);
    float tw = 1.f, th = 1.f;
    if (m.texture) {
        tw = float(1 << m.texture->texture.widthPower);
        th = float(1 << m.texture->texture.heightPower);
    }
    const float ox = GsOriginX(), oy = GsOriginY();
    g.text_quads += uint64_t(count);
    static const bool textDiag = std::getenv("TE_GFX_DIAG_TEXT") != nullptr;
    if (const char* diag = textDiag ? "all" : std::getenv("TE_GFX_DIAG_PRESENT")) {
        if (textDiag || g.presents.load() + 1 == uint64_t(std::atol(diag))) {
            // The TEXT-QUAD HASH (the language judge): FNV-1a over every glyph's texel rectangle and its size -- the glyph sequence of
            // the string drawn, independent of where on the frame, how large and in what colour it is drawn; two draws of the same string
            // from the same font hash alike, a translation differs.
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](float f) { uint32_t v; std::memcpy(&v, &f, 4); for (int i = 0; i < 4; ++i) { h ^= uint8_t(v >> (8 * i)); h *= 1099511628211ull; } };
            // The BOX of every quad of this call in frame pixels (the census: where the string is on the frame), and the frame's size
            float bx0 = q[0].x0, by0 = q[0].y0, bx1 = q[0].x1, by1 = q[0].y1;
            for (int i = 0; i < count; ++i) {
                mix(q[i].u0); mix(q[i].v0); mix(q[i].u1); mix(q[i].v1);   // texels only: the selected row breathes (its size varies)
                bx0 = std::min(bx0, q[i].x0); by0 = std::min(by0, q[i].y0); bx1 = std::max(bx1, q[i].x1); by1 = std::max(by1, q[i].y1);
            }
            const char* text = g_text_item.text ? te::GuestString(g_text_item.text, 128) : nullptr;
            std::fprintf(stderr, "[te.gfx.diag] present %llu text %d quads material %#x tex %u tfx %d tcc %d colour %08x first (%.1f %.1f)-(%.1f %.1f) glyph %.1fx%.1f quadhash %016llx"
                         " box (%.1f %.1f)-(%.1f %.1f) frame %dx%d at (%.3f %.3f) align %#x string '%s'\n", (unsigned long long)g.presents.load() + 1, count,
                         material, st.texture, st.tfx, st.tcc, q[0].colour, q[0].x0 / 16.f - ox, q[0].y0 / 16.f - oy, q[0].x1 / 16.f - ox, q[0].y1 / 16.f - oy,
                         (q[0].x1 - q[0].x0) / 16.f, (q[0].y1 - q[0].y0) / 16.f, (unsigned long long)h,
                         bx0 / 16.f - ox, by0 / 16.f - oy, bx1 / 16.f - ox, by1 / 16.f - oy, ScreenW(), ScreenH(), g_text_item.x, g_text_item.y,
                         unsigned(g_text_item.alignment), text ? text : "");
        }
    }
    Draw d;
    d.state = st;
    d.verts.reserve(size_t(count) * 6);
    for (int i = 0; i < count; ++i) {
        const float x0 = q[i].x0 / 16.f - ox, y0 = q[i].y0 / 16.f - oy, x1 = q[i].x1 / 16.f - ox, y1 = q[i].y1 / 16.f - oy;
        const float u0 = q[i].u0 / 16.f / tw, v0 = q[i].v0 / 16.f / th, u1 = q[i].u1 / 16.f / tw, v1 = q[i].v1 / 16.f / th;
        const uint32_t colour = q[i].colour;
        d.verts.push_back(Vertex(x0, y0, u0, v0, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
        d.verts.push_back(Vertex(x1, y0, u1, v0, colour));
        d.verts.push_back(Vertex(x1, y1, u1, v1, colour));
        d.verts.push_back(Vertex(x0, y1, u0, v1, colour));
    }
    ::Record(m.bucket, std::move(d));
}

// ---- textures (wasm side ReadTexture -> the decoded image) -----------------------------------------------------------
ps2e_s32 ps2e_gfx2_texture_loaded(ps2e_u32 texture, ps2e_u32 header, ps2e_u32 image, ps2e_s32 size) {
    const auto* h = Guest<te_gfx_texture_header>(header);
    const uint8_t* bytes = te::GuestPointer(image, size_t(size));
    if (!h || !bytes || size <= 0 || !EnsureDevice()) return 0;
    te::gfx::DecodedTexture decoded;
    std::string error;
    if (!te::gfx::DecodeTextureResource(*h, bytes, size_t(size), decoded, &error)) {
        g.texture_failures++;
        std::fprintf(stderr, "[te.gfx] texture %#x: decode failed: %s (psm %u upload %u %ux%u levels %u)\n", texture, error.c_str(),
                     h->pixelFormat, h->uploadFormat, 1u << h->widthPower, 1u << h->heightPower, h->levels);
        return 0;
    }
    auto old = g.textures.find(texture);
    if (old != g.textures.end()) {
        const uint32_t stale = old->second;
        OnRender([stale] { ps2e_draw_texture_destroy(g.dev, stale); });
        g.textures.erase(old);
    }
    if (const char* dir = std::getenv("TE_GFX_DUMP_TEXTURES")) {
        char path[1024];
        std::snprintf(path, sizeof path, "%s/texture_%08x.ppm", dir, texture);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", decoded.width, decoded.height);
            for (uint32_t px : decoded.rgba) { const uint8_t rgb[3] = {uint8_t(px), uint8_t(px >> 8), uint8_t(px >> 16)}; std::fwrite(rgb, 1, 3, f); }
            std::fclose(f);
        }
    }
    uint32_t handle = 0;
    OnRender([&] { handle = CreateTexture(decoded); });
    if (!handle) return 0;
    g.textures[texture] = handle;
    g.texture_sources[texture] = Gfx::TextureSource{*h, std::vector<uint8_t>(bytes, bytes + size)};
    g.textures_loaded++;
    if (g.log)
        std::fprintf(stderr, "[te.gfx] texture %#x: %dx%d psm %u (upload %u) levels %u -> handle %u (%u transfers, %u bytes)\n", texture,
                     decoded.width, decoded.height, h->pixelFormat, h->uploadFormat, h->levels, handle, decoded.transfers,
                     decoded.image_bytes);
    return ps2e_s32(handle);
}

void ps2e_gfx2_texture_released(ps2e_u32 texture) {
    g.texture_sources.erase(texture);
    auto it = g.textures.find(texture);
    if (it == g.textures.end()) return;
    const uint32_t handle = it->second;
    if (g.dev) OnRender([handle] { ps2e_draw_texture_destroy(g.dev, handle); });
    g.textures.erase(it);
}

// ---- culling, models, sky, shadows: graphics_scene.cpp ---------------------------------------------------------------
// ---- screen models, particles, decals ------------------------------------------------------------------------------------
// (DrawSkin / DrawBlendSkin: graphics_scene.cpp; the screen models -- BeginScreenModel .. DrawScreenModel, the skid
// marks: graphics_screen_models.cpp)

// (the particles: graphics_particles.cpp; the decals -- LoadDecalView / LoadDecalType / AgeDecal / DrawDecals /
// InitDecalBlock / InitDecalUvPacket: graphics_decals.cpp)

}  // extern "C"

// ---- the movie owner's hooks (movie_player.cpp; declared in te_graphics_hooks.h) ------------------------------------
// The device the movie's picture texture is created on (on demand, as every other draw's), and the picture's quad
// into the movies bucket (27, where the PS2 player's sprites went): corners in FRAME pixels (the game's Movie::Area is
// in display pixels, no GS origin), u/v fractions, the texture's colours as they are (TFX decal, no blending, in front
// of everything and written -- gsvalues.h TexturedSprite in white), bilinear.
extern "C" ps2e_draw* te_graphics_device(void) { return EnsureDevice() ? g.dev : nullptr; }
// the CT16 present toggled on the device (on the render thread when there is one); an engine without the CT16
// scanout keeps the request and reports 0 (the frame stays 32-bit)
extern "C" int te_graphics_set_present_ct16(int on) {
    g.present_ct16 = on != 0;
    if (!EnsureDevice()) return 1;
    OnRender([] { ps2e_draw_set_present_ct16(g.dev, g.present_ct16 ? 1 : 0, 0); });
    static bool said = false;
    if (!said) { said = true; std::fprintf(stderr, "[te.gfx] present: %s\n", g.present_ct16 ? "CT16 (the retail dithered 16-bit copy)" : "the frame's colours"); }
    return 1;
}
extern "C" int te_graphics_present_ct16(void) { return g.present_ct16 ? 1 : 0; }
extern "C" void te_graphics_on_render(void (*fn)(void* user), void* user) { OnRender([fn, user] { fn(user); }); }
extern "C" void te_graphics_record_movie_quad(uint32_t texture, float x0, float y0, float x1, float y1, float u0, float v0,
                                              float u1, float v1) {
    ps2e_draw_state st{};
    st.texture = texture;
    st.gouraud = 0;
    st.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    st.depth_write = 1;
    st.blend = 0;
    st.tfx = PS2E_DRAW_TFX_DECAL;
    st.tcc = 1;
    st.linear = 1;
    st.wrap_u = PS2E_DRAW_WRAP_CLAMP;
    st.wrap_v = PS2E_DRAW_WRAP_CLAMP;
    Draw d;
    d.state = st;
    const uint32_t white = 0x80FFFFFFu;
    d.verts = {Vertex(x0, y0, u0, v0, white), Vertex(x1, y0, u1, v0, white), Vertex(x0, y1, u0, v1, white),
               Vertex(x1, y0, u1, v0, white), Vertex(x1, y1, u1, v1, white), Vertex(x0, y1, u0, v1, white)};
    ::Record(TE_GFX_BUCKET_MOVIES, std::move(d));
}
