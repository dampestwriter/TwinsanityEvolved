#include "games/slus_20909_crash_twinsanity/composition/graphics_runtime.h"

#include <map>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_census.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_import.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/hash/hash.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_report.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_dump.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pack.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_uploads.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"

namespace ps2r::app {
namespace {
using Assets = ::title::TextureAssets;
using Bindings = ::title::TextureBindings;
using Uploads = ::title::TextureUploads;
using Selection = ::title::TextureSelection;
using Codec = ::title::TextureStateCodec;
using SkyCodec = ::title::SkyState;
using SkyAssets = ::title::SkyAssets;
using Blob = savestate::Blob;
constexpr uint32_t kSelectionTag = 0x4c535847; // GXSL
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
::title::MaterialState::Read ReadRam(const uint8_t* data, size_t size) {
    return [data, size](uint32_t address, void* out, size_t length) {
        if (!data || address > size || length > size - address) return false;
        std::memcpy(out, data + address, length);
        return true;
    };
}
void SkyDigest(std::vector<uint8_t>& selection, const std::vector<uint8_t>& bytes) {
    const auto hash = hash::Fnv1a64(bytes.data(), bytes.size());
    for (unsigned i = 0; i < 8; ++i) selection.push_back(uint8_t(hash >> (i * 8)));
}
GraphicsProducers::Output Connect(GraphicsOutput& output) {
    return {
        [&output](auto bucket, auto& frame, auto& environment) {
            return output.CaptureUi(bucket, frame, environment);
        },
        [&output](auto& frame, auto& environment) { return output.CaptureSky(frame, environment); },
        [&output](auto& frame) { return output.CaptureScene(27, frame); },
        [&output](auto bucket, auto& frame, auto& environment) {
            return output.CaptureMesh(bucket, frame, environment);
        },
        [&output](auto bucket, auto work) {
            return output.RecordOwned(bucket, [work = std::move(work)](VideoStack&) { return work(); });
        },
        [&output](auto work) {
            return output.EnqueueOwned([work = std::move(work)](VideoStack&) { return work(); });
        },
        [&output](uint64_t pending) {
            return output.SettleOwnedForCapture(nullptr) && output.UnlinkedAtBoundary().owned_items >= pending;
        }
    };
}
VideoStack& RequireVideo(VideoStack& video) {
    if (!video.Active() || !video.Resources() || !video.GraphicsRenderer() || !video.Scene())
        throw std::invalid_argument("Graphics selection requires the shared video scene");
    return video;
}
}  // namespace

struct GraphicsRuntime::Impl {
    struct State {
        ::title::Graphics textures;
        Assets assets;
        Selection selection;
        Bindings bindings;
        Uploads uploads;
        GraphicsProducerDraws draws;
        GraphicsProducers producers;
        std::map<uint32_t, Body> entries;
        State(Impl& root)
            : assets(textures, root.resources, root.backend, root.AssetLimits()),
              selection(assets, root.Guest(), {root.options.text, root.options.sprites, root.options.textures},
                        &root.refusals),
              bindings(assets, root.Guest(),
                       [this](auto object, auto id) { return selection.Texture(object, id); }),
              uploads(assets, root.Guest()),
              draws(root.resources, root.renderer, root.scene, assets, root.options.synthesize_tail),
              producers(root.memory, draws, bindings, selection, root.options, root.Services(),
                        root.scratchpad, root.arena) {
            if (root.texture_release)
                bindings.SetReleased([&root](Assets::Token token) { root.texture_release(token.generation); });
            if (root.pack.Active()) assets.SetPack(&root.pack);
            if (root.dump.Active()) assets.SetDump(&root.dump);
        }
    };
    MainMemory& memory;
    Window scratchpad, arena;
    // A4: the texture pack (opened + prewarmed at construction from
    // Options::texture_pack_dir) and the --dump-textures writer; both outlive
    // every State (a restore's fresh owner re-applies the pack at Restore).
    ::title::TexturePack pack;
    mutable ::title::TextureDump dump;   // Finish at the census (the exit path may not destruct)
    std::string pack_error, dump_error;
    ::title::GraphicsResources& resources;
    ::title::GraphicsRenderer& renderer;
    ::title::GraphicsScene& scene;
    render::Backend& backend;
    Options options;
    GraphicsProducers::Output output;
    std::map<uint32_t, Body> originals;
    Bindings::Deallocate deallocate;
    std::unique_ptr<State> state, candidate;
    std::vector<uint8_t> candidate_bytes;
    GraphicsImport* import = nullptr;
    bool legacy_candidate = false;   // the staged candidate came from the import
    bool installed = false, registered = false;
    uint64_t restores = 0;
    GraphicsCensus census;
    ::title::GraphicsRefusals refusals; // process lifetime: outlives every State
    ::title::GraphicsRefusals::Clock source_clock;
    std::function<bool(std::string*)> scene_checkpoint;
    // Textures at load: the slot binds / releases / the restore reset travel to
    // the GS consumer through GraphicsOutput (empty in module-only compositions).
    std::function<void()> detach_uploads;   // unhooks the walk listener before this owner dies
    std::function<void(uint32_t, ps2r::gs::OwnedTextureSlots::Texture&&)> texture_bind;
    std::function<void(uint64_t)> texture_release;
    std::function<void()> texture_reset;
    // Every record of a level converts under `textures`: the byte budget grows
    // with it (a level's textures as RGBA8 levels; the retained UI set is small).
    Assets::Limits AssetLimits() const {
        Assets::Limits limits;
        if (options.textures) limits.bytes = 512ull * 1024 * 1024;
        return limits;
    }
    GraphicsProducers::Output Services() {
        auto services = output;
        services.refusals = &refusals;
        return services;
    }
    Bindings::Memory Guest() const {
        return {memory.Data(), memory.Size(), scratchpad.data, scratchpad.size};
    }

