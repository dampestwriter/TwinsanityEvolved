#include "games/slus_20909_crash_twinsanity/composition/mesh_producer.h"
#include "engine/ee/timing/tick_profile.h"

#include <array>
#include <stdexcept>
#include <utility>

#include "engine/core/hash/hash.h"
#include "engine/core/hash/sha256.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"

namespace ps2r::app {
namespace {
using Pass = ::title::MeshPass;
using Assets = ::title::MeshAssets;
using Certificate = ::title::MaterialCertificate;
using Family = ::title::GraphicsRefusals::Family;
using Reason = ::title::GraphicsRefusals::Reason;
constexpr uint32_t Physical(uint32_t address) { return address & 0x1fffffff; }
// Refusal details of the Meshes family (owns_material / texture / reserve), on
// the producer that asked (FlushMaterial for a reservation, the model boundary
// for a collection decline). Documented local codes (section 8a).
enum Detail : uint32_t {
    kNoReceipt = 1, kUnknownMaterial, kUnknownRecord, kUnownedConsumer, kUnverifiedArchive,
    kTextureDisagrees, kBucketDeclined, kTextureMissing, kTextureMismatch,
    kEnvironment, kView, kSlot, kReceiptStale
};
}  // namespace

// The per-request slot the ordered output holds: the frame captured at the
// reservation, the draws the pass records synchronously after it, the texture
// View pinned for the whole frame. Delivered on the GS consumer; nothing is read
// from guest memory there.
struct MeshProducer::Slot {
    Frame frame;
    ::title::TextureAssets::View view;
    std::vector<GraphicsProducerDraws::AssetDraw> draws;
    std::atomic<bool> filled{false};
};

MeshProducer::MeshProducer(bool enabled, Services services) : enabled_(enabled), services_(std::move(services)) {
    if (!services_.read || !services_.write || !services_.texture || !services_.view || !services_.environment ||
        !services_.queue || !services_.draw)
        throw std::invalid_argument("Mesh producer requires every service");
    Pass::Hooks hooks;
    hooks.enabled = enabled_;
    hooks.read = services_.read;
    hooks.write = services_.write;
    hooks.model = [this](uint32_t object) { return Model(object); };
    hooks.texture = [this](uint32_t object, ::title::MeshMaterial::Texture& out) { return Texture(object, out); };
    hooks.environment = [this](Environment& out) {
        Frame frame;
        return services_.environment(2, frame, out);   // the admission is the same for every material bucket
    };
    hooks.owns_material = [this](uint32_t material) { return OwnsMaterial(material); };
    hooks.reserve = [this](const auto& requests, auto& records) { return Reserve(requests, records); };
    hooks.frame_admits = [this] { return FrameAdmits(); };
    pass_ = std::make_unique<Pass>(std::move(hooks));
}
MeshProducer::~MeshProducer() = default;

bool MeshProducer::Install(const Capture& capture, const Installer& install) {
    if (installed_ || !capture || !install) return false;
    if (!pass_->Install(capture, install)) return false;
    constexpr uint32_t observers[3] = {kModelRead, kModelDestroy, kMaterialResourceRead};
    for (auto address : observers) {
        auto original = capture(address);
        if (!original) return false;
        originals_[address] = std::move(original);
    }
    if (!install(kModelRead, [this](CpuContext& c, uint32_t at) { ObserveModelRead(c, at); }) ||
        !install(kModelDestroy, [this](CpuContext& c, uint32_t at) { ObserveModelDestroy(c, at); }) ||
        !install(kMaterialResourceRead, [this](CpuContext& c, uint32_t at) { ObserveMaterialRead(c, at); }))
        return false;
    installed_ = true;
    return true;
}

// ModelRead (0x1bcb58): after the original completes, convert the model's
// parts through the DiskManager's software allocation table into an immutable
// asset (MeshAssets::ModelLoaded). A decline leaves the model to the writers
// (every instance of it refuses at collection: `model` returns null).
void MeshProducer::ObserveModelRead(CpuContext& context, uint32_t entry) {
    const bool full = !entry || entry == kModelRead;
    const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
    originals_.at(kModelRead)(context, entry);
    if (!full || !enabled_) return;
    ++counts_.model_reads;
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Mesh);
    switch (assets_.ModelLoaded(services_.read, object)) {
        case Assets::Result::Ok: ++counts_.models_loaded; break;
        case Assets::Result::InvalidMemory: ++counts_.models_invalid; break;
        case Assets::Result::UnsupportedAsset: ++counts_.models_unsupported; break;
        case Assets::Result::LimitExceeded: ++counts_.models_limit; break;
    }
}
// ModelDestroy (0x1c0b08): forget BEFORE the source destroys; a lease a
// collected block holds keeps the previous owned version alive.
void MeshProducer::ObserveModelDestroy(CpuContext& context, uint32_t entry) {
    const bool full = !entry || entry == kModelDestroy;
    if (full && enabled_) {
        ++counts_.model_destroys;
        ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Mesh);
        assets_.ForgetModel(context.cpuRegs.GPR.r[4].UL[0]);
    }
    originals_.at(kModelDestroy)(context, entry);
}
// MaterialResourceRead (0x1c0670; a0 resource, a1 Stream): the complete
// serialized material record is the bytes the original consumes from the
// Stream (position +12 before/after; the record occupies its section item
// exactly: material_records.py `finish`). Its SHA-256 is the receipt the
// MaterialCertificate is consulted with for the Material* the original
// publishes at resource+8 (the resource id at +4). A read whose stream cannot
// be followed leaves no receipt: every instance of that material declines.
void MeshProducer::ObserveMaterialRead(CpuContext& context, uint32_t entry) {
    const bool full = !entry || entry == kMaterialResourceRead;
    const uint32_t resource = context.cpuRegs.GPR.r[4].UL[0], stream = context.cpuRegs.GPR.r[5].UL[0];
    uint32_t vtable = 0, before = 0, begin = 0, length = 0;
    // The title's memory Stream (TextureBindings::kMemoryStream: vtable, +8 begin,
    // +12 position, +16 length); any other reader leaves no receipt.
    const bool followed = full && enabled_ && !(stream & 3) && Value(stream, vtable) &&
                          vtable == ::title::TextureBindings::kMemoryStream &&
                          Value(stream + 8, begin) && Value(stream + 12, before) &&
                          Value(stream + 16, length) && before >= begin && before - begin <= length;
    originals_.at(kMaterialResourceRead)(context, entry);
    if (!full || !enabled_) return;
    ++counts_.material_reads;
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Mesh);
    uint32_t after = 0, material = 0, id = 0;
    std::vector<uint8_t> record;
    if (!Value(resource + 8, material)) material = 0;
    if (followed && material && Value(stream + 12, after) && after > before && after - before >= 16 &&
        after - before <= 4096 && after - begin <= length && Value(resource + 4, id)) {
        record.resize(after - before);
        if (services_.read(before, record.data(), record.size())) {
            receipts_[Physical(material)] = {id, hash::Sha256::HexOf(record.data(), record.size()), ShaderHash(material)};
            ++counts_.receipts;
            return;
        }
    }
    ++counts_.unhashed;
    if (material) receipts_.erase(Physical(material));
}

