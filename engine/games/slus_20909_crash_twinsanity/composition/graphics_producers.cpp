#include "games/slus_20909_crash_twinsanity/composition/graphics_producers.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"

namespace ps2r::app {
namespace {
using Bindings = ::title::TextureBindings;
using Material = ::title::PrimitiveMaterial;
using Sprite = ::title::SpriteService;
using Text = ::title::TextService;
using Sky = ::title::SkyPass;
using Family = ::title::GraphicsRefusals::Family;
using Graphics = ::title::Graphics;
constexpr uint32_t kFlush = 0x1ba3a8, kForget = 0x1c0410, kLink = 0x17ed50;
constexpr uint32_t kMaterialCount = 0x30a810, kMaterials = 0x3d5cf0;
bool SameFrame(const GraphicsProducers::Frame& a, const GraphicsProducers::Frame& b) {
    return a.color_buffer == b.color_buffer && a.depth_buffer == b.depth_buffer &&
        a.width == b.width && a.height == b.height &&
        a.source_frame == b.source_frame && a.epoch == b.epoch;
}
bool UiDraw(const gs::DrawIr& draw, const GraphicsProducers::Frame& frame) {
    return !draw.ctx && !draw.fog && !draw.scanmsk && !draw.raster.pabe &&
        !draw.raster.dthe && draw.raster.colclamp && draw.raster.frame.psm == 0 &&
        draw.raster.zbuf.psm == 0 && draw.raster.frame.fbw == frame.width &&
        draw.raster.frame.fbp == frame.color_buffer && draw.raster.zbuf.zbp == frame.depth_buffer;
}
GraphicsProducers::Output Connect(GraphicsOutput& output) {
    GraphicsProducers::Output hooks;
    hooks.ui = [&output](uint32_t bucket, auto& frame, auto& environment) {
        return output.CaptureUi(bucket, frame, environment);
    };
    hooks.sky = [&output](auto& frame, auto& environment) { return output.CaptureSky(frame, environment); };
    hooks.movie = [&output](auto& frame) { return output.CaptureScene(27, frame); };
    hooks.mesh = [&output](uint32_t bucket, auto& frame, auto& environment) {
        return output.CaptureMesh(bucket, frame, environment);
    };
    hooks.record = [&output](uint32_t bucket, std::function<bool()> work) {
        return output.RecordOwned(bucket, [work = std::move(work)](VideoStack&) { return work(); });
    };
    hooks.immediate = [&output](std::function<bool()> work) {
        return output.EnqueueOwned([work = std::move(work)](VideoStack&) { return work(); });
    };
    hooks.settled = [&output](uint64_t pending) {
        return output.SettleOwnedForCapture(nullptr) && output.UnlinkedAtBoundary().owned_items >= pending;
    };
    return hooks;
}
}  // namespace

struct GraphicsProducers::Impl {
    struct Ticket {
        std::atomic<bool> entered{false};
        std::atomic<uint64_t>& pending;
        std::atomic<uint64_t>& cancelled;
        Ticket(std::atomic<uint64_t>& p, std::atomic<uint64_t>& c) : pending(p), cancelled(c) {}
        ~Ticket() {
            if (!entered.exchange(true)) { --pending; ++cancelled; }
        }
    };
    struct TextMaterial {
        Frame frame;
        std::vector<GraphicsProducerDraws::AssetDraw> draws;
        ::title::TextIntent::Page page;
        bool flushed = false;
    };
    MainMemory& memory;
    Window scratchpad, arena;
    Bindings& bindings;
    ::title::TextureSelection& selection;
    Options options;
    Output output;
    std::unique_ptr<GraphicsProducerDraws> owned_draws;
    GraphicsProducerDraws* draws = nullptr;
    std::unique_ptr<Sprite> sprites;
    std::unique_ptr<Text> text;
    std::unique_ptr<Sky> sky;
    std::unique_ptr<MeshProducer> meshes;
    std::map<uint32_t, TextMaterial> materials;
    std::array<Body, 3> originals;
    Frame sprite_frame, text_frame, sky_frame;
    Environment text_environment;
    bool installed = false, faulted = false, sky_open = false;
    uint64_t flush_calls = 0, flush_replaced = 0, flush_model_calls = 0, refused_recordings = 0;
    std::atomic<uint64_t> recorded{0}, completed{0}, pending{0}, failures{0}, cancelled{0}, refused_slots{0};

