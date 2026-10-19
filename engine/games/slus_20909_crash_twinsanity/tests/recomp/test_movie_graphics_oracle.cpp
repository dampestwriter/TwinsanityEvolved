// Actual generated upload/draw output versus the owned movie producer. No
// historical captures, game process, GPU or external emulator. The consumer
// checks real GraphicsResources upload bytes; renderer pixels remain the
// parent's composition oracle, because FakeBackend does not rasterize.
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_oracle_args.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include "engine/dmac/dmac.h"
#include "engine/ee/cpu/guest_park.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/format/swizzle.h"
#include "engine/gs/state/gs_state.h"
#include "engine/ipu/hle/frame_writer.h"
#include "engine/ipu/hle/guest_memory.h"
#include "engine/vif/vif.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"
#include "tests/lib/fake_backend.h"

namespace {
using Movie = title::MovieGraphics;
using Decoder = title::MovieDecoder;
using Graphics = title::Graphics;
using Resources = title::GraphicsResources;
using Frame = title::FrameEnvironment;
using Draw = ps2r::gs::DrawIr;
using Picture = ps2r::ipu::DecodedPicture;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kController = 0x10000, kDecoder = kController + 0x80;
constexpr uint32_t kStack = 0x1f00000, kArena = 0x400000;
constexpr uint32_t kAnchor = 0x11000, kPacket = 0x12000, kBucket = 0x323c48;
constexpr uint32_t kChain = 0x323300, kGp = 0x3114f0;
constexpr uint32_t kPictureBase = 0x180000; // retained output only, never an owned identity
size_t checks = 0, cases = 0;
[[noreturn]] void Failed(const char* expression, int line) {
    std::fprintf(stderr, "MOV3 case %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define CHECK(x) do { ++checks; if (!(x)) Failed(#x, __LINE__); } while (false)

void Append(Bytes& bytes, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
}
void Word(Bytes& bytes, uint64_t value) {
    for (unsigned n = 0; n < 8; ++n) bytes.push_back(uint8_t(value >> (n * 8)));
}

struct Sink : ps2r::gs::DrawIrSink {
    std::vector<Draw> draws;
    void OnDraw(const Draw& d) override { draws.push_back(d); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};
struct Memory : ps2r::ipu::GuestMemory {
    MainMemory ram;
    uint8_t* Translate(uint32_t address, size_t size) override {
        return ram.Pointer(MainMemory::Phys(address), size);
    }
    uint32_t Read32(uint32_t address) override {
        uint32_t value = 0;
        auto* p = Translate(address, sizeof(value)); CHECK(p);
        std::memcpy(&value, p, sizeof(value)); return value;
    }
    void Write32(uint32_t address, uint32_t value) override {
        auto* p = Translate(address, sizeof(value)); CHECK(p);
        std::memcpy(p, &value, sizeof(value));
    }
};
struct Rig;
Rig* current = nullptr;

struct Rig {
    Memory memory;
    Sink sink;
    ps2r::gs::GsState gs;
    GsStateSink gs_sink{gs};
    GifRegs gif_regs;
    GifParser parser{gs_sink, gif_regs};
    GifArbiter arbiter{parser};
    Scratchpad scratchpad;
    VuUnit vu0{0}, vu1{1, &arbiter};
    Vif vif{vu0, vu1, arbiter, gif_regs};
    Dmac dmac{memory.ram, scratchpad, vif, arbiter};
    ps2r::test::FakeBackend backend;
    Resources resources{backend};
    Graphics graphics;
    std::unique_ptr<Decoder> decoder;
    std::unique_ptr<Movie> movie;
    CpuContext context{};
    std::map<uint32_t, Decoder::Body> bindings;
    std::map<uint32_t, uint64_t> originals;
    std::vector<std::vector<uint8_t>> uploaded;
    std::vector<Picture> views;
    std::vector<Graphics::Lease> view_allocations;
    std::unique_ptr<Frame> scene_source;
    std::map<uint32_t, Frame::Body> scene_bindings;
    std::vector<Frame::SceneFrame> delivered_scenes;
    uint64_t scene_reads = 0, scene_captures = 0;
    bool prohibit_scene_reads = false;
    std::vector<Draw> draws;
    std::vector<char> order;
    std::vector<std::function<void()>> deferred;
    struct Resident {
        std::weak_ptr<const Movie::Image> image;
        Graphics::Lease allocation;
    };
    std::map<uint32_t, Resident> residents;
    Graphics::Counters completed;
    Bytes input;
    uint32_t width = 0, height = 0, next_picture = 0, source_frame = 0;
    uint64_t gif_starts = 0, timer_pumps = 0, presents = 0;
    bool selected = false, deferred_output = false;
    bool reject_register = false, reject_draw = false, exhausted = false;
    explicit Rig(bool selection, bool services = false, bool source_frames = false)
        : selected(selection), deferred_output(services) {
        current = this;
        gs.SetSink(&sink);
        if (source_frames) {
            CHECK(selected && deferred_output);
            scene_source = std::make_unique<Frame>([this](uint32_t at, void* out, size_t size) {
                CHECK(!prohibit_scene_reads);
                ++scene_reads;
                const auto* p = memory.Translate(at, size);
                if (!p) return false;
                std::memcpy(out, p, size);
                return true;
            });
            // Compose the real observer with this boundary fixture. The six
            // generated movie bodies remain the retained-output oracle; these
            // setup no-ops are not a frame-source model-retirement claim.
            CHECK(scene_source->Install(
                [](uint32_t) -> Frame::Body { return [](CpuContext&, uint32_t) {}; },
                [this](uint32_t at, Frame::Body body) {
                    return scene_bindings.emplace(at, std::move(body)).second;
                }));
        }
        auto hooks = Decoder::Hooks{};
        hooks.create = hooks.reset = [](CpuContext&) {};
        hooks.initialize = [] {};
        hooks.destroy = [](uint32_t) {};
        hooks.has_pending = [](uint32_t) { return true; };
        hooks.picture_rgb16 = [](CpuContext&) { CHECK(false); };
        hooks.picture_rgb32 = [&](CpuContext& c) {
            Picture p; Serve(c, p);
            if (c.cpuRegs.GPR.r[2].SL[0] == 0) {
                auto* out = memory.Translate(c.cpuRegs.GPR.r[5].UL[0], p.rgba.size());
                CHECK(out);
                ps2r::ipu::FrameWriter::WriteRgb32Strips(out, p);
            }
        };
        hooks.call = [&](CpuContext& c, uint32_t address) { Call(address, c); };
        decoder = std::make_unique<Decoder>(memory, hooks);
        CHECK(decoder->Install(Capture(), Installer(), true) == 7);
        Movie::Hooks output;
        output.picture = [&](CpuContext& c, Picture& p) { Serve(c, p); };
        output.register_image = [&](const ps2r::render::SurfaceDesc& d, Graphics::Handle& h) {
            if (reject_register) return false;
            CHECK(!d.renderable && !(d.width & 15) && !(d.height & 15));
            return resources.Register(graphics, d, &h) == Resources::Result::Ok;
        };
        output.record = [&](const Graphics::Anchor& anchor, const Draw& draw, const Graphics::Handle& image) {
            if (reject_draw) return false;
            Graphics::Lease view;
            CHECK(graphics.RetainResource(image, &view) == Graphics::Result::Ok);
            view_allocations.push_back(std::move(view));
            return graphics.RecordDraw(anchor, draw, {image}) == Graphics::Result::Ok;
        };
        output.present = [&] {
            CHECK(graphics.PendingEntries() == 0); // parent's required submit-before-present order
            CHECK(deferred.empty());
            order.push_back('P'); ++presents; return true;
        };
        if (deferred_output) {
            output.register_image = {};
            output.record = {};
            Movie::OutputServices services;
            services.upload = [this](Movie::ImageLease image) {
                if (reject_register) return false;
                CHECK(image->source_id == 0);
                // Root captures source ordering/frame before accepting. The
                // immutable image survives Close, next decode and guest reuse.
                deferred.push_back([this, image] {
                    Graphics group;
                    // QueuePicture follows Link: no completed scene snapshot
                    // is required for this image-only ordered upload group.
                    CHECK(group.BeginFrame(0) == Graphics::Result::Ok);
                    const auto& p = image->pixels;
                    Graphics::Handle handle;
                    CHECK(resources.Register(group, {uint32_t(p.width), uint32_t(p.height),
                        ps2r::render::SurfaceFormat::Rgba8, false, 1}, &handle) == Resources::Result::Ok);
                    Graphics::Lease allocation;
                    CHECK(group.RetainResource(handle, &allocation) == Graphics::Result::Ok);
                    CHECK(group.RecordUpload(group.End(Movie::kBucket), handle,
                        {0, 0, p.width, p.height}, size_t(p.width) * 4, p.rgba) == Graphics::Result::Ok);
                    CHECK(group.ReleaseResource(handle) == Graphics::Result::Ok);
                    SubmitGroup(group, image);
                    residents[image->id] = {image, std::move(allocation)};
                });
                return true;
            };
            services.record = [this](const Draw& draw, Movie::ImageLease image) {
                if (reject_draw) return false;
                CHECK(image->source_id != 0 && image->source_id != image->id);
                Frame::SceneFrame scene;
                const bool has_scene = bool(scene_source);
                if (has_scene) {
                    ++scene_captures;
                    if (!scene_source->CaptureScene(Movie::kBucket, scene)) return false;
                    if (draw.raster.frame.fbw != scene.width) return false;
                }
                const auto frame = has_scene ? scene.source_frame : source_frame;
                deferred.push_back([this, draw, image, frame, scene, has_scene] {
                    if (has_scene) delivered_scenes.push_back(scene);
                    Graphics group;
                    CHECK(group.BeginFrame(frame) == Graphics::Result::Ok);
                    Graphics::Handle handle;
                    const auto found = residents.find(image->id);
                    if (found == residents.end()) {
                        // Upload this immutable sampled view in the same source
                        // bucket as its first draw. Older views keep their bytes.
                        const auto& p = image->pixels;
                        CHECK(resources.Register(group, {uint32_t(p.width), uint32_t(p.height),
                            ps2r::render::SurfaceFormat::Rgba8, false, 1}, &handle) == Resources::Result::Ok);
                        Graphics::Lease allocation;
                        CHECK(group.RetainResource(handle, &allocation) == Graphics::Result::Ok);
                        CHECK(group.RecordUpload(group.End(Movie::kBucket), handle,
                            {0, 0, p.width, p.height}, size_t(p.width) * 4, p.rgba) == Graphics::Result::Ok);
                        residents[image->id] = {image, std::move(allocation)};
                    } else {
                        // Rebind the canonical lifetime, not another queue's
                        // handle or an external import of its owned surface.
                        const auto& resource = found->second.allocation;
                        CHECK(group.RegisterResource(resource->description, resource->lifetime, &handle) ==
                              Graphics::Result::Ok);
                    }
                    CHECK(group.RecordDraw(group.End(Movie::kBucket), draw, {handle}) == Graphics::Result::Ok);
                    CHECK(group.ReleaseResource(handle) == Graphics::Result::Ok);
                    SubmitGroup(group, image);
                });
                return true;
            };
            movie = std::make_unique<Movie>(memory, *decoder, output, std::move(services));
        } else {
            movie = std::make_unique<Movie>(memory, *decoder, graphics, output);
        }
        CHECK(movie->Install(Capture(), Installer(), selected) == 3);
        CHECK(movie->Selected() == selected);
        context.cpuRegs.GPR.r[29].UD[0] = kStack;
        context.cpuRegs.GPR.r[28].UD[0] = kGp;
        memory.Write32(0x2e7d78 + 8, 0x1000a000); // NTSC DMA base table's GIF entry
        memory.Write32(0x3c6b40 + 8, 0); // source's saved CHCR settings
    }
    ~Rig() {
        movie->Close();
        CHECK(deferred.empty());
        residents.clear();
        CHECK(graphics.Reset() == Graphics::Result::Ok);
        CHECK(resources.CollectAfterQuiescence() == Resources::Result::Ok);
        current = nullptr;
    }
    Decoder::Capture Capture() {
        return [&](uint32_t address) -> Decoder::Body {
            return [address](CpuContext& c, uint32_t e) {
                CHECK(e == address || e == 0);
                CHECK(movie_oracle::RunGenerated(address, c));
            };
        };
    }
    Decoder::Installer Installer() {
        return [&](uint32_t a, Decoder::Body body) { bindings[a] = std::move(body); return true; };
    }
    void Call(uint32_t address, CpuContext& c) {
        const auto binding = bindings.find(address);
        if (binding != bindings.end()) { binding->second(c, address); return; }
        if (movie_oracle::RunGenerated(address, c)) return;
        if (address == Decoder::kDemultiplex) {
            const uint32_t header = memory.Read32(kDecoder + 0x100);
            const std::array<uint8_t, 8> sequence{0, 0, 1, 0xb3, uint8_t(width >> 4),
                uint8_t((width << 4) | (height >> 8)), uint8_t(height), 0x15};
            std::memcpy(memory.Translate(header, sequence.size()), sequence.data(), sequence.size());
            c.cpuRegs.GPR.r[2].SD[0] = 1;
            return;
        }
        // Shared lifecycle setup and CPU cache operation have no graphics
        // output. No replaced upload/draw/queue body is stubbed.
        CHECK(address == 0x2b1120 || address == 0x2cfbc0);
    }
    void Serve(CpuContext& c, Picture& picture) {
        ++timer_pumps;
        if (exhausted) { c.cpuRegs.GPR.r[2].SD[0] = -1; return; }
        picture = Pixels(width, height, next_picture++);
        Word(input, 1); Word(input, width); Word(input, height);
        Append(input, picture.rgba.data(), picture.rgba.size());
        c.cpuRegs.GPR.r[2].SD[0] = 0;
    }
    static Picture Pixels(uint32_t w, uint32_t h, uint32_t frame) {
        Picture p{int(w), int(h), std::vector<uint8_t>(size_t(w) * h * 4)};
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            const size_t at = (size_t(y) * w + x) * 4;
            p.rgba[at] = uint8_t(x + frame * 37);
            p.rgba[at + 1] = uint8_t(y ^ (frame * 71));
            p.rgba[at + 2] = uint8_t((x >> 4) + (y >> 3) * 7);
            p.rgba[at + 3] = 255;
        }
        return p;
    }
    void Start(uint32_t w, uint32_t h, bool create = true) {
        width = w; height = h; next_picture = 0; exhausted = false;
        if (create) {
            memory.Write32(Decoder::kArenaNext, kArena);
            memory.Write32(Decoder::kArenaEnd, kArena + 0x800000);
        }
        const std::array<uint32_t, 8> args{create ? 1u : 0u, kDecoder, 1, 2, 1, 0, 0, 0};
        for (size_t i = 0; i < args.size(); ++i) context.cpuRegs.GPR.r[4 + i].UD[0] = args[i];
        memory.Write32(kStack, 0);
        memory.Write32(kStack + 8, 1);
        memory.Write32(kStack + 16, (w + 63) / 64);
        memory.Write32(kStack + 24, 0x100);
        Call(Decoder::kStart, context);
        CHECK(context.cpuRegs.GPR.r[2].UL[0] == 1);
        CHECK(!decoder->CanCheckpoint());
        memory.Write32(kController + 4, w); memory.Write32(kController + 8, h);
        if (selected) {
            CHECK(originals[0x2b1448] == 0);
            CHECK(memory.Read32(kDecoder + 0x94) == 0 && memory.Read32(kDecoder + 0xa8) == 0);
        } else CHECK(originals[0x2b1448] >= 2);
    }
    void Decode() {
        context.cpuRegs.GPR.r[4].UD[0] = kController + 0x44;
        context.cpuRegs.GPR.r[5].UD[0] = kPictureBase / 256;
        Call(Movie::kDecodeAndUpload, context);
    }
    // This is only a fixture for the retained Start/Step QueueDecoded
    // bookkeeping; no replacement algorithm or production gameplay hook.
    void Enqueue() {
        const uint32_t queued = memory.Read32(kController + 0x40);
        CHECK(queued < 2);
        memory.Write32(kController + 0x38 + queued * 4, memory.Read32(kController + 0x44));
        memory.Write32(kController + 0x40, queued + 1);
    }
    void Queue() {
        context.cpuRegs.GPR.r[4].UD[0] = kController;
        Call(Movie::kQueuePicture, context);
        arbiter.drain();
        if (!selected) {
            std::vector<uint8_t> pixels(size_t(width) * height * 4);
            ps2r::gs::Swizzle::VramToLinear(gs.Vram().Bytes(),
                {ps2r::gs::Psm::CT32, kPictureBase / 256, (width + 63) / 64},
                {0, 0, width, height}, pixels.data(), size_t(width) * 4);
            uploaded.push_back(std::move(pixels));
        }
    }
    void DrawFrame(bool tv, bool wide, uint32_t screen_width = 512, uint32_t screen_height = 448) {
        Word(input, 2); Word(input, tv); Word(input, wide);
        Word(input, screen_width); Word(input, screen_height);
        Word(input, memory.Read32(kController + 0x1e4));
        memory.Write32(kController, wide ? 0x40 : 0);
        *memory.Translate(Movie::kWidescreenTv, 1) = tv;
        memory.Write32(Movie::kDisplayWidth, screen_width);
        memory.Write32(Movie::kDisplayHeight, screen_height);
        memory.Write32(kBucket + 8, kAnchor); memory.Write32(kBucket + 16, 0);
        memory.Write32(kChain + 16, kPacket);
        memory.Write32(kGp - 0x6d74, kPictureBase / 8192);
        // The source frame setup initializes context 2, independently of the
        // current GS snapshot. Deliberately nonzero inactive ALPHA is allowed.
        gs.WriteRegister(0x41, uint64_t(1024) << 16 | uint64_t(1024) << 48);
        gs.WriteRegister(0x46, 1);
        gs.WriteRegister(0x44, 0x6071243571603524ull);
        gs.WriteRegister(0x3b, uint64_t(0x80) << 32);
        gs.WriteRegister(0x43, 0x7f00000044ull);
        context.cpuRegs.GPR.r[4].UD[0] = kController;
        Call(Movie::kDraw, context);
        if (!selected && memory.Read32(kController + 0x1e4) >= 2) {
            const uint32_t qwords = memory.Read32(kPacket) & 0xffff;
            parser.ReceiveData(GifPath::Path2, memory.Translate(kPacket + 16, qwords * 16), qwords * 16);
            CHECK(parser.CanAcceptDirect());
            CHECK(sink.draws.size() == draws.size() + 1);
            draws.push_back(sink.draws.back());
        }
    }
    void BeginFrame(uint32_t frame) {
        source_frame = frame;
        if (!deferred_output) CHECK(graphics.BeginFrame(frame) == Graphics::Result::Ok);
    }
    void SceneCall(uint32_t entry, uint32_t argument = 0) {
        CpuContext scene_context{};
        scene_context.cpuRegs.GPR.r[4].UL[0] = argument;
        scene_bindings.at(entry)(scene_context, entry);
    }
    void SetUpScene(uint32_t w = 512, uint32_t h = 448) {
        constexpr uint32_t target = 0x1f10000;
        memory.Write32(Movie::kDisplayWidth, w);
        memory.Write32(Movie::kDisplayHeight, h);
        memory.Write32(title::MaterialState::kDisplayPixelBytes, 2);
        memory.Write32(Frame::kDrawPixelBytes, 4);
        memory.Write32(Frame::kFrameBufferPage, w * h * 2 / 8192);
        memory.Write32(title::MaterialState::kDepthBufferPage, w * h * 6 / 8192);
        memory.Write32(target + 0x18, w);
        memory.Write32(target + 0x1c, h);
        SceneCall(Frame::kSetUpFrameAddress, target);
    }
    void SubmitGroup(Graphics& group, Movie::ImageLease image = {}) {
        Graphics::Hooks hooks;
        hooks.upload = [&](const Graphics::Upload& upload) {
            CHECK(resources.Upload(upload) == Resources::Result::Ok);
            ps2r::render::SurfaceId surface;
            CHECK(resources.Resolve(upload.resource, &surface) == Resources::Result::Ok);
            const auto& pixels = backend.At(surface).rgba;
            const bool view = image ? image->source_id != 0 :
                std::find(view_allocations.begin(), view_allocations.end(), upload.resource) !=
                    view_allocations.end();
            if (view) {
                const auto& d = upload.resource->description;
                views.push_back({int(d.width), int(d.height), pixels});
                order.push_back('V');
            } else {
                uploaded.push_back(pixels);
                order.push_back('U');
            }
            return true;
        };
        hooks.draw = [&](const Draw& d, const std::vector<Graphics::Lease>& leases) {
            CHECK(leases.size() == 1);
            ps2r::render::SurfaceId surface;
            CHECK(resources.Resolve(leases[0], &surface) == Resources::Result::Ok);
            CHECK(backend.At(surface).rgba == (image ? image->pixels.rgba : views.back().rgba));
            draws.push_back(d); order.push_back('D'); return true;
        };
        hooks.drain = [] { return true; };
        const auto before = group.Counts();
        CHECK(group.Submit(hooks) == Graphics::Result::Ok);
        const auto after = group.Counts();
        completed.uploads += after.uploads - before.uploads;
        completed.draws += after.draws - before.draws;
    }
    void Submit() {
        if (!deferred_output) {
            SubmitGroup(graphics);
            view_allocations.clear();
            return;
        }
        auto pending = std::move(deferred);
        deferred.clear();
        for (const auto& work : pending) work();
        pending.clear();
        for (auto it = residents.begin(); it != residents.end();) {
            if (it->second.image.expired()) it = residents.erase(it);
            else ++it;
        }
        CHECK(resources.CollectAfterQuiescence() == Resources::Result::Ok);
    }
};

Draw CanonicalDraw(Draw draw) {
    // Resource addresses intentionally die. Owned handles were independently
    // resolved and their actual upload bytes compared. Blending is disabled,
    // so inherited ALPHA is inactive. No active raster/texture field is ignored.
    CHECK(!draw.alpha_blend);
    draw.raster.frame.fbp = draw.raster.zbuf.zbp = draw.tex.tbp = 0;
    draw.raster.alpha = {};
    // Uploads advance the legacy mirror's epoch and job sequence; the selected
    // queue has an explicit upload preceding its draw instead.
    draw.seq = draw.vram_epoch = 0;
    return draw;
}

void Compare(Draw model, const Draw& selected) {
    model = CanonicalDraw(std::move(model));
    auto a = model.ToDrawRec(), b = selected.ToDrawRec();
    if (std::memcmp(&a, &b, sizeof(a))) {
        for (size_t i = 0; i < sizeof(a); ++i)
            if (reinterpret_cast<uint8_t*>(&a)[i] != reinterpret_cast<uint8_t*>(&b)[i])
                std::fprintf(stderr, "DrawRec byte %zu model=%02x selected=%02x\n", i,
                    reinterpret_cast<uint8_t*>(&a)[i], reinterpret_cast<uint8_t*>(&b)[i]);
        CHECK(false);
    }
    CHECK(model.verts.size() == selected.verts.size());
    CHECK(std::memcmp(model.verts.data(), selected.verts.data(), model.verts.size() * sizeof(model.verts[0])) == 0);
    CHECK(model.raster.dimx == selected.raster.dimx && model.raster.dthe == selected.raster.dthe);
    CHECK(model.tex.mxl == selected.tex.mxl && model.tex.lcm == selected.tex.lcm);
}

struct Output {
    std::vector<Draw> draws;
    std::vector<std::vector<uint8_t>> pixels;
    std::map<uint32_t, uint64_t> calls;
    uint64_t image_qwords = 0, gif_starts = 0;
    Movie::Counters movie;
    Bytes input;
    Graphics::Counters completed;
    std::vector<Picture> views;
};
Output Run(bool selected, uint32_t width, uint32_t height, bool tv, bool wide, bool services = false) {
    auto rig = std::make_unique<Rig>(selected, services);
    auto& r = *rig;
    r.Start(width, height);
    r.BeginFrame(0);
    r.Decode(); CHECK(r.context.cpuRegs.GPR.r[2].UL[0] == 1);
    r.Enqueue();
    r.Decode(); CHECK(r.context.cpuRegs.GPR.r[2].UL[0] == 1); // second prime stays pending
    r.memory.Write32(kController + 0x1e4, 0);
    r.DrawFrame(tv, wide);
    CHECK(r.draws.empty());
    r.Queue();
    CHECK(r.memory.Read32(kController + 0x40) == 0);
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(tv, wide);
    if (selected) {
        CHECK(r.backend.uploads == 0); // producer never calls backend
        r.Submit();
        CHECK(r.movie->Present());
        CHECK(r.order == std::vector<char>({'U','V','D','P'}));
        r.BeginFrame(1);
    }
    r.Enqueue(); // retained Step accepts the second prime
    r.Decode();  // the following pending picture
    r.Enqueue();
    CHECK(r.memory.Read32(kController + 0x40) == 2);
    r.Queue();
    r.gs.SetSrcFrame(1);
    r.DrawFrame(tv, wide);
    CHECK(r.memory.Read32(kController + 0x40) == 1);
    if (selected) {
        r.Submit();
        CHECK(r.movie->Present());
        r.BeginFrame(2);
    }
    r.Queue();
    r.gs.SetSrcFrame(2);
    r.DrawFrame(tv, wide);
    // Close before the consumer runs: pending upload/draw owns its allocation.
    r.context.cpuRegs.GPR.r[4].UD[0] = kDecoder;
    r.Call(Decoder::kClose, r.context);
    CHECK(r.decoder->CanCheckpoint());
    if (selected) {
        CHECK(!r.movie->Active() && !r.movie->Present());
        r.Submit();
        CHECK(r.movie->PendingPictures() == 0);
        CHECK((r.movie->Counts().original_calls == std::array<uint64_t, 3>{}));
        CHECK(r.resources.CollectAfterQuiescence() == Resources::Result::Ok);
        CHECK(r.resources.Counts().live_surfaces == 0);
    }
    CHECK(r.draws.size() == 3 && r.uploaded.size() == 3);
    CHECK(r.timer_pumps == 3);
    Output out{r.draws, r.uploaded, r.originals, r.gs_sink.ImageQwords(), r.gif_starts,
               r.movie->Counts(), r.input, r.completed, r.views};
    return out;
}

void TestClosedPictureCalls() {
    // OLD FMV2 accepted-backlog / SOUND_P5 Close boundaries: Close releases
    // the movie owner, not already accepted render leases. It must never
    // make an obsolete picture ID a DMA packet address or a valid checkpoint.
    for (bool after_close : {false, true}) for (uint32_t at :
            {Movie::kDecodeAndUpload, Movie::kQueuePicture, Movie::kDraw}) {
        ++cases;
        auto rig = std::make_unique<Rig>(true, true);
        auto& r = *rig;
        CHECK(r.movie->CanCheckpoint());
        if (after_close) {
            r.Start(512, 288); r.BeginFrame(9);
            r.Decode(); r.Enqueue(); r.Queue();
            r.memory.Write32(kController + 0x1e4, 2);
            r.DrawFrame(false, true);
            r.Call(Decoder::kClose, r.context);
            CHECK(r.movie->CanCheckpoint() && r.deferred.size() == 2);
        }
        r.context.cpuRegs.GPR.r[4].UD[0] =
            at == Movie::kDecodeAndUpload ? kController + 0x44 : kController;
        const auto queued = r.memory.Read32(kController + 0x40);
        const auto pending = r.deferred.size();
        const auto pumps = r.timer_pumps;
        try { r.Call(at, r.context); CHECK(false); } catch (const std::runtime_error&) {}
        std::string reason;
        CHECK(!r.movie->CanCheckpoint(&reason) && !reason.empty());
        CHECK(r.movie->Counts().errors == 1 && r.gif_starts == 0 && r.timer_pumps == pumps);
        CHECK(r.memory.Read32(kController + 0x40) == queued && r.deferred.size() == pending);
        CHECK((r.movie->Counts().original_calls == std::array<uint64_t,3>{}));
        CHECK(r.originals[at] == 0);
        r.movie->Close();
        CHECK(!r.movie->CanCheckpoint()); // cleanup cannot hide a failed output
        if (after_close) {
            r.Submit();
            CHECK(r.uploaded.size() == 1 && r.draws.size() == 1);
        }
    }
}

void TestOwnershipErrors(bool services = false) {
    ++cases;
    auto rig = std::make_unique<Rig>(true, services);
    auto& r = *rig;
    r.Start(512, 288);
    CHECK(r.movie->Present()); // source can present the cleared scene before QueuePicture
    CHECK(!r.decoder->SetPictureServices({})); // cannot deselect an active owner
    r.BeginFrame(0);
    r.Decode();
    const uint32_t old_id = r.memory.Read32(kController + 0x44);
    r.Enqueue();
    r.reject_register = true;
    try { r.Queue(); CHECK(false); } catch (const std::runtime_error&) {}
    CHECK(r.movie->Counts().errors == 1 && r.gif_starts == 0);
    CHECK(r.memory.Read32(kController + 0x40) == 1);
    CHECK(r.graphics.PendingEntries() == 0); // rejected operation did not consume the queue
    CHECK(r.deferred.empty());
    r.reject_register = false;
    r.Queue();
    r.memory.Write32(kController + 0x1e4, 2);
    r.reject_draw = true;
    try { r.DrawFrame(false, true); CHECK(false); } catch (const std::runtime_error&) {}
    CHECK(r.movie->Counts().errors == 2 && r.gs_sink.Kicks() == 0);
    r.reject_draw = false;
    r.DrawFrame(false, true);
    r.Submit();
    CHECK(r.movie->Present());
    // EOF/error may end the provider while the last picture still belongs to
    // the player. Keep it drawable until explicit Close, and keep saves refused.
    r.exhausted = true;
    r.memory.Write32(kDecoder + 0x15c, 1);
    r.Decode();
    CHECK(r.context.cpuRegs.GPR.r[2].UL[0] == 0);
    CHECK(r.movie->Active() && !r.decoder->CanCheckpoint());
    CHECK(r.movie->Present());
    r.Call(Decoder::kClose, r.context);
    CHECK(r.decoder->CanCheckpoint() && !r.movie->Present());
    // Abort/reopen uses a fresh ID and dimensions; the old ID cannot upload a
    // newly allocated picture even when the controller and arena are reused.
    r.Start(640, 480);
    r.BeginFrame(1);
    r.Decode();
    const uint32_t fresh_id = r.memory.Read32(kController + 0x44);
    CHECK(fresh_id != old_id && fresh_id != 0);
    r.Enqueue();
    r.memory.Write32(kController + 0x38, old_id);
    try { r.Queue(); CHECK(false); } catch (const std::runtime_error&) {}
    CHECK(r.movie->Counts().errors == 3 && r.gif_starts == 0);
    r.memory.Write32(kController + 0x38, fresh_id);
    r.Queue();
    r.DrawFrame(false, false);
    const auto before = r.memory.Read32(kController + 0x44);
    r.decoder->ResetAfterRestore();
    CHECK(!r.movie->Active() && r.movie->PendingPictures() == 0);
    CHECK(r.memory.Read32(kController + 0x44) == before); // restore reset never edits restored RAM
    r.Submit(); // queue owns the image even after restore invalidates its owner
    CHECK(r.resources.CollectAfterQuiescence() == Resources::Result::Ok);
    CHECK(r.resources.Counts().live_surfaces == 0);
    CHECK((r.movie->Counts().original_calls == std::array<uint64_t, 3>{}));
    // Public Restart reuses the decoder's arena after Close/restore reset.
    // It must re-arm the owned picture service without allocating guest planes.
    const uint32_t arena = r.memory.Read32(Decoder::kArenaNext);
    r.Start(640, 480, false);
    CHECK(r.memory.Read32(Decoder::kArenaNext) == arena);
    r.Decode();
    CHECK(r.context.cpuRegs.GPR.r[2].UL[0] == 1 && r.movie->Active());
    CHECK(r.memory.Read32(kController + 0x44) != fresh_id);
    r.Call(Decoder::kClose, r.context);
    // Selection readiness must fail before replacing any captured function.
    Movie unready(r.memory, *r.decoder, r.graphics, {});
    unsigned captures = 0, installs = 0;
    CHECK(unready.Install([&](uint32_t) -> Decoder::Body {
        ++captures; return [](CpuContext&, uint32_t) {};
    }, [&](uint32_t, Decoder::Body) { ++installs; return true; }, true) == 0);
    CHECK(captures == 0 && installs == 0);
    Movie::Hooks hooks;
    hooks.picture = [](CpuContext&, Picture&) {};
    hooks.present = [] { return true; };
    Movie incomplete(r.memory, *r.decoder, hooks, {[](Movie::ImageLease) { return true; }, {}});
    CHECK(incomplete.Install(r.Capture(), r.Installer(), true) == 0);
    hooks.record = [](const Graphics::Anchor&, const Draw&, const Graphics::Handle&) { return true; };
    Movie ambiguous(r.memory, *r.decoder, hooks, {[](Movie::ImageLease) { return true; },
        [](const Draw&, Movie::ImageLease) { return true; }});
    CHECK(ambiguous.Install(r.Capture(), r.Installer(), true) == 0);
}

void TestDeferredReopen() {
    ++cases;
    auto rig = std::make_unique<Rig>(true, true);
    auto& r = *rig;
    r.Start(512, 288);
    r.BeginFrame(71);
    r.Decode(); r.Enqueue(); r.Queue();
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(false, true);
    const auto first = r.memory.Read32(kController + 0x44);
    CHECK(r.backend.uploads == 0 && r.deferred.size() == 2);
    r.Call(Decoder::kClose, r.context);
    CHECK(r.movie->Selected() && !r.movie->Active());

    // The first image/draw remains unconsumed across a differently sized new
    // movie and across complete guest state invalidation. Delivery must use
    // only the accepted immutable inputs.
    r.Start(640, 480);
    r.BeginFrame(72);
    r.Decode(); r.Enqueue(); r.Queue();
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(false, false);
    CHECK(r.memory.Read32(kController + 0x44) != first && r.deferred.size() == 4);
    r.Call(Decoder::kClose, r.context);
    CHECK(r.movie->PendingPictures() == 0);
    std::memset(r.memory.ram.Data(), 0xcc, r.memory.ram.Size());
    r.width = r.height = r.source_frame = 999; // mutable producer state is no input to delivery
    r.Submit();
    CHECK(r.order == std::vector<char>({'U','V','D','U','V','D'}));
    CHECK(r.uploaded.size() == 2 && r.draws.size() == 2);
    CHECK(r.uploaded[0].size() == size_t(512) * 288 * 4 &&
          r.uploaded[1].size() == size_t(640) * 480 * 4);
    CHECK(r.draws[0].src_frame == 71 && r.draws[1].src_frame == 72);
    CHECK(r.draws[0].bbox[1] == 80 && r.draws[0].bbox[4] == 368);
    CHECK(r.residents.empty() && r.resources.Counts().live_surfaces == 0);
}

void CheckSampledView(const Picture& view, const Bytes& source, uint32_t source_width,
                      uint32_t content_width, uint32_t content_height) {
    CHECK(view.width > int(content_width) && view.height > int(content_height));
    CHECK(!(view.width & 15) && !(view.height & 15));
    // Exact actual upload bytes: preserve the sampled source rectangle, then
    // repeat its final column/row. In particular, excluded crop/strip texels
    // must not survive as the first right/bottom tap (R197's +1 regression).
    for (int y = 0; y < view.height; ++y) {
        const auto* expected = source.data() +
            size_t(std::min(uint32_t(y), content_height - 1)) * source_width * 4;
        const auto* row = view.rgba.data() + size_t(y) * view.width * 4;
        CHECK(std::memcmp(row, expected, size_t(content_width) * 4) == 0);
        for (uint32_t x = content_width; x < uint32_t(view.width); ++x)
            CHECK(std::memcmp(row + size_t(x) * 4, expected + size_t(content_width - 1) * 4, 4) == 0);
    }
    // OLD's last two filter taps must agree; a nearest/full-image clamp alone
    // does not establish this for cropped/strip-rounded content.
    const auto* edge = view.rgba.data() +
        (size_t(content_height - 1) * view.width + content_width - 1) * 4;
    CHECK(std::memcmp(edge, edge + 4, 4) == 0);
    CHECK(std::memcmp(edge, edge + size_t(view.width) * 4, 4) == 0);
}

void TestMovieViewChanges(bool services) {
    ++cases;
    auto rig = std::make_unique<Rig>(true, services);
    auto& r = *rig;
    r.Start(640, 480); r.BeginFrame(91);
    r.Decode(); r.Enqueue(); r.Queue();
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(false, false);
    r.DrawFrame(false, false); // identical input reuses one immutable view
    CHECK(r.movie->Counts().views == 1);
    r.DrawFrame(true, false);  // 12 strips * floor(640/12) = 636, not 640
    CHECK(r.movie->Counts().views == 2);
    r.DrawFrame(true, false, 512, 400); // bottom crop now ends at row 440
    CHECK(r.movie->Counts().views == 3);
    r.Call(Decoder::kClose, r.context);
    std::memset(r.memory.ram.Data(), 0xcc, r.memory.ram.Size());
    r.Submit();
    CHECK(r.uploaded.size() == 1 && r.views.size() == 3 && r.draws.size() == 4);
    CHECK(r.order == std::vector<char>({'U','V','D','D','V','D','V','D'}));
    CheckSampledView(r.views[0], r.uploaded[0], 640, 640, 464);
    CheckSampledView(r.views[1], r.uploaded[0], 640, 636, 464);
    CheckSampledView(r.views[2], r.uploaded[0], 640, 636, 440);
    CHECK(r.resources.CollectAfterQuiescence() == Resources::Result::Ok);
    CHECK(r.resources.Counts().live_surfaces == 0);
}

void TestMovieViewPolicy() {
    ++cases;
    auto picture = Rig::Pixels(640, 480, 3);
    const auto unchanged = picture.rgba;
    Draw draw;
    CHECK(Movie::BuildDraw({512,448,640,480,true,false}, draw));
    Picture view;
    CHECK(Movie::BuildImageView(draw, picture, view));
    CheckSampledView(view, unchanged, 640, 636, 464);
    CHECK(picture.rgba == unchanged); // the raw decoder image is never patched
    // Negative controls: full-picture clamping and preserving the first
    // excluded texel produce different edge colors with these source inputs.
    const auto raw = [&](unsigned x, unsigned y) { return unchanged.data() + (size_t(y) * 640 + x) * 4; };
    CHECK(std::memcmp(raw(635,100), raw(636,100), 4) != 0);
    CHECK(std::memcmp(raw(100,463), raw(100,464), 4) != 0);
    CHECK(std::memcmp(raw(635,463), raw(639,479), 4) != 0);
    const auto saved = view.rgba;
    for (unsigned invalid = 0; invalid != 5; ++invalid) {
        auto other = draw;
        if (invalid == 0) other.use_fst = false;
        if (invalid == 1) other.ctx = 0; // never turn other image families into movie views
        if (invalid == 2) other.tex.psm = 19;
        if (invalid == 3) other.tex.wms = 1;
        if (invalid == 4) other.verts.back().uv_u = 12000; // outside this picture
        CHECK(!Movie::BuildImageView(other, picture, view));
        CHECK(view.rgba == saved);
    }
    picture.rgba.pop_back();
    CHECK(!Movie::BuildImageView(draw, picture, view));
    CHECK(view.rgba == saved);
}

std::array<uint64_t, 6> SceneFields(const Frame::SceneFrame& scene) {
    return {scene.color_buffer, scene.depth_buffer, scene.width, scene.height,
            scene.source_frame, scene.epoch};
}

void TestMovieSceneCapture() {
    ++cases;
    auto rig = std::make_unique<Rig>(true, true, true);
    auto& r = *rig;
    r.Start(640, 480);
    r.BeginFrame(777); // deliberately not the source Link ordinal
    r.Decode(); r.Enqueue(); r.Queue();
    CHECK(r.deferred.size() == 1 && r.scene_captures == 0 && r.scene_reads == 0);
    // The image-only QueuePicture slot is valid before any scene snapshot.
    r.SetUpScene();
    r.prohibit_scene_reads = true;
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(false, false);
    CHECK(r.deferred.size() == 2);
    r.SceneCall(Frame::kLinkAddress, Frame::kFrameBuckets);
    Frame::SceneFrame unavailable{1,2,3,4,5,6};
    CHECK(!r.scene_source->CaptureScene(Movie::kBucket, unavailable));
    CHECK(SceneFields(unavailable) == (std::array<uint64_t,6>{1,2,3,4,5,6}));
    r.Call(Decoder::kClose, r.context);

    r.Start(512, 288);
    r.BeginFrame(888);
    r.Decode(); r.Enqueue(); r.Queue(); // another upload after invalidating Link
    CHECK(r.deferred.size() == 3 && r.scene_captures == 1);
    r.prohibit_scene_reads = false;
    r.SetUpScene(512, 512);
    r.prohibit_scene_reads = true;
    r.memory.Write32(kController + 0x1e4, 2);
    r.DrawFrame(false, true, 512, 512);
    r.Call(Decoder::kClose, r.context);
    Frame::SceneFrame latest;
    CHECK(r.scene_source->CaptureScene(Movie::kBucket, latest));
    CHECK(latest.source_frame == 1 && latest.epoch == 2); // a late lookup would redirect the first draw
    r.SceneCall(Frame::kLinkAddress, Frame::kFrameBuckets);
    const auto reads = r.scene_reads;
    std::memset(r.memory.ram.Data(), 0xcc, r.memory.ram.Size());
    r.source_frame = 999;
    r.Submit();
    CHECK(r.scene_reads == reads && r.scene_captures == 2);
    CHECK(r.delivered_scenes.size() == 2 && r.draws.size() == 2);
    CHECK(SceneFields(r.delivered_scenes[0]) ==
          (std::array<uint64_t,6>{0x70000,0x150000,512,448,0,1}));
    CHECK(SceneFields(r.delivered_scenes[1]) ==
          (std::array<uint64_t,6>{0x80000,0x180000,512,512,1,2}));
    CHECK(r.draws[0].src_frame == 0 && r.draws[1].src_frame == 1);
    CHECK(r.resources.Counts().live_surfaces == 0);
    CHECK(r.gif_starts == 0 && r.gs_sink.ImageQwords() == 0);
    CHECK((r.movie->Counts().original_calls == std::array<uint64_t,3>{}));

    // Root drains accepted groups before restore/reset. Source setup epoch
    // stays monotonic while the primary Link ordinal restarts at zero.
    CHECK(r.deferred.empty() && !r.movie->Active());
    r.scene_source->ResetAfterQuiescence();
    CHECK(!r.scene_source->CaptureScene(Movie::kBucket, unavailable));
    r.prohibit_scene_reads = false;
    r.SetUpScene();
    CHECK(r.scene_source->CaptureScene(Movie::kBucket, latest));
    CHECK(latest.source_frame == 0 && latest.epoch > 2);
}

void TestMovieMissingScene() {
    for (bool mismatched_width : {false, true}) {
        ++cases;
        auto rig = std::make_unique<Rig>(true, true, true);
        auto& r = *rig;
        r.Start(512, 288); r.Decode(); r.Enqueue(); r.Queue();
        if (mismatched_width) r.SetUpScene(640, 448); // valid source tuple, incompatible Movie FBW
        else {
            r.SceneCall(Frame::kInitDisplayAddress);
            r.SceneCall(Frame::kWriteFrameHeadAddress);
        }
        r.prohibit_scene_reads = true;
        // Priming does not demand a draw snapshot.
        r.memory.Write32(kController + 0x1e4, 0);
        r.DrawFrame(false, true);
        CHECK(r.scene_captures == 0 && r.deferred.size() == 1);
        r.memory.Write32(kController + 0x1e4, 2);
        try { r.DrawFrame(false, true); CHECK(false); }
        catch (const std::runtime_error&) {}
        CHECK(r.scene_captures == 1 && r.movie->Counts().errors == 1);
        CHECK(r.deferred.size() == 1); // no accepted draw, only the earlier raw upload
        CHECK((r.movie->Counts().original_calls == std::array<uint64_t,3>{}));
        r.Call(Decoder::kClose, r.context);
        r.Submit();
        CHECK(r.uploaded.size() == 1 && r.views.empty() && r.draws.empty());
        CHECK(r.delivered_scenes.empty() && r.resources.Counts().live_surfaces == 0);
        CHECK(r.gif_starts == 0 && r.gs_sink.ImageQwords() == 0);
    }
}

struct Trace {
    Bytes input, picture, draw;
    uint64_t image_qwords = 0, gif_starts = 0, uploads = 0, draws = 0;
    std::map<uint32_t, uint64_t> generated;
    void Record(const Output& output) {
        Word(input, output.input.size());
        Append(input, output.input.data(), output.input.size());
        for (const auto& pixels : output.pixels) {
            Word(picture, pixels.size());
            Append(picture, pixels.data(), pixels.size());
        }
        for (const auto& raw : output.draws) {
            const auto canonical = CanonicalDraw(raw);
            const auto record = canonical.ToDrawRec();
            Append(draw, &record, sizeof(record));
            Word(draw, canonical.raster.dimx); Word(draw, canonical.raster.dthe);
            Word(draw, canonical.tex.mxl); Word(draw, canonical.tex.lcm);
            Word(draw, canonical.tex.l); Word(draw, uint16_t(canonical.tex.k));
            for (unsigned i = 0; i < 6; ++i) {
                Word(draw, canonical.tex.mip_tbp[i]); Word(draw, canonical.tex.mip_tbw[i]);
            }
            Word(draw, canonical.verts.size());
            Append(draw, canonical.verts.data(), canonical.verts.size() * sizeof(canonical.verts[0]));
        }
        image_qwords += output.image_qwords; gif_starts += output.gif_starts;
        uploads += output.completed.uploads; draws += output.completed.draws;
        for (const auto& entry : output.calls) generated[entry.first] += entry.second;
    }
};
std::string Digest(const Bytes& bytes) {
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    CHECK(sha && av_sha_init(sha.get(), 256) == 0);
    av_sha_update(sha.get(), bytes.data(), bytes.size());
    std::array<uint8_t, 32> result{};
    av_sha_final(sha.get(), result.data());
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t b : result) out << std::setw(2) << unsigned(b);
    return out.str();
}
std::string Quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char b : value) {
        if (b == '"' || b == '\\') out << '\\' << b;
        else if (b < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned(b);
        else out << b;
    }
    out << '"'; return out.str();
}
Bytes Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    CHECK(file.good() && file.tellg() > 0);
    Bytes bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    CHECK(file.good()); return bytes;
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    CHECK(!std::filesystem::exists(path));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close(); CHECK(file.good() && std::filesystem::is_regular_file(path));
}
void Save(const std::filesystem::path& path, const std::string& text) {
    Save(path, Bytes(text.begin(), text.end()));
}
void Export(const Trace& model, const Trace& selected, int argc, char** argv,
            const movie_oracle::ExportArgs& args) {
    if (args.directory.empty()) return;
    const auto binary = std::filesystem::canonical(argv[0]);
    const auto directory = std::filesystem::absolute(args.directory);
    const std::string binary_sha = Digest(Load(binary)),
                      source_sha = Digest(Load(args.source_manifest));
    CHECK(!std::filesystem::exists(directory) && std::filesystem::create_directories(directory));
    std::ostringstream command; command << '[' << Quote(binary.string());
    for (int i = 1; i < argc; ++i) command << ',' << Quote(argv[i]);
    command << ']';
    std::ostringstream index;
    index << "{\"schema\":\"movie-graphics-artifacts-v1\",\"test\":\"MovieGraphicsSelfOracle\","
          << "\"scope\":\"owned RGB32 upload bytes and canonical active movie draw; no rasterizer/audio/presentation claim\","
          << "\"canonicalization\":\"explicit owned resource identity; legacy transfer seq/epoch; disabled ALPHA only\","
          << "\"attributions\":{},\"arms\":{";
    for (bool native : {false, true}) {
        const auto& trace = native ? selected : model;
        const std::string mode = native ? "native" : "model"; // the artifact schema's spelling
        const auto arm = directory / mode;
        CHECK(std::filesystem::create_directory(arm));
        std::map<std::string, std::string> hashes;
        for (const auto& lane : std::array<std::pair<const char*, const Bytes*>, 3>{{
                {"input", &trace.input}, {"picture", &trace.picture}, {"draw", &trace.draw}}}) {
            Save(arm / (std::string(lane.first) + ".bin"), *lane.second);
            hashes[lane.first] = Digest(*lane.second);
        }
        std::ostringstream receipt;
        receipt << "{\"schema\":\"retirement-oracle-run-v1\",\"mode\":" << Quote(mode)
            << ",\"test\":\"MovieGraphicsSelfOracle\",\"command\":" << command.str()
            << ",\"binary_sha256\":" << Quote(binary_sha) << ",\"source_sha256\":" << Quote(source_sha)
            << ",\"input_sha256\":" << Quote(hashes.at("input"))
            << ",\"exit_code\":0,\"completed\":true,\"settings\":{\"hle.movie_graphics\":"
            << Quote(native ? "on" : "off") << ",\"hle.movie_lifecycle\":\"on\","
            << "\"input\":\"owned-rgba-source-fixture\",\"consumer\":\"GraphicsResources/FakeBackend\","
            << "\"output_adapter\":\"deferred-owned-services\"},"
            << "\"outputs\":{\"picture\":" << Quote(hashes.at("picture")) << ",\"draw\":" << Quote(hashes.at("draw"))
            << "},\"counters\":{\"gif\":" << Quote("image_qwords=" + std::to_string(trace.image_qwords))
            << ",\"movie_transport\":" << Quote("gif_starts=" + std::to_string(trace.gif_starts))
            << ",\"graphics\":" << Quote("movie_uploads=" + std::to_string(trace.uploads) +
                                       ",movie_draws=" + std::to_string(trace.draws))
            << "},\"counter_provenance\":{\"gif.image_qwords\":\"engine/gif/hle/gs_state_sink.cpp:62\","
            << "\"graphics\":\"Graphics::Submit completed dispatch counters\"},\"generated_entries\":{";
        bool first = true;
        for (const auto& entry : trace.generated) {
            std::ostringstream name; name << "0x" << std::hex << entry.first;
            receipt << (first ? "" : ",") << Quote(name.str()) << ':' << entry.second; first = false;
        }
        receipt << "}}\n";
        Save(arm / "receipt.json", receipt.str());
        const auto ref = [&](const std::string& file, const std::string& hash) {
            return "{\"path\":" + Quote(mode + "/" + file) + ",\"sha256\":" + Quote(hash) + "}";
        };
        index << (native ? "," : "") << Quote(mode) << ":{\"input\":" << ref("input.bin", hashes.at("input"))
            << ",\"receipt\":" << ref("receipt.json", Digest(Load(arm / "receipt.json")))
            << ",\"outputs\":{\"picture\":" << ref("picture.bin", hashes.at("picture"))
            << ",\"draw\":" << ref("draw.bin", hashes.at("draw")) << "}}";
    }
    index << "}}\n"; Save(directory / "artifacts.json", index.str());
}
}  // namespace