// The certificate (whole-source consumer closure) over the live
// receipt, then the two live agreements the certificate asks the root for: the
// material's texture is the entry's, and its bucket is one this owner's
// environment hook certifies (FrameEnvironment::MeshBucket).
bool MeshProducer::OwnsMaterial(uint32_t material) {
    ++counts_.owns_calls;
    const auto refuse = [this](uint64_t& counter, uint32_t detail) {
        ++counter;
        if (services_.refuse) services_.refuse(Reason::UnsupportedState, ::title::MeshIntent::kPlaced, detail);
        return false;
    };
    const auto receipt = receipts_.find(Physical(material));
    if (receipt == receipts_.end()) { ++frame_receipt_refusals_; return refuse(counts_.no_receipt, kNoReceipt); }
    if (receipt->second.shader_hash != ShaderHash(material)) {
        ++frame_receipt_refusals_;
        return refuse(counts_.receipt_stale, kReceiptStale);
    }
    // The archive identity the catalogues were generated from (the same
    // observed identity TextureSelection/TextureConsumers consult; the record
    // digest itself IS the live bytes').
    const auto decision = Certificate::Inspect(::title::TextureConsumers::RetailArchive(), receipt->second.id,
                                               receipt->second.sha256);
    switch (decision.result) {
        case Certificate::Result::Certified: break;
        case Certificate::Result::UnknownMaterial: return refuse(counts_.unknown_material, kUnknownMaterial);
        case Certificate::Result::UnknownRecord: return refuse(counts_.unknown_record, kUnknownRecord);
        case Certificate::Result::UnownedConsumer: return refuse(counts_.unowned_consumer, kUnownedConsumer);
        case Certificate::Result::UnverifiedArchive: return refuse(counts_.unverified_archive, kUnverifiedArchive);
    }
    uint32_t shader = 0, texture = 0, id = 0, bucket = 0;
    if (!Value(material, shader) || !Value(shader + 0x14, texture) || !Value(texture + 4, id) ||
        !decision.entry || id != decision.entry->texture)
        return refuse(counts_.texture_disagrees, kTextureDisagrees);
    if (!Value(material + 0x20, bucket) || !::title::FrameEnvironment::MeshBucket(bucket))
        return refuse(counts_.bucket_declined, kBucketDeclined);
    ++counts_.certified;
    return true;
}