    Impl(MainMemory& ram, Bindings& texture_bindings, ::title::TextureSelection& policy,
         Options selected, Output services, Window stack, Window extra)
        : memory(ram), scratchpad(stack), arena(extra), bindings(texture_bindings), selection(policy),
          options(selected), output(std::move(services)) {}
    // Main RAM in any KUSEG/KSEG0/KSEG1 alias, the root's extra physical
    // region above it (the META arena window, kArenaBase), or the scratchpad
    // window exactly (0x70000000..0x70003fff; no mirror above it is mapped).
    uint8_t* Bytes(uint32_t address, size_t size) const {
        if ((address & ~Scratchpad::kOffsetMask) == Scratchpad::kBase) {
            const uint32_t offset = Scratchpad::Offset(address);
            if (!scratchpad.data || offset >= scratchpad.size || offset >= Scratchpad::kSize ||
                size > scratchpad.size - offset || size > Scratchpad::kSize - offset) return nullptr;
            return scratchpad.data + offset;
        }
        const uint32_t segment = address >> 29;
        if (!address || (segment != 0 && segment != 4 && segment != 5)) return nullptr;
        const uint32_t physical = address & 0x1fffffff;
        if (physical >= kArenaBase) {
            const uint32_t offset = physical - kArenaBase;
            if (!arena.data || offset >= arena.size || size > arena.size - offset) return nullptr;
            return arena.data + offset;
        }
        return memory.Pointer(physical, size);
    }
    bool Read(uint32_t address, void* out, size_t size) const {
        const auto* bytes = Bytes(address, size);
        if (!bytes || !out) return false;
        std::memcpy(out, bytes, size);
        return true;
    }
    template<class T> bool Value(uint32_t address, T& value) const {
        return Read(address, &value, sizeof(value));
    }
    [[noreturn]] void Fail(const char* why) {
        faulted = true;
        ++failures;
        throw std::runtime_error(std::string("Graphics producers: ") + why);
    }
    // The ordered-group lifecycle at delivery (GraphicsOutput): the callback's
    // false is a REFUSAL at the slot -- the owned scene declined before drawing
    // (its target not ready, a stale/missing scene), nothing was published,
    // the slot is released and the refusal is attributed to the family
    // (ordering, detail kSlotRefused) -- never a failure of the run. A throw is
    // the fault (the renderer ran and failed): the ordered output faults its
    // walk and this owner refuses every later source entry.
    static constexpr uint32_t kSlotRefused = 0x200;
    // The work receives a detail word to name its refusal (GraphicsProducerDraws'
    // kBindRefused/kUploadRefused/kDrawRefused|result); it is ORed into the
    // family's kSlotRefused detail so the ledger says WHICH slot check declined.
    using Work = std::function<bool(uint32_t& why)>;
    bool Queue(uint32_t bucket, Work work, bool immediate = false,
               Family family = Family::Count, uint32_t producer = 0) {
        if (faulted || failures.load() || !work) Fail("selected output is faulted");
        // Account before the callback: an immediate source slot may run inline.
        ++pending;
        auto entered = std::make_shared<Ticket>(pending, cancelled);
        auto call = [this, work = std::move(work), entered, family, producer] {
            if (entered->entered.exchange(true)) return false; // never replay a selected callback
            bool ok = false;
            uint32_t why = 0;
            try { if (!failures.load()) ok = work(why); }
            catch (...) { ++failures; --pending; throw; }
            if (ok) ++completed;
            else {
                ++refused_slots;
                if (family != Family::Count && output.refusals)
                    output.refusals->Refuse(family, ::title::GraphicsRefusals::Reason::Ordering, producer,
                                            kSlotRefused | (why & 0xff) | (why & 0xff00) << 4);
            }
            --pending;
            return ok;
        };
        bool accepted = false;
        try {
            accepted = immediate ? output.immediate(std::move(call)) : output.record(bucket, std::move(call));
        } catch (...) {
            if (!entered->entered.exchange(true)) --pending;
            Fail("ordered recording threw");
        }
        if (!accepted) {
            // The ordered output refused the reservation (no predecessor,
            // stopped, no video): nothing was recorded, so the caller may
            // refuse to its original. A transport that retained the closure
            // until its fence sees a cancelled ticket: a late invocation
            // returns false before touching this owner or replaying the draw.
            if (!entered->entered.exchange(true)) --pending;
            ++refused_recordings;
            return false;
        }
        ++recorded;
        return true;
    }
    bool RecordSprite(const ::title::SpriteIntent::Output& intent) {
        if (intent.bucket != 24 || !UiDraw(intent.draw, sprite_frame)) return false;
        if (intent.draw.verts.empty()) return true; // validated source no-op, no GPU completion credit
        std::vector<GraphicsProducerDraws::AssetDraw> batch{{intent.draw, intent.sampling.asset}};
        return Queue(24, [this, frame = sprite_frame, batch = std::move(batch)](uint32_t& why) {
            return draws->Assets(frame, batch, &why);
        }, false, Family::Sprites, ::title::SpriteIntent::kPlain);
    }
    // Software bookkeeping writes of an owned pass (the sky's matrix/material
    // words, the mesh pass's material list words): all or none, never a packet.
    template<class Write> bool WriteGuest(const std::vector<Write>& writes) {
        for (const auto& write : writes) if (!Bytes(write.address, write.bytes.size())) return false;
        for (const auto& write : writes)
            std::memcpy(Bytes(write.address, write.bytes.size()), write.bytes.data(), write.bytes.size());
        return true;
    }
    bool WriteSky(const std::vector<Sky::Write>& writes) { return WriteGuest(writes); }
    bool RecordText(const ::title::TextIntent::Output& intent) {
        if (faulted || failures.load()) Fail("text after failed output");
        uint32_t count = 0;
        if (!Value(kMaterialCount, count) || count > 500) return false;
        std::set<uint32_t> fresh;
        // Build the entire host update before publishing any guest list word.
        auto next = materials;
        for (const auto& page : intent.pages) {
            const uint32_t material = page.material & 0x1fffffff;
            uint32_t writer = 0;
            uint8_t listed = 0, direct = 0;
            if (page.bucket != 26 || !Value(page.material + 0x28, writer) || writer ||
                !Value(page.material + 0x64, listed) || !Value(page.material + 0x66, direct) || direct)
                return false;
            for (const auto& draw : page.draws) if (!UiDraw(draw, text_frame)) return false;
            auto found = next.find(material);
            if (found == next.end()) {
                if (listed) return false; // a retained writer already owns this material
                fresh.insert(material);
                found = next.emplace(material, TextMaterial{text_frame, {}, page, false}).first;
                found->second.page.draws.clear();
            }
            auto& group = found->second;
            if (group.flushed || !SameFrame(group.frame, text_frame) ||
                group.page.shader != page.shader ||
                group.page.sampling.asset.lease != page.sampling.asset.lease ||
                group.draws.size() + page.draws.size() > 65536) return false;
            for (const auto& draw : page.draws) group.draws.push_back({draw, page.sampling.asset});
        }
        if (fresh.size() > 500 - count || !Bytes(kMaterials + count * 4, fresh.size() * 4))
            return false;
        // Follow the source's first-touch page order, never map/pointer order.
        for (const auto& page : intent.pages) {
            const auto material = page.material & 0x1fffffff;
            if (!fresh.erase(material)) continue;
            std::memcpy(Bytes(kMaterials + count++ * 4, 4), &page.material, 4);
            *Bytes(page.material + 0x64, 1) = 1;
        }
        std::memcpy(Bytes(kMaterialCount, 4), &count, 4);
        materials.swap(next);
        return true;
    }
    void Flush(uint32_t material) {
        auto& group = materials.at(material & 0x1fffffff);
        Frame frame;
        Environment environment;
        Material::Output now;
        uint32_t writer = 0;
        if (group.flushed || !output.ui(26, frame, environment) ||
            !SameFrame(frame, group.frame) || !Value(material + 0x28, writer) || writer ||
            Material::Capture([this](auto at, auto* out, auto size) { return Read(at, out, size); },
                bindings, material, environment, now, Material::Kind::Text) != Material::Result::Ok ||
            now.bucket != 26 || now.shader != group.page.shader ||
            now.sampling.asset.lease != group.page.sampling.asset.lease)
            Fail("text material/target changed before source flush");
        // Source FlushMaterial supplies the state. A captured batch whose
        // source target/material changed cannot be silently reinterpreted.
        for (const auto& item : group.draws) {
            auto before = item.draw, after = now.draw;
            before.verts.clear(); after.verts.clear();
            before.vertex_count = after.vertex_count = 0;
            before.snapshot = after.snapshot = {};
            std::fill(std::begin(before.bbox), std::end(before.bbox), 0);
            std::fill(std::begin(after.bbox), std::end(after.bbox), 0);
            before.src_frame = after.src_frame = 0;
            const auto a = before.ToDrawRec(), b = after.ToDrawRec();
            if (std::memcmp(&a, &b, sizeof(a)) ||
                before.raster.dimx != after.raster.dimx || before.raster.dthe != after.raster.dthe ||
                before.tex.mxl != after.tex.mxl || before.tex.k != after.tex.k ||
                before.tex.l != after.tex.l || before.tex.lcm != after.tex.lcm)
                Fail("text inherited state changed before source flush");
        }
        // The material list words were already published at RecordText: a
        // refused reservation here is a fault, never a model flush.
        if (!Queue(26, [this, frame = group.frame, batch = group.draws](uint32_t& why) {
                       return draws->Assets(frame, batch, &why);
                   }, false, Family::Text, kFlush))
            Fail("text flush recording refused after the material list was published");
        group.flushed = true;
        ++flush_replaced;
    }
    void Dispatch(size_t index, CpuContext& context, uint32_t entry) {
        constexpr std::array<uint32_t, 3> entries{kFlush, kForget, kLink};
        if (faulted || failures.load()) Fail("source entry after failed output");
        const bool full = !entry || entry == entries[index];
        const auto argument = context.cpuRegs.GPR.r[4].UL[0];
        if (!full && (!materials.empty() || sky_open)) Fail("interior with selected source work");
        if (index == 0) {
            ++flush_calls;
            if (full && materials.count(argument & 0x1fffffff)) { Flush(argument); return; }
            ++flush_model_calls;
        } else if (full && (index == 1 ||
                   (argument & 0x1fffffff) == ::title::FrameEnvironment::kFrameBuckets)) {
            for (const auto& item : materials)
                if (!item.second.flushed) Fail("source frame ended before selected text flush");
            if (index == 1) materials.clear();
            if (index == 2 && sky_open) Fail("source frame ended inside selected sky");
        }
        originals[index](context, entry);
    }
    void BuildServices() {

        const auto refuser = [this](Family family) -> ::title::GraphicsRefusals::Sink {
            if (!output.refusals) return {};
            return output.refusals->Refuser(family);
        };
        const auto faulter = [this](Family family) -> ::title::GraphicsRefusals::Sink {
            if (!output.refusals) return {};
            return output.refusals->Faulter(family);
        };
        Sprite::Hooks sprite;
        sprite.enabled = options.sprites;
        sprite.refuse = refuser(Family::Sprites);
        sprite.fault = faulter(Family::Sprites);
        sprite.read = [this](auto at, auto* out, auto size) { return Read(at, out, size); };
        sprite.environment = [this](Environment& out) {
            return !sky_open && output.ui(24, sprite_frame, out);
        };
        sprite.selected = [this](uint32_t object) { return selection.Sprite(object); };
        sprite.record = [this](const auto& intent) { return RecordSprite(intent); };
        sprites = std::make_unique<Sprite>(bindings, std::move(sprite));
        Text::Hooks font;
        font.enabled = options.text;
        font.refuse = refuser(Family::Text);
        font.fault = faulter(Family::Text);
        font.read = [this](auto at, auto* out, auto size) { return Read(at, out, size); };
        font.environment = [this](Environment& out) {
            if (sky_open || !output.ui(26, text_frame, text_environment)) return false;
            out = text_environment;
            return true;
        };
        font.selected = [this](uint32_t object) { return selection.Font(object); };
        font.record = [this](const auto& intent) { return RecordText(intent); };
        text = std::make_unique<Text>(bindings, std::move(font));
        Sky::Hooks backdrop;
        backdrop.enabled = options.sky;
        backdrop.refuse = refuser(Family::Sky);
        backdrop.fault = faulter(Family::Sky);
        backdrop.read = [this](auto at, auto* out, auto size) { return Read(at, out, size); };
        backdrop.converted = [this](uint32_t object) { return bindings.Converted(object); };
        backdrop.write = [this](const auto& writes) { return WriteSky(writes); };
        backdrop.environment = [this](Environment& out) { return output.sky(sky_frame, out); };
        backdrop.begin = [this](uint32_t width, uint32_t height) {
            if (sky_open || width != sky_frame.width || height != sky_frame.height) return false;
            sky_open = true;
            return true;
        };
        backdrop.record = [this](const Sky::Output& intent) {
            if (!sky_open) return false;
            Queue(0, [this, frame = sky_frame, intent](uint32_t&) { return draws->Sky(frame, intent); },
                  false, Family::Sky, Sky::kDrawModel);
            sky_open = false;
            return true;
        };
        sky = std::make_unique<Sky>(std::move(backdrop));
        // The owned mesh bucket. Every service comes from this owner;
        // the pass installs whether or not it is selected (the census counts the
        // model calls on both arms) and converts/hashes/records only when it is.
        MeshProducer::Services mesh;
        mesh.read = [this](auto at, auto* out, auto size) { return Read(at, out, size); };
        mesh.write = [this](const auto& writes) { return WriteGuest(writes); };
        mesh.texture = [this](uint32_t object, ::title::TextureAssets::View& out) { return bindings.View(object, out); };
        mesh.view = [this](const Graphics::Lease& lease, ::title::TextureAssets::View& out) {
            return bindings.Assets().Find(lease, out) == ::title::TextureAssets::Result::Ok;
        };
        mesh.environment = [this](uint32_t bucket, Frame& frame, Environment& out) {
            return !sky_open && output.mesh && output.mesh(bucket, frame, out);
        };
        mesh.queue = [this](uint32_t bucket, MeshProducer::Work work) {
            return Queue(bucket, std::move(work), false, Family::Meshes, ::title::MeshIntent::kFlushMaterial);
        };
        mesh.draw = [this](const Frame& frame, const auto& batch, uint32_t* why) { return draws->Assets(frame, batch, why); };
        mesh.refuse = refuser(Family::Meshes);
        mesh.fault = faulter(Family::Meshes);
        meshes = std::make_unique<MeshProducer>(options.meshes, std::move(mesh));
    }
};

GraphicsProducers::GraphicsProducers(MainMemory& memory, GraphicsOutput& output, VideoStack& video,
    ::title::TextureAssets& assets, Bindings& bindings, ::title::TextureSelection& selection, Options options,
    Window scratchpad, Window arena)
    : impl_(new Impl(memory, bindings, selection, options, Connect(output), scratchpad, arena)) {
    if (!video.Active() || !video.Resources() || !video.GraphicsRenderer() || !video.Scene())
        throw std::invalid_argument("Graphics producers require the shared video scene");
    impl_->owned_draws = std::make_unique<GraphicsProducerDraws>(*video.Resources(),
        *video.GraphicsRenderer(), *video.Scene(), assets, options.synthesize_tail);
    impl_->draws = impl_->owned_draws.get();
    impl_->BuildServices();
}
GraphicsProducers::GraphicsProducers(MainMemory& memory, GraphicsProducerDraws& draws, Bindings& bindings,
    ::title::TextureSelection& selection, Options options, Output output, Window scratchpad, Window arena)
    : impl_(new Impl(memory, bindings, selection, options, std::move(output), scratchpad, arena)) {
    impl_->draws = &draws;
    impl_->BuildServices();
}
// Dispatch must stop and the output owner must drain before destruction.
// Destroying captures still queued on that owner would make them dangling.
GraphicsProducers::~GraphicsProducers() {
    if (impl_->pending.load()) std::terminate();
}

bool GraphicsProducers::Install(const Capture& capture, const Installer& install) {
    auto& p = *impl_;
    if (p.installed || !capture || !install || !p.output.record || !p.output.immediate ||
        !p.output.ui || !p.output.sky || !p.output.movie) return false;
    // Match the same immutable TextureSelection used at load/destruction.
    // Under `textures` every record converts (Texture() is the convert policy,
    // true for every id), so the agreement is on the services themselves:
    // who draws a UI pair (text/sprites) and whether every record converts.
    const auto ready = p.selection.Ready();
    if (ready.text != p.options.text || ready.sprites != p.options.sprites ||
        ready.textures != p.options.textures) return false;
    constexpr std::array<uint32_t, 3> entries{kFlush, kForget, kLink};
    for (size_t i = 0; i < entries.size(); ++i)
        if (!capture(entries[i])) return false;
    if (!p.sprites->Install(capture, install) || !p.text->Install(capture, install) ||
        !p.sky->Install(capture, install) || !p.meshes->Install(capture, install)) return false;
    // The three originals are captured AFTER the sub-producers installed: the
    // mesh pass wraps FlushMaterial and ForgetMaterials too, and this owner's
    // Dispatch must chain onto its wrappers (a text material flushes here, every
    // other material continues into the pass, then the title's body).
    for (size_t i = 0; i < entries.size(); ++i) {
        p.originals[i] = capture(entries[i]);
        if (!p.originals[i]) return false;
    }
    for (size_t i = 0; i < entries.size(); ++i)
        if (!install(entries[i], [&p, i](CpuContext& c, uint32_t at) { p.Dispatch(i, c, at); })) return false;
    p.installed = true;
    return true;
}

::title::MovieGraphics::OutputServices GraphicsProducers::MovieOutput() {
    if (!impl_->options.movie) return {};
    ::title::MovieGraphics::OutputServices services;
    services.upload = [this](auto image) {
        return impl_->Queue(27, [this, image = std::move(image)](uint32_t&) { return impl_->draws->UploadMovie(image); }, true);
    };
    services.record = [this](const gs::DrawIr& draw, auto image) {
        Frame frame;
        if (impl_->sky_open || !impl_->output.movie(frame) || draw.raster.frame.fbw != frame.width)
            return false;
        return impl_->Queue(27, [this, frame, draw, image = std::move(image)](uint32_t&) {
            return impl_->draws->Movie(frame, draw, image);
        });
    };
    return services;
}

GraphicsProducers::Counters GraphicsProducers::Counts() const {
    const auto& p = *impl_;
    return {p.sprites->Counts(), p.text->Counts(), p.sky->Stats(), p.meshes->Counts(),
        p.flush_calls, p.flush_replaced, p.flush_model_calls,
        p.recorded.load(), p.completed.load(), p.pending.load(), p.failures.load(), p.materials.size(),
        p.cancelled.load(), p.refused_recordings, p.refused_slots.load()};
}
bool GraphicsProducers::CanCheckpoint(std::string* reason) const {
    const auto& p = *impl_;
    const auto refuse = [&](const std::string& why) {
        if (reason) *reason = "Graphics producers have " + why;
        return false;
    };
    if (p.faulted || p.failures.load() || p.sprites->Counts().failed || p.text->Counts().failed || p.sky->Stats().faults)
        return refuse("failed selected work");
    if (std::string why; !p.meshes->CanCheckpoint(&why)) return refuse(why);
    if (p.sky_open) return refuse("an open sky pass");
    if (!p.materials.empty()) return refuse("an unflushed text material (" + std::to_string(p.materials.size()) + ")");
    // Pending tickets at a boundary are the frame's recorded, unlinked owned
    // groups when the ordered output has settled (the title Links them after
    // SyncV): admitted through the settle; otherwise the capture refuses.
    const auto pending = p.pending.load();
    if (pending && !(p.output.settled && p.output.settled(pending)))
        return refuse("pending selected work (" + std::to_string(pending) + ") at an unsettled boundary");
    return true;
}
void GraphicsProducers::ResetAfterQuiescence() {
    // A reset (restore) needs the recording CLOSED: a settled open recording
    // admits a capture, never a reset that would orphan its tickets.
    if (!CanCheckpoint() || impl_->pending.load()) throw std::logic_error("Graphics producers reset before quiescence");
    impl_->draws->ResetAfterQuiescence();
    impl_->sky->Reset();
    impl_->meshes->ResetAfterQuiescence();
}
bool GraphicsProducers::CaptureSkyAssets(::title::SkyAssets::Snapshot& snapshot, std::string* reason) const {
    if (!CanCheckpoint(reason)) return false;
    // Pending tickets passed CanCheckpoint only through the ordered output's
    // settle: they are the frame's unlinked recording, and so are the model/
    // texture leases the recorded sky pass holds (GraphicsRuntime::CaptureTextures
    // applies the same rule to TextureAssets). Before the owned sky ever ran,
    // this refused every in-play boundary with an owned sky in flight:
    // `graphics_resources: Sky state: texture still leased by unfinished work`.
    const bool unlinked_open = impl_->pending.load() != 0;
    return impl_->sky->CaptureAssets(snapshot, reason, unlinked_open);
}
bool GraphicsProducers::RestoreSkyAssets(const ::title::SkyAssets::Snapshot& snapshot, std::string* reason) {
    return CanCheckpoint(reason) && impl_->sky->RestoreAssets(snapshot, reason);
}
bool GraphicsProducers::CaptureMeshReceipts(std::vector<uint8_t>& out) const {
    return impl_->meshes->CaptureReceipts(out);
}
bool GraphicsProducers::RestoreMeshReceipts(const std::vector<uint8_t>& payload,
                                            const ::title::MaterialState::Read& incoming, std::string* reason) {
    return CanCheckpoint(reason) && impl_->meshes->RestoreReceipts(payload, incoming, reason);
}

}  // namespace ps2r::app