namespace movie_oracle {
std::ostringstream g_logFile;
void Read(uint32_t address, void* data, size_t bytes) {
    CHECK(current);
    if (MainMemory::Phys(address) >= 0x10000000) {
        CHECK(bytes <= 8);
        const uint64_t value = current->dmac.Read(address, unsigned(bytes));
        std::memcpy(data, &value, bytes); return;
    }
    auto* p = current->memory.Translate(address, bytes); CHECK(p);
    std::memcpy(data, p, bytes);
}
void Write(uint32_t address, const void* data, size_t bytes) {
    CHECK(current);
    if (MainMemory::Phys(address) >= 0x10000000) {
        CHECK(bytes <= 8);
        uint64_t value = 0; std::memcpy(&value, data, bytes);
        current->dmac.Write(address, unsigned(bytes), value);
        if (address == 0x1000a000 && (value & 0x100)) ++current->gif_starts;
        return;
    }
    auto* p = current->memory.Translate(address, bytes); CHECK(p);
    std::memcpy(p, data, bytes);
}
void Dispatch(uint32_t address, CpuContext& context) { current->Call(address, context); }
void ObserveGenerated(uint32_t address) { ++current->originals[address]; }
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) {
    CHECK(false); return nullptr;
}
}  // namespace movie_oracle