// The Material's one shader's IDENTITY bytes (TR Material: shaders[0] at +0):
// the settings word, the LOD pair and the texture object (+0x00..+0x20) and the
// call mode / vtable (+0x60..+0x70), plus the material's bucket word -- never
// the colour/scroll block (+0x20..+0x60), which AnimateMaterial advances every
// frame for a scrolling material. A reused address or a restored image holding
// another material reads differently; the same material animating does not.
uint64_t MeshProducer::ShaderHash(const ::title::MaterialState::Read& read, uint32_t material) {
    std::array<uint8_t, 0x20 + 0x10 + 4> bytes{};
    uint32_t shader = 0;
    if (!read || !material || !read(material, &shader, sizeof shader) || !shader ||
        !read(shader, bytes.data(), 0x20) || !read(shader + 0x60, bytes.data() + 0x20, 0x10) ||
        !read(material + 0x20, bytes.data() + 0x30, 4)) return 0;
    return hash::Fnv1a64(bytes.data(), bytes.size());
}

// The frame's admission: every material the title's table lists must hold a
// receipt. A restored image lists the materials of its loaded chunks but the
// section-less (or older) image carried no receipts for them: such a frame runs
// whole on the model path -- the owned bucket never shares a frame's material
// list with the writers (the s11 boss resume: 144,000 kNoReceipt refusals next
// to zero replaced boundaries would have been exactly that mixture had any
// material certified). The previous admitted frame is judged here: a receipt
// refusal in a frame that also replaced a boundary is a mixed frame.
bool MeshProducer::FrameAdmits() {
    if (frame_admitted_ && frame_receipt_refusals_ && PassReplaced() != frame_replaced_at_admission_)
        ++counts_.mixed_frames;
    frame_receipt_refusals_ = 0;
    frame_admitted_ = false;
    const auto refuse = [this](uint64_t& counter) {
        ++counter;
        ++counts_.frames_whole_model;
        return false;
    };
    uint32_t entries = 0, vtable = 0;
    uint16_t count = 0, capacity = 0;
    if (!Value(kMaterialTable, entries) || !Value(kMaterialTable + 4, count) || !Value(kMaterialTable + 6, capacity) ||
        !Value(kMaterialTable + 0x14, vtable) || vtable != kMaterialTableVtable || count > capacity ||
        capacity > kMaterialTableCapacity || (count && (!entries || (entries & 3))))
        return refuse(counts_.table_unreadable);
    size_t unreceipted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t item = 0, material = 0;
        if (!Value(entries + i * 8, item)) return refuse(counts_.table_unreadable);
        if (!item) continue;
        if (!Value(item + 8, material)) return refuse(counts_.table_unreadable);
        if (material && !receipts_.count(Physical(material))) ++unreceipted;
    }
    counts_.unreceipted_live = unreceipted;
    if (unreceipted) {
        ++counts_.frames_whole_model;
        return false;
    }
    ++counts_.frames_admitted;
    frame_admitted_ = true;
    frame_replaced_at_admission_ = PassReplaced();
    return true;
}
uint64_t MeshProducer::PassReplaced() const {
    const auto pass = pass_->Stats();
    return pass.rigid.replaced + pass.placed.replaced + pass.direct.replaced + pass.flush.replaced;
}