    Impl(MainMemory& ram, ::title::GraphicsResources& r, ::title::GraphicsRenderer& draw,
         ::title::GraphicsScene& targets, render::Backend& device, Options selected,
         GraphicsProducers::Output services, Window stack, Window extra)
        : memory(ram), scratchpad(stack), arena(extra), resources(r), renderer(draw), scene(targets),
          backend(device), options(selected), output(std::move(services)) {
        if (!options.texture_pack_dir.empty()) {
            if (pack.Open(options.texture_pack_dir, &pack_error)) pack.Prewarm();
            else std::fprintf(stderr, "[textures] %s\n", pack_error.c_str());
        }
        if (!options.dump_textures_dir.empty() && !dump.Open(options.dump_textures_dir, &dump_error))
            std::fprintf(stderr, "[textures] %s\n", dump_error.c_str());
    }
    ~Impl() { dump.Finish(); }
    bool OwnsTextures() const { return options.sprites || options.text || options.textures; }
    std::vector<uint8_t> SelectionBytes() const {
        return {uint8_t(options.sprites), uint8_t(options.text), uint8_t(options.sky),
                uint8_t(options.movie), uint8_t(options.synthesize_tail)};
    }
    std::unique_ptr<State> Build(const Capture& capture) {
        auto next = std::make_unique<State>(*this);
        const auto read = [&](uint32_t address) -> Body {
            if (const auto local = next->entries.find(address); local != next->entries.end())
                return local->second;
            auto found = originals.find(address);
            if (found == originals.end()) {
                const auto body = capture ? capture(address) : Body{};
                if (!body) return {};
                found = originals.emplace(address, body).first;
            }
            return found->second;
        };
        const auto bind = [&](uint32_t address, Body body) {
            if (!body) return false;
            next->entries[address] = std::move(body);
            return true;
        };
        if ((OwnsTextures() && !next->bindings.Install(read, bind, deallocate,
                                                       refusals.Refuser(::title::GraphicsRefusals::Family::Textures))) ||
            (options.textures && !next->uploads.Install(read, bind)) ||
            !next->producers.Install(read, bind)) return {};
        return next;
    }
    bool CaptureTextures(Assets::Snapshot& snapshot, std::string* reason) const {
        if (!state || !state->producers.CanCheckpoint(reason)) return false;
        // Pending producer tickets passed CanCheckpoint only through the ordered
        // output's settle: they are the frame's unlinked recording, and so are
        // the texture leases their captured draws hold.
        const bool unlinked_open = state->producers.Counts().pending != 0;
        if (state->assets.Capture(snapshot, reason, unlinked_open) != Assets::Result::Ok) return false;
        return Selection::PreflightSnapshot(snapshot, {memory.Data(), memory.Size()},
                                            {options.text, options.sprites, options.textures}, reason);
    }
    bool CaptureSky(SkyAssets::Snapshot& snapshot, std::string* reason) const {
        return state && state->producers.CaptureSkyAssets(snapshot, reason) &&
            SkyAssets::Preflight(snapshot, ReadRam(memory.Data(), memory.Size()), reason);
    }
    std::vector<uint8_t> CaptureSelection() const {
        auto bytes = SelectionBytes();
        if (options.sky) {
            SkyAssets::Snapshot snapshot;
            std::vector<uint8_t> sky;
            std::string reason;
            if (!CaptureSky(snapshot, &reason) || !SkyCodec::Encode(snapshot, sky, &reason))
                throw std::runtime_error("Graphics capture: " + reason);
            SkyDigest(bytes, sky);
        }
        return bytes;
    }
    // The staged candidate becomes the owner (GXSL restore / legacy absent).
    void Publish() {
        census.Retire(state->producers.Counts(), state->bindings.Counts(), state->assets.Counts());
        state.swap(candidate);
        candidate.reset();
        candidate_bytes.clear();
        legacy_candidate = false;
        ++restores;
        // The published owner's generations are new: every slot bind of the
        // retired owner is stale (the title re-uploads every slot next frame).
        if (texture_reset) texture_reset();
    }
    // A frozen image the import admits: the selected textures derived from
    // its own RAM0/META, validated against the same incoming RAM after the
    // owned commit words it will publish; the selection is the active one.
    bool PreflightLegacy(const std::vector<Blob>& sections, const Blob& ram, std::string* reason) {
        if (!import->Prepare(sections, reason)) return false;   // admission + derivation, refusals named

        Assets::Snapshot snapshot = import->Textures();
        if (OwnsTextures()) {
            std::vector<uint8_t> migrated(ram.payload);
            if (!::title::TextureImport::Migrate(migrated.data(), migrated.size(), import->Objects()) ||
                !Selection::PreflightSnapshot(snapshot, {migrated.data(), migrated.size()},
                                              {options.text, options.sprites, options.textures}, reason)) return false;
        }
        auto next = Build({});
        if (!next) return Refuse(reason, "Graphics restore composition has no original body");
        std::vector<Assets::Token> uploads;
        if (OwnsTextures() && next->assets.Restore(snapshot, uploads, reason) != Assets::Result::Ok) return false;
        candidate_bytes = SelectionBytes();
        candidate = std::move(next);
        legacy_candidate = true;
        return true;
    }
    bool Preflight(const std::vector<Blob>& sections, std::string* reason) {
        candidate.reset();
        candidate_bytes.clear();
        legacy_candidate = false;
        const Blob* ram = nullptr;
        const Blob* textures = nullptr;
        const Blob* sky = nullptr;
        const Blob* selection = nullptr;
        const Blob* receipts = nullptr;
        for (const auto& section : sections) {
            if (section.tag == savestate::TAG_RAM) ram = &section;
            if (section.tag == Codec::kTag) textures = &section;
            if (section.tag == SkyCodec::kTag) sky = &section;
            if (section.tag == kSelectionTag) selection = &section;
            if (section.tag == MeshProducer::kReceiptsTag) receipts = &section;
        }
        if (ram && !selection && import) return PreflightLegacy(sections, *ram, reason);
        if (!ram || !selection || (OwnsTextures() && !textures) || (options.sky && !sky))
            return Refuse(reason, "Graphics requires incoming RAM0 and every selected resource section");
        Assets::Snapshot snapshot;
        if (OwnsTextures() && (!Codec::Decode(textures->payload, snapshot, reason) ||
            !Selection::PreflightSnapshot(snapshot,
                {const_cast<uint8_t*>(ram->payload.data()), ram->payload.size()},
                {options.text, options.sprites, options.textures}, reason))) return false;
        SkyAssets::Snapshot sky_snapshot;
        if (options.sky) {
            auto inventory = SelectionBytes();
            SkyDigest(inventory, sky->payload);
            if (selection->payload != inventory)
                return Refuse(reason, "Graphics SKYA does not match the captured owner inventory");
            if (!SkyCodec::Decode(sky->payload, sky_snapshot, reason) ||
                !SkyAssets::Preflight(sky_snapshot, ReadRam(ram->payload.data(), ram->payload.size()), reason))
                return false;
        }
        auto next = Build({});
        if (!next) return Refuse(reason, "Graphics restore composition has no original body");
        std::vector<Assets::Token> uploads;
        if (OwnsTextures() && next->assets.Restore(snapshot, uploads, reason) != Assets::Result::Ok)
            return false;
        if (options.sky && !next->producers.RestoreSkyAssets(sky_snapshot, reason)) return false;
        // The owned mesh bucket's receipts (MSHR), verified against the incoming
        // image. An image without the section (an older capture) restores no
        // receipt: its loaded materials leave every frame whole to the model path
        // (MeshProducer::FrameAdmits) until their chunks unload.
        if (options.meshes && receipts &&
            !next->producers.RestoreMeshReceipts(receipts->payload, ReadRam(ram->payload.data(), ram->payload.size()),
                                                 reason)) return false;
        // These allocations are intentionally not uploaded here. The first
        // actual source draw uploads every authored level at its ordered slot.
        candidate_bytes = selection->payload;
        candidate = std::move(next);
        return true;
    }
};

GraphicsRuntime::GraphicsRuntime(MainMemory& memory, GraphicsOutput& output, VideoStack& video, Options options,
                                 Window scratchpad, Window arena)
    : GraphicsRuntime(memory, *RequireVideo(video).Resources(), *video.GraphicsRenderer(),
                      *video.Scene(), video.Pipe()->Backend(), options, Connect(output), scratchpad, arena) {
    impl_->scene_checkpoint = [&output](auto* error) { return output.CanCheckpointScene(error); };
    impl_->source_clock = [&output] { return output.SourceFrame(); };
    impl_->refusals.SetClocks({}, impl_->source_clock);
    if (options.textures) {
        impl_->detach_uploads = [&output] { output.SetUploadTagListener({}); };
        impl_->texture_bind = [&output](uint32_t base, ps2r::gs::OwnedTextureSlots::Texture&& texture) {
            output.DeliverTextureBind(base, std::move(texture));
        };
        impl_->texture_release = [&output](uint64_t key) { output.DeliverTextureRelease(key); };
        impl_->texture_reset = [&output] { output.ResetOwnedTextures(); };
        output.SetUploadTagListener([this](uint32_t address, bool scratchpad, bool first_tag) {
            auto& p = *impl_;
            if (!p.state) return;
            if (first_tag) p.state->uploads.SetFrame(p.source_clock ? p.source_clock() : 0);
            Uploads::Upload upload;
            if (p.state->uploads.TakeTag(address, scratchpad, upload))
                p.texture_bind(upload.base, std::move(upload.texture));
        });
    }
}
void GraphicsRuntime::SetFrameClock(::title::GraphicsRefusals::Clock host_frame) {
    impl_->refusals.SetClocks(std::move(host_frame), impl_->source_clock);
}
const ::title::GraphicsRefusals& GraphicsRuntime::Refusals() const { return impl_->refusals; }
void GraphicsRuntime::SetImport(GraphicsImport* import) {
    if (!impl_->registered) impl_->import = import;
}
GraphicsRuntime::GraphicsRuntime(MainMemory& memory, ::title::GraphicsResources& resources,
    ::title::GraphicsRenderer& renderer, ::title::GraphicsScene& scene, render::Backend& backend,
    Options options, GraphicsProducers::Output output, Window scratchpad, Window arena)
    : impl_(new Impl(memory, resources, renderer, scene, backend, options, std::move(output), scratchpad, arena)) {}
GraphicsRuntime::~GraphicsRuntime() {
    if (impl_->detach_uploads) impl_->detach_uploads();
}

bool GraphicsRuntime::Install(const Capture& capture, const Installer& install, Bindings::Deallocate free) {
    auto& p = *impl_;
    if (p.installed || !capture || !install || (p.OwnsTextures() && !free)) return false;
    p.deallocate = std::move(free);
    auto state = p.Build(capture);
    if (!state) return false;
    p.state = std::move(state);
    for (const auto& entry : p.state->entries) {
        const uint32_t address = entry.first;
        if (!install(address, [this, address](CpuContext& context, uint32_t at) {
            impl_->state->entries.at(address)(context, at);
        })) return false; // abort startup; never execute an installed prefix
    }
    p.installed = true;
    return true;
}

bool GraphicsRuntime::RegisterState(StateRestorer& restorer) {
    auto& p = *impl_;
    if (!p.installed || p.registered) return false;
    const bool legacy = p.import != nullptr;
    savestate::SectionSpec selection;
    selection.tag = kSelectionTag;
    selection.version = p.options.sky ? 2 : 1;
    selection.owner = "Graphics selection";
    selection.required = !legacy;
    selection.capture_required = true;
    selection.serialize = [this](std::vector<uint8_t>& out) {
        std::string reason;
        if (!CanCheckpoint(&reason)) throw std::runtime_error("Graphics capture: " + reason);
        out = impl_->CaptureSelection();
    };
    selection.validate = [this](const Blob& blob, std::string* reason) {
        const auto& owner = *impl_;
        const auto expected = owner.SelectionBytes();
        const auto prefix = blob.payload.size() >= expected.size()
            ? std::vector<uint8_t>(blob.payload.begin(), blob.payload.begin() + expected.size())
            : std::vector<uint8_t>{};
        if (blob.version != (owner.options.sky ? 2u : 1u) || prefix != expected ||
            blob.payload.size() != expected.size() + (owner.options.sky ? 8u : 0u))
            return Refuse(reason, "Graphics snapshot selection differs from the active owners");
        return true;
    };
    selection.restore = [this](const Blob& blob, std::string* error) {
        auto& owner = *impl_;
        if (!owner.candidate || owner.legacy_candidate || owner.candidate_bytes != blob.payload ||
            !owner.state->producers.CanCheckpoint(error))
            return Refuse(error, "Graphics restore was not staged at a quiescent boundary");
        owner.Publish();
        return true;
    };
    if (legacy) {
        // The frozen image's position: RAM0 is restored; the owned commit words
        // of the derived textures land there, then the candidate publishes.
        selection.absent = [this] {
            auto& owner = *impl_;
            std::string error;
            if (!owner.candidate || !owner.legacy_candidate || !owner.import->Prepared() ||
                !owner.state->producers.CanCheckpoint(&error))
                throw std::runtime_error("Graphics legacy import was not staged at a quiescent boundary: " + error);
            if (!owner.import->PublishMemory(owner.memory.Data(), owner.memory.Size(), &error))
                throw std::runtime_error(error);
            owner.Publish();
        };
    }
    if (!restorer.Registry().Register(selection)) return false;
    if (p.OwnsTextures()) {
        savestate::SectionSpec spec;
        spec.tag = Codec::kTag;
        spec.version = Codec::kVersion;
        spec.required = !legacy;
        spec.capture_required = true;
        spec.owner = "Graphics TXAS";
        spec.serialize = [this](std::vector<uint8_t>& out) {
            Assets::Snapshot snapshot;
            std::string error;
            if (!impl_->CaptureTextures(snapshot, &error) || !Codec::Encode(snapshot, out, &error))
                throw std::runtime_error("Graphics capture: " + error);
        };
        spec.validate = [](const Blob& blob, std::string* error) {
            return blob.version == Codec::kVersion && Codec::Validate(blob.payload, error);
        };
        // GXSL publishes the complete already-staged composition once.
        spec.restore = [](const Blob&, std::string*) { return true; };
        if (!restorer.Registry().Register(spec)) return false;
    }
    if (p.options.sky) {
        savestate::SectionSpec spec;
        spec.tag = SkyCodec::kTag;
        spec.version = SkyCodec::kVersion;
        spec.required = !legacy;
        spec.capture_required = true;
        spec.owner = "Graphics Sky converted assets";
        spec.serialize = [this](std::vector<uint8_t>& out) {
            SkyAssets::Snapshot snapshot;
            std::string error;
            if (!impl_->CaptureSky(snapshot, &error) || !SkyCodec::Encode(snapshot, out, &error))
                throw std::runtime_error("Graphics capture: " + error);
        };
        spec.validate = [](const Blob& blob, std::string* error) {
            return blob.version == SkyCodec::kVersion && SkyCodec::Validate(blob.payload, error);
        };
        spec.restore = [](const Blob&, std::string*) { return true; };
        if (!restorer.Registry().Register(spec)) return false;
    }
    if (p.options.meshes) {
        savestate::SectionSpec spec;
        spec.tag = MeshProducer::kReceiptsTag;
        spec.version = MeshProducer::kReceiptsVersion;
        spec.required = false;           // older images carry none: their frames run whole-model
        spec.capture_required = true;
        spec.owner = "Graphics mesh receipts";
        spec.serialize = [this](std::vector<uint8_t>& out) {
            if (!impl_->state->producers.CaptureMeshReceipts(out))
                throw std::runtime_error("Graphics capture: mesh receipts are malformed");
        };
        spec.validate = [](const Blob& blob, std::string* error) {
            return blob.version == MeshProducer::kReceiptsVersion && MeshProducer::ValidateReceipts(blob.payload, error);
        };
        // GXSL publishes the complete already-staged composition once; the
        // receipts were restored into the candidate at the image preflight.
        spec.restore = [](const Blob&, std::string*) { return true; };
        if (!restorer.Registry().Register(spec)) return false;
    }
    restorer.AddImagePreflight([this](const auto& sections, auto* error) {
        return impl_->Preflight(sections, error);
    }, [this] {
        auto& owner = *impl_;
        owner.candidate.reset();
        owner.candidate_bytes.clear();
        owner.legacy_candidate = false;
        if (owner.import) owner.import->Discard();
    });
    p.registered = true;
    return true;
}

bool GraphicsRuntime::CanCheckpoint(std::string* reason) const {
    const auto& p = *impl_;
    if (!p.installed || !p.state->producers.CanCheckpoint(reason)) return false;
    const auto counts = p.state->producers.Counts();
    // Module-only composition must supply the actual shared-surface owner;
    // converted texture state cannot substitute for attachment contents.
    if (counts.recorded && !p.scene_checkpoint)
        return Refuse(reason, "Graphics scene state capture is not yet composed");
    if (p.scene_checkpoint && !p.scene_checkpoint(reason)) return false;
    if (p.OwnsTextures()) {
        Assets::Snapshot snapshot;
        if (!p.CaptureTextures(snapshot, reason)) return false;
    }
    if (p.options.sky) {
        SkyAssets::Snapshot snapshot;
        if (!p.CaptureSky(snapshot, reason)) return false;
    }
    return true;
}

::title::MovieGraphics::OutputServices GraphicsRuntime::MovieOutput() {
    if (!impl_->installed || !impl_->options.movie) return {};
    return {
        [this](auto image) { return impl_->state->producers.MovieOutput().upload(std::move(image)); },
        [this](const auto& draw, auto image) {
            return impl_->state->producers.MovieOutput().record(draw, std::move(image));
        }
    };
}

Assets::Counters GraphicsRuntime::TextureCounts() const {
    return impl_->census.Assets(impl_->state->assets.Counts());
}
GraphicsProducers::Counters GraphicsRuntime::ProducerCounts() const {
    return impl_->census.Producers(impl_->state->producers.Counts());
}
uint64_t GraphicsRuntime::Restores() const { return impl_->restores; }
void GraphicsRuntime::Record(verify::RunMeta& meta) const {
    const auto& p = *impl_;
    const auto counts = ProducerCounts();
    const auto textures = p.census.Bindings(p.state->bindings.Counts());
    const auto assets = TextureCounts();
    meta.Set("graphics_selection", "sprites=" + std::to_string(p.options.sprites) +
        ",text=" + std::to_string(p.options.text) + ",sky=" + std::to_string(p.options.sky) +
        ",movie=" + std::to_string(p.options.movie));
    ::title::GraphicsReport::Sprites(meta, counts.sprites);
    ::title::GraphicsReport::Text(meta, counts.text);
    // A4: the texture pack (hits = converted textures the pack replaced by
    // content hash) and the --dump-textures writer; packs off: pack=none,
    // replaced=0 -- the converted textures exactly.
    {
        const auto pack = p.pack.Counts();
        const auto replaced = p.state->assets.PackCounts();
        meta.Set("graphics_texture_pack", std::string("pack=") + (p.pack.Active() ? p.options.texture_pack_dir : "none") +
            ",files=" + std::to_string(pack.files) + ",lookups=" + std::to_string(pack.lookups) +
            ",hits=" + std::to_string(pack.hits) + ",misses=" + std::to_string(pack.misses) +
            ",replaced=" + std::to_string(replaced.replaced) + ",decoded=" + std::to_string(pack.decoded) +
            ",decode_failures=" + std::to_string(pack.decode_failures) + ",prewarmed=" + std::to_string(pack.prewarmed) +
            ",cached_bytes=" + std::to_string(pack.cached_bytes) + ",uncached=" + std::to_string(pack.uncached));
        if (p.dump.Active()) {
            p.dump.Finish();   // index.tsv + contact_sheet.png at the census; idempotent
            const auto dump = p.dump.Counts();
            meta.Set("graphics_texture_dump", "dir=" + p.options.dump_textures_dir +
                ",records=" + std::to_string(dump.records) + ",written=" + std::to_string(dump.written) +
                ",failures=" + std::to_string(dump.failures));
        }
    }
    meta.Set("graphics_textures", "loaded=" + std::to_string(textures.loaded) +
        ",destroyed=" + std::to_string(textures.destroyed) + ",model_calls=" +
        std::to_string(textures.declined) + ",refused=" + std::to_string(textures.refused) +
        ",failed=" + std::to_string(textures.failed) +
        ",uploaded=" + std::to_string(assets.uploaded) + ",retained_bytes=" + std::to_string(assets.retained_bytes) +
        ",current=" + std::to_string(assets.current) + ",restores=" + std::to_string(p.restores));
    // Textures at load: the title's WriteTextureUpload calls this owner took
    // (owned = an empty tag + a slot bind), left to the original (declined: an
    // object this owner never converted; refused: a converted object whose
    // header or slot could not be read), the binds the walk delivered / dropped.
    const auto uploads = p.state->uploads.Counts();
    meta.Set("graphics_texture_uploads", "selected=" + std::to_string(p.options.textures) +
        ",calls=" + std::to_string(uploads.calls) + ",owned=" + std::to_string(uploads.owned) +
        ",model_calls=" + std::to_string(uploads.declined) + ",refused=" + std::to_string(uploads.refused) +
        ",taken=" + std::to_string(uploads.taken) + ",discarded=" + std::to_string(uploads.discarded) +
        ",overwritten=" + std::to_string(uploads.overwritten) +
        ",pending=" + std::to_string(uploads.pending));
    meta.Set("graphics_producers", "recorded=" + std::to_string(counts.recorded) +
        ",completed=" + std::to_string(counts.completed) + ",pending=" + std::to_string(counts.pending) +
        ",failed=" + std::to_string(counts.failures) + ",cancelled=" + std::to_string(counts.cancelled) +
        ",refused_recordings=" + std::to_string(counts.refused_recordings) +
        ",refused_slots=" + std::to_string(counts.refused_slots));
    ::title::GraphicsReport::Sky(meta, counts.sky);
    // The owned mesh bucket. `graphics_meshes` is the producer row
    // (section 9: calls/replaced/model_calls per boundary; replaced instances
    // never reach the writers), `graphics_mesh_assets` the ModelRead conversions,
    // `graphics_mesh_materials` the MaterialResourceRead receipts, the
    // certificate's verdicts and the FlushMaterial reservations. Retirement
    // counter: gs_sink_path1_by_bucket 2/3.
    {
        const auto& m = counts.meshes;
        const auto& pass = m.pass;
        ::title::GraphicsReport::Meshes(meta, pass, p.options.meshes, m.delivered_draws);
        meta.Set("graphics_mesh_assets", "model_reads=" + std::to_string(m.model_reads) +
            ",loaded=" + std::to_string(m.models_loaded) + ",invalid=" + std::to_string(m.models_invalid) +
            ",unsupported=" + std::to_string(m.models_unsupported) + ",limit=" + std::to_string(m.models_limit) +
            ",destroys=" + std::to_string(m.model_destroys) + ",lazy_loads=" + std::to_string(m.lazy_loads) +
            ",lazy_misses=" + std::to_string(m.lazy_misses) + ",live=" + std::to_string(m.models_live));
        meta.Set("graphics_mesh_materials", "reads=" + std::to_string(m.material_reads) +
            ",receipts=" + std::to_string(m.receipts) + ",unhashed=" + std::to_string(m.unhashed) +
            ",live=" + std::to_string(m.receipts_live) +
            ",owns_calls=" + std::to_string(m.owns_calls) + ",certified=" + std::to_string(m.certified) +
            ",no_receipt=" + std::to_string(m.no_receipt) + ",unknown_material=" + std::to_string(m.unknown_material) +
            ",unknown_record=" + std::to_string(m.unknown_record) + ",unowned_consumer=" + std::to_string(m.unowned_consumer) +
            ",unverified_archive=" + std::to_string(m.unverified_archive) +
            ",texture_disagrees=" + std::to_string(m.texture_disagrees) + ",bucket_declined=" + std::to_string(m.bucket_declined) +
            ",receipt_stale=" + std::to_string(m.receipt_stale) +
            ",texture_lookups=" + std::to_string(m.texture_lookups) + ",texture_missing=" + std::to_string(m.texture_missing) +
            ",texture_mismatch=" + std::to_string(m.texture_mismatch) +
            ",requests=" + std::to_string(m.requests) + ",reserved=" + std::to_string(m.reserved) +
            ",refused_environment=" + std::to_string(m.refused_environment) + ",refused_view=" + std::to_string(m.refused_view) +
            ",refused_slot=" + std::to_string(m.refused_slot) +
            ",restored=" + std::to_string(m.receipts_restored) + ",dropped=" + std::to_string(m.receipts_dropped) +
            ",frames_admitted=" + std::to_string(m.frames_admitted) + ",frames_whole_model=" + std::to_string(m.frames_whole_model) +
            ",table_unreadable=" + std::to_string(m.table_unreadable) + ",unreceipted_live=" + std::to_string(m.unreceipted_live) +
            ",mixed_frames=" + std::to_string(m.mixed_frames));
    }
    // What the UI texture selection left to the original, by texture ID with
    // its TextureConsumers family bits (0xffff = no retail record; sprites'
    // texture 0 = untextured/unreadable material): the attribution row for
    // graphics_sprites model_calls / graphics_textures model_calls.
    const auto left = [](const std::vector<::title::TextureSelection::Left>& rows) {
        std::string text;
        size_t shown = 0;
        uint64_t total = 0;
        for (const auto& row : rows) {
            total += row.count;
            if (shown++ >= 12) continue;
            if (!text.empty()) text += ",";
            char id[32];
            std::snprintf(id, sizeof id, "%08x:%04x:", row.texture, row.families);
            text += id + std::to_string(row.count);
        }
        return "ids=" + std::to_string(rows.size()) + ",total=" + std::to_string(total) +
            ",top=" + (text.empty() ? std::string("none") : text);
    };
    meta.Set("graphics_sprites_model_by_texture", left(p.state->selection.UnselectedSprites()));
    meta.Set("graphics_textures_model_by_texture", left(p.state->selection.DeclinedTextures()));
    using Family = ::title::GraphicsRefusals::Family;
    p.refusals.Record(meta, {Family::Textures, Family::Sprites, Family::Text, Family::Sky, Family::Meshes});
}

}  // namespace ps2r::app