int main(int argc, char** argv) {
    movie_oracle::ExportArgs args;
    if (argc < 2 || std::string(argv[1]) != "--both-paths" ||
        !movie_oracle::ExportArgs::Parse(argc, argv, 2, args)) {
        std::fprintf(stderr, "usage: test_movie_graphics_oracle --both-paths "
                             "[--export-dir DIR --source-manifest FILE]\n");
        return 2;
    }
    GuestPark::SetSink([](const ParkRecord& record) {
        std::fprintf(stderr, "Movie rejected intent: %s at %x\n", record.reason, record.pc);
        throw std::runtime_error("movie park");
    });
    Trace retained, replacement, fixed_queue;
    for (const auto& dims : {std::pair{512u,288u}, {512u,448u}, {640u,480u}}) {
        for (bool tv : {false, true}) for (bool wide : {false, true}) {
            ++cases;
            const auto model = Run(false, dims.first, dims.second, tv, wide, true);
            const auto fixed = Run(true, dims.first, dims.second, tv, wide);
            const auto selected = Run(true, dims.first, dims.second, tv, wide, true);
            CHECK(model.input == selected.input);
            retained.Record(model); replacement.Record(selected); fixed_queue.Record(fixed);
            CHECK(model.gif_starts == 3 && selected.gif_starts == 0);
            CHECK(model.image_qwords == uint64_t(dims.first) * dims.second * 3 / 4);
            CHECK(selected.image_qwords == 0);
            if (model.pixels != selected.pixels) {
                for (size_t frame = 0; frame < model.pixels.size(); ++frame) {
                    const auto& a = model.pixels[frame]; const auto& b = selected.pixels[frame];
                    const auto first = std::mismatch(a.begin(), a.end(), b.begin());
                    if (first.first != a.end()) std::fprintf(stderr,
                        "picture %zu first byte %zu model=%02x selected=%02x\n", frame,
                        size_t(first.first - a.begin()), *first.first, *first.second);
                }
                CHECK(false);
            }
            CHECK(model.calls.at(Movie::kDecodeAndUpload) == 3);
            CHECK(model.calls.at(Movie::kQueuePicture) == 3);
            CHECK(model.calls.at(Movie::kDraw) == 4);
            CHECK(selected.movie.decoded == 3 && selected.movie.uploaded == 3 && selected.movie.draws == 3);
            for (size_t i = 0; i < model.draws.size(); ++i) Compare(model.draws[i], selected.draws[i]);
            CHECK(selected.views.size() == 3 && fixed.views.size() == 3);
            // Source mode pins, independent of the production UV scanner.
            const uint32_t cw = tv && !wide ? (dims.first == 512 ? 504 : 636) : dims.first;
            const uint32_t ch = dims.second == 480 ? 464 : dims.second;
            for (size_t i = 0; i < selected.views.size(); ++i) {
                CheckSampledView(selected.views[i], model.pixels[i], dims.first, cw, ch);
                CHECK(fixed.views[i].width == selected.views[i].width &&
                      fixed.views[i].height == selected.views[i].height &&
                      fixed.views[i].rgba == selected.views[i].rgba);
            }
            if (!tv && wide && dims.first == 512 && dims.second == 288) {
                CHECK(selected.draws[0].bbox[1] == 80 && selected.draws[0].bbox[4] == 368);
            }
        }
    }
    TestOwnershipErrors();
    TestOwnershipErrors(true);
    TestClosedPictureCalls();
    TestDeferredReopen();
    TestMovieViewChanges(false);
    TestMovieViewChanges(true);
    TestMovieViewPolicy();
    TestMovieSceneCapture();
    TestMovieMissingScene();
    CHECK(retained.input == replacement.input && retained.picture == replacement.picture &&
          retained.draw == replacement.draw);
    CHECK(fixed_queue.input == replacement.input && fixed_queue.picture == replacement.picture &&
          fixed_queue.draw == replacement.draw);
    CHECK(retained.image_qwords > 0 && replacement.image_qwords == 0);
    CHECK(replacement.uploads == 72 && replacement.draws == 36);
    Export(retained, replacement, argc, argv, args);
    GuestPark::SetSink({});
    std::printf("MovieGraphicsSelfOracle: %zu checks, %zu cases; actual GIF starts/IMAGE positive control; raw RGBA/draw exact; movie edges and immutable source frames pinned\n",
                checks, cases);
    return 0;
}