// MSHR: u32 count, then per receipt (address order) u32 material (physical),
// u32 id, u64 shader identity hash, 32 raw SHA-256 bytes of the record.
namespace {
constexpr size_t kReceiptBytes = 4 + 4 + 8 + 32;
void PutWord(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) out.push_back(uint8_t(value >> (8 * i)));
}
uint64_t GetWord(const uint8_t* at, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(at[i]) << (8 * i);
    return value;
}
bool HexByte(char high, char low, uint8_t& out) {
    const auto nibble = [](char c, uint8_t& n) {
        if (c >= '0' && c <= '9') { n = uint8_t(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { n = uint8_t(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { n = uint8_t(c - 'A' + 10); return true; }
        return false;
    };
    uint8_t h = 0, l = 0;
    if (!nibble(high, h) || !nibble(low, l)) return false;
    out = uint8_t(h << 4 | l);
    return true;
}
}  // namespace
bool MeshProducer::CaptureReceipts(std::vector<uint8_t>& out) const {
    out.clear();
    PutWord(out, receipts_.size(), 4);
    for (const auto& [material, receipt] : receipts_) {
        if (receipt.sha256.size() != 64) return false;
        PutWord(out, material, 4);
        PutWord(out, receipt.id, 4);
        PutWord(out, receipt.shader_hash, 8);
        for (size_t i = 0; i < 32; ++i) {
            uint8_t byte = 0;
            if (!HexByte(receipt.sha256[2 * i], receipt.sha256[2 * i + 1], byte)) return false;
            out.push_back(byte);
        }
    }
    return true;
}
bool MeshProducer::ValidateReceipts(const std::vector<uint8_t>& payload, std::string* error) {
    const auto bad = [error](const char* why) { if (error) *error = std::string("Mesh receipts: ") + why; return false; };
    if (payload.size() < 4) return bad("payload lacks the count");
    const uint64_t count = GetWord(payload.data(), 4);
    if (count > 65536 || payload.size() != 4 + count * kReceiptBytes) return bad("payload size disagrees with the count");
    uint32_t previous = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* at = payload.data() + 4 + i * kReceiptBytes;
        const auto material = uint32_t(GetWord(at, 4));
        if (!material || (material & 3) || material >= 0x02000000 || (i && material <= previous))
            return bad("receipt addresses are not ascending physical material addresses");
        previous = material;
    }
    return true;
}
bool MeshProducer::RestoreReceipts(const std::vector<uint8_t>& payload, const ::title::MaterialState::Read& incoming,
                                   std::string* error) {
    if (!ValidateReceipts(payload, error)) return false;
    if (!incoming) { if (error) *error = "Mesh receipts: no incoming image to verify against"; return false; }
    const uint64_t count = GetWord(payload.data(), 4);
    receipts_.clear();
    std::map<uint32_t, uint32_t> live;
    if (!LiveMaterials(incoming, live)) {
        counts_.receipts_dropped += count;
        return true;   // the image has no readable table: nothing to bind the receipts to
    }
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* at = payload.data() + 4 + i * kReceiptBytes;
        Receipt receipt;
        const auto material = uint32_t(GetWord(at, 4));
        receipt.id = uint32_t(GetWord(at + 4, 4));
        receipt.shader_hash = GetWord(at + 8, 8);
        static constexpr char digits[] = "0123456789abcdef";
        receipt.sha256.reserve(64);
        for (size_t b = 0; b < 32; ++b) {
            receipt.sha256.push_back(digits[at[16 + b] >> 4]);
            receipt.sha256.push_back(digits[at[16 + b] & 15]);
        }
        // The image's material table must still bind the address to a resource of
        // the receipt's id (a destroyed material, or an address reused by another
        // resource, is dropped). The identity hash is re-taken from the image: the
        // title adjusts a shader's identity words after the read for materials no
        // mesh consumer asks about (the capture's hash would make those stale),
        // and the live check keeps watching the address from here on.
        const auto bound = live.find(material);
        const uint64_t identity = ShaderHash(incoming, material);
        if (bound == live.end() || bound->second != receipt.id || !identity) {
            ++counts_.receipts_dropped;
            continue;
        }
        receipt.shader_hash = identity;
        receipts_[material] = std::move(receipt);
        ++counts_.receipts_restored;
    }
    return true;
}
// The image's material table: Material* (physical) -> resource id for every
// listed item (TR GraphicsTable<MaterialKind>); false when the table is not
// the title's (the receipts then restore nothing: every frame stays whole-model).
bool MeshProducer::LiveMaterials(const ::title::MaterialState::Read& read, std::map<uint32_t, uint32_t>& out) {
    const auto value = [&read](uint32_t at, auto& v) { return read && at && read(at, &v, sizeof v); };
    uint32_t entries = 0, vtable = 0;
    uint16_t count = 0, capacity = 0;
    if (!value(kMaterialTable, entries) || !value(kMaterialTable + 4, count) || !value(kMaterialTable + 6, capacity) ||
        !value(kMaterialTable + 0x14, vtable) || vtable != kMaterialTableVtable || count > capacity ||
        capacity > kMaterialTableCapacity || (count && (!entries || (entries & 3)))) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t item = 0, id = 0, material = 0;
        if (!value(entries + i * 8, item) || !value(entries + i * 8 + 4, id)) return false;
        if (!item) continue;
        if (!value(item + 8, material)) return false;
        if (material) out[Physical(material)] = id;
    }
    return true;
}

