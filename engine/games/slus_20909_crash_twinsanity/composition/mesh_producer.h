#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/composition/graphics_producer_draws.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"

namespace ps2r::app {

// The root composition
// of the owned mesh bucket for the type-01 rigid/placed family. Owns the
// MeshPass (the six source boundaries), the MeshAssets converted at the title's
// ModelRead, and the MaterialResourceRead receipts the MaterialCertificate is
// consulted with. Every hook the pass needs is supplied from the GraphicsProducers
// owner through Services: guest reads/writes, the texture owner's published
// View, the material buckets' frame environment, and the ordered output's
// bucket slot. No VIF/VU1/GIF transport: a selected material's draws are built
// from the owned model asset and recorded at the bucket's slot of the ordered
// output; the model writers never run for it (gs_sink_path1_by_bucket is the
// retirement counter).
class MeshProducer {
public:
    using Body = ::title::MeshPass::Body;
    using Capture = ::title::MeshPass::Capture;
    using Installer = ::title::MeshPass::Installer;
    using Frame = GraphicsProducerDraws::Frame;
    using Environment = ::title::MaterialState::Environment;
    using Work = std::function<bool(uint32_t& why)>;
    static constexpr uint32_t kMaterialResourceRead = 0x001c0670;
    static constexpr uint32_t kModelRead = ::title::MeshAssets::kModelRead;
    static constexpr uint32_t kModelDestroy = ::title::MeshAssets::kModelDestroy;
    // The title's live material table (TR graphicstables.cpp g_MaterialTable,
    // GraphicsTable<MaterialKind>: entries{item*, id} at +0, count/capacity
    // halves at +4/+6, vtable at +0x14; an item is a MaterialResource: id +4,
    // Material* +8). Every live material is listed here (GraphicsResourceReader::
    // Read inserts at the read), so a frame's admission (FrameAdmits) is
    // decided over it: a listed material without a receipt leaves the frame
    // whole to the model path.
    static constexpr uint32_t kMaterialTable = 0x003b0208, kMaterialTableVtable = 0x002f9188;
    static constexpr uint32_t kMaterialTableCapacity = 4096;
    // Savestate section: the receipts the MaterialResourceRead observer holds,
    // restored for the materials the image's table still binds to their resource id
    // (the record bytes are consumed from the chunk's streamed section and are
    // not in the image afterwards: TR graphicstables.cpp GraphicsResourceReader::
    // Read over a MemoryStream of the storage buffer; readers.cpp ReadSections_).
    static constexpr uint32_t kReceiptsTag = 0x5248534d;   // MSHR
    static constexpr uint32_t kReceiptsVersion = 1;
    // Slot refusal details (ORed into GraphicsProducers' kSlotRefused word).
    static constexpr uint32_t kUnfilled = 0x80;
    struct Services {
        ::title::MaterialState::Read read;
        std::function<bool(const std::vector<::title::MeshPass::Write>&)> write;
        // The texture owner's published asset of a GameTexture object (TextureBindings::View).
        std::function<bool(uint32_t object, ::title::TextureAssets::View&)> texture;
        // The same owner's View of an allocation by LEASE (TextureAssets::Find(lease)):
        // the exact generation the material was captured with, pinned for the frame.
        std::function<bool(const ::title::Graphics::Lease&, ::title::TextureAssets::View&)> view;
        // GraphicsOutput::CaptureMesh for the material's bucket.
        std::function<bool(uint32_t bucket, Frame&, Environment&)> environment;
        // GraphicsProducers' Queue at the bucket's slot (family Meshes, producer FlushMaterial).
        std::function<bool(uint32_t bucket, Work)> queue;
        // The render-owner half: Assets(frame, draws, &why) at the slot.
        std::function<bool(const Frame&, const std::vector<GraphicsProducerDraws::AssetDraw>&, uint32_t*)> draw;
        ::title::GraphicsRefusals::Sink refuse, fault;
    };
    struct Counters {
        ::title::MeshPass::Counters pass;
        // ModelRead observer: conversions (owned model assets) and declines by MeshAssets::Result.
        uint64_t model_reads = 0, models_loaded = 0, models_invalid = 0, models_unsupported = 0, models_limit = 0;
        uint64_t model_destroys = 0, lazy_loads = 0, lazy_misses = 0;
        // MaterialResourceRead observer: receipts (record digests) and reads it could not hash.
        uint64_t material_reads = 0, receipts = 0, unhashed = 0;
        // owns_material by MaterialCertificate::Result (+ this owner's own declines).
        uint64_t owns_calls = 0, certified = 0, no_receipt = 0, unknown_material = 0, unknown_record = 0,
                 unowned_consumer = 0, unverified_archive = 0, texture_disagrees = 0, bucket_declined = 0;
        // a receipt whose Material* now holds other shader bytes (the address reused; a restored image)
        uint64_t receipt_stale = 0;
        // texture lookups: the owner's View taken / missing / header-asset disagreement.
        uint64_t texture_lookups = 0, texture_missing = 0, texture_mismatch = 0;
        // reserve: requests recorded at their bucket, refused before commit (environment / view / slot).
        uint64_t requests = 0, reserved = 0, refused_environment = 0, refused_view = 0, refused_slot = 0;
        uint64_t delivered_draws = 0; // DrawIr records handed to the ordered output
        size_t receipts_live = 0, models_live = 0;
        // Restore: receipts the image's section carried / dropped (the image's
        // Material at the address holds other shader bytes).
        uint64_t receipts_restored = 0, receipts_dropped = 0;
        // FrameAdmits: frames admitted / refused whole (a live material without a
        // receipt; `unreceipted_live` = how many at the last decision; a table the
        // owner could not read counts as refused too: `table_unreadable`).
        uint64_t frames_admitted = 0, frames_whole_model = 0, table_unreadable = 0;
        size_t unreceipted_live = 0;
        // Admitted frames in which the pass replaced a boundary AND a receipt
        // refusal (no_receipt / receipt_stale) still fell a material to the
        // writer: owned and model producers mixed in one material list. The
        // admission gate makes this zero by construction.
        uint64_t mixed_frames = 0;
    };
    MeshProducer(bool enabled, Services services);
    ~MeshProducer();
    MeshProducer(const MeshProducer&) = delete;
    MeshProducer& operator=(const MeshProducer&) = delete;
    // Installs the pass's six boundaries and the three observers (ModelRead,
    // ModelDestroy, MaterialResourceRead). The pass installs with its hooks in
    // place whether or not it is enabled (the census counts the model calls);
    // the observers convert and hash only when enabled.
    bool Install(const Capture&, const Installer&);
    bool Enabled() const { return enabled_; }
    Counters Counts() const;
    bool CanCheckpoint(std::string* reason = nullptr) const;
    // Quiescent reset (restore): the pass's abandoned frame; the converted models
    // stay (their source objects are still loaded) and a missing one converts
    // lazily at its first instance.
    void ResetAfterQuiescence();
    // The receipts as a savestate section payload (sorted by address; empty
    // when none) and their restore into a fresh owner from an image: a receipt
    // whose Material in `incoming` reads another shader identity is dropped.
    // Validate checks the payload shape alone (the section's validate hook).
    bool CaptureReceipts(std::vector<uint8_t>& out) const;
    static bool ValidateReceipts(const std::vector<uint8_t>& payload, std::string* error);
    bool RestoreReceipts(const std::vector<uint8_t>& payload, const ::title::MaterialState::Read& incoming,
                         std::string* error);
    // The frame admission (MeshPass::Hooks::frame_admits): true when every live
    // material the title's table lists has a receipt. Also closes the previous
    // frame's mixed-frame judgement.
    bool FrameAdmits();
    // The hooks the pass is given (MeshPass::Hooks owns_material / texture /
    // model / reserve), callable directly by the composition's own oracle.
    bool OwnsMaterial(uint32_t material);
    bool Texture(uint32_t object, ::title::MeshMaterial::Texture& out);
    ::title::MeshAssets::ModelLease Model(uint32_t object);
    bool Reserve(const std::vector<::title::MeshPass::Request>&, std::vector<::title::MeshPass::Record>&);

private:
    struct Slot;
    void ObserveModelRead(CpuContext&, uint32_t entry);
    void ObserveModelDestroy(CpuContext&, uint32_t entry);
    void ObserveMaterialRead(CpuContext&, uint32_t entry);
    template<class T> bool Value(uint32_t address, T& out) const {
        return services_.read && services_.read(address, &out, sizeof(T));
    }
    // id + record digest, plus the Material's shader bytes digest at the read:
    // a later owns_material on the same address must find the same shader.
    struct Receipt { uint32_t id = 0; std::string sha256; uint64_t shader_hash = 0; };
    uint64_t ShaderHash(uint32_t material) const { return ShaderHash(services_.read, material); }
    static uint64_t ShaderHash(const ::title::MaterialState::Read& read, uint32_t material);
    static bool LiveMaterials(const ::title::MaterialState::Read& read, std::map<uint32_t, uint32_t>& out);
    // Per admitted frame: receipt refusals seen and the pass's replaced total at
    // the admission (the mixed-frame judgement at the next admission).
    bool frame_admitted_ = false;
    uint64_t frame_receipt_refusals_ = 0, frame_replaced_at_admission_ = 0;
    uint64_t PassReplaced() const;
    bool enabled_ = false;
    Services services_;
    ::title::MeshAssets assets_;
    std::unique_ptr<::title::MeshPass> pass_;
    std::map<uint32_t, Receipt> receipts_;
    std::map<uint32_t, Body> originals_;
    Counters counts_;
    std::atomic<uint64_t> delivered_draws_{0};
    bool installed_ = false;
};

}  // namespace ps2r::app