// The texture owner's published asset (TextureBindings::View) and the
// GameTexture header at object+12 (TR renderer.h Texture: widthPower,
// heightPower, levels, pixelFormat, hasAlpha, function, levelWidths[0] in 64
// pixels) -- the header and the asset must agree on the size and level count.
bool MeshProducer::Texture(uint32_t object, ::title::MeshMaterial::Texture& out) {
    ++counts_.texture_lookups;
    const auto refuse = [this](uint64_t& counter, uint32_t detail) {
        ++counter;
        if (services_.refuse) services_.refuse(Reason::MissingResource, ::title::MeshIntent::kFlushMaterial, detail);
        return false;
    };
    ::title::TextureAssets::View view;
    if (!services_.texture(object, view)) return refuse(counts_.texture_missing, kTextureMissing);
    uint16_t width_power = 0, height_power = 0;
    uint8_t levels = 0, format = 0, alpha = 0, function = 0;
    uint32_t buffer_width = 0;
    if (!Value(object + 16, width_power) || !Value(object + 18, height_power) || !Value(object + 20, levels) ||
        !Value(object + 21, format) || !Value(object + 23, alpha) || !Value(object + 25, function) ||
        !Value(object + 0x38, buffer_width) || width_power > 10 || height_power > 10 ||
        view.image->levels[0].width != (1u << width_power) || view.image->levels[0].height != (1u << height_power) ||
        view.image->levels.size() != levels || buffer_width > 63)
        return refuse(counts_.texture_mismatch, kTextureMismatch);
    out.handle = view.handle;
    out.lease = view.lease;
    out.width = 1u << width_power;
    out.height = 1u << height_power;
    out.buffer_width = buffer_width * 64;
    out.levels = levels;
    out.source_format = format;
    out.function = function;
    out.alpha = alpha != 0;
    return true;
}

// The model asset converted at ModelRead; a missing one (a restored session, a
// read this owner was not yet installed for) converts here from the still-
// loaded DiskManager blocks. Null leaves the instance to the writers.
::title::MeshAssets::ModelLease MeshProducer::Model(uint32_t object) {
    auto model = assets_.FindModel(object);
    if (model || !enabled_) return model;
    ++counts_.lazy_loads;
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Mesh);
    if (assets_.ModelLoaded(services_.read, object) == Assets::Result::Ok) return assets_.FindModel(object);
    ++counts_.lazy_misses;
    return {};
}

// FlushMaterial's reservation: the material's bucket slot of the ordered
// output, captured frame + pinned texture View per request, all validated
// before any slot is taken (false leaves the root unchanged: the pass refuses
// the material to the writer). The record fills the slot synchronously
// (MeshPass::Deliver follows Reserve on the game thread); the slot delivers
// on the ordered output after the Link, refusing (kUnfilled) if it never was.
bool MeshProducer::Reserve(const std::vector<Pass::Request>& requests, std::vector<Pass::Record>& records) {
    const auto refuse = [this](uint64_t& counter, uint32_t detail) {
        ++counter;
        if (services_.refuse) services_.refuse(Reason::Ordering, ::title::MeshIntent::kFlushMaterial, detail);
        return false;
    };
    std::vector<std::shared_ptr<Slot>> slots;
    for (const auto& request : requests) {
        ++counts_.requests;
        auto slot = std::make_shared<Slot>();
        Environment environment;
        if (!services_.environment(request.shader.bucket, slot->frame, environment))
            return refuse(counts_.refused_environment, kEnvironment);
        if (!request.shader.texture.lease || !services_.view(request.shader.texture.lease, slot->view) ||
            !slot->view.image || slot->view.image->levels.empty())
            return refuse(counts_.refused_view, kView);
        slots.push_back(std::move(slot));
    }
    // Every request validated: take the slots. The ordered output's refusal of a
    // reservation (no predecessor, stopped) is a refusal before commit for the
    // first; a later one after an accepted slot cannot be undone -- the pass
    // only ever reserves one material per call (FlushMaterial), so the loop is
    // all-or-nothing in practice and a mixed outcome is a fault here.
    size_t taken = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& slot = slots[i];
        const auto work = [this, slot](uint32_t& why) {
            if (!slot->filled.load(std::memory_order_acquire)) {
                why = kUnfilled;
                return false;
            }
            return services_.draw(slot->frame, slot->draws, &why);
        };
        if (!services_.queue(requests[i].shader.bucket, work)) {
            if (taken) {
                if (services_.fault) services_.fault(Reason::Ordering, ::title::MeshIntent::kFlushMaterial, kSlot);
                throw std::runtime_error("Mesh reservation refused after an accepted slot");
            }
            return refuse(counts_.refused_slot, kSlot);
        }
        ++taken;
    }
    for (size_t i = 0; i < requests.size(); ++i) {
        ++counts_.reserved;
        records.push_back([this, slot = slots[i]](const std::vector<ps2r::gs::DrawIr>& draws) {
            if (slot->filled.load(std::memory_order_acquire)) return false; // one record per reservation
            slot->draws.reserve(draws.size());
            for (const auto& draw : draws) slot->draws.push_back({draw, slot->view});
            delivered_draws_.fetch_add(draws.size(), std::memory_order_relaxed);
            slot->filled.store(true, std::memory_order_release);
            return true;
        });
    }
    return true;
}

MeshProducer::Counters MeshProducer::Counts() const {
    auto counts = counts_;
    counts.pass = pass_->Stats();
    counts.delivered_draws = delivered_draws_.load();
    counts.receipts_live = receipts_.size();
    counts.models_live = assets_.Count();
    return counts;
}
bool MeshProducer::CanCheckpoint(std::string* reason) const {
    if (pass_->Stats().faults) {
        if (reason) *reason = "Mesh producer has faulted selected work";
        return false;
    }
    return true;
}
void MeshProducer::ResetAfterQuiescence() { pass_->Reset(); }

}  // namespace ps2r::app
