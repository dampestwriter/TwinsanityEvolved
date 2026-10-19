// platform/nativeee (host side) -- saves.h: Platform::Saves over the engine's title::SaveStorage (one operation at a
// time, advanced on the logical frame boundary, the memory card a host directory <mc_dir>/mc<port>). The operation
// numbers, Result, StorageInfo and the 64-byte FileEntry are bit-identical between the header and the engine
// (FIT_MEMORY row 5). This is the engine's SaveBindings adapter with the CpuContext removed: the game's output
// pointers (kilobytes, size, entry, read buffer) are guest addresses kept until the completion is published, inside
// the Update call that finishes the operation, so the game never observes a completion before its outputs.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <filesystem>

#include "games/slus_20909_crash_twinsanity/hle/saves/save_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_mods.h"
#include "te_services.h"

namespace te {

namespace {
using Storage = title::SaveStorage;

bool String(uint32_t address, size_t max, std::string& out) {
    out.clear();
    const char* s = GuestString(address, max + 1);
    if (!s) return false;
    const size_t n = std::strlen(s);
    if (n > max) return false;
    out.assign(s, n);
    return true;
}
bool WriteGuest(uint32_t address, const void* bytes, size_t size) {
    uint8_t* p = GuestPointer(address, size);
    if (!p) return false;
    std::memcpy(p, bytes, size);
    return true;
}
}  // namespace

Saves::Saves(Clock& clock, const Settings& settings) : mc_dir_(settings.mc_dir), clock_(clock) {
    Storage::Config config;
    config.directory = settings.mc_dir;
    // Port 0 slot 0 is the one card the game looks at first; the second port is present too (two cards), the
    // multitap slots are not.
    config.present = {true, false, true, false};
    core_.reset(new Storage(config));
    // title::SaveStateCodec over a SaveBindings::Snapshot (the storage's snapshot + the game's output addresses).
    savestate::Register(savestate::kTagSaves, "te::Saves", false,
        [](std::vector<uint8_t>& out) { Services::Get().saves().Serialize(out); },
        [](const savestate::Blob& blob, std::string* err) { return Services::Get().saves().Restore(blob, err); });
}
Saves::~Saves() = default;

std::string Saves::CardDirectory() const { return use_mods_ ? mc_dir_ + "-mods" : mc_dir_; }

// the card follows the mods' switch between operations. The -mods card is made on first use: a copy of the vanilla
// card's folder when there is one (the player's progress carries into modded play; the vanilla files are never written
// again while a mod is on), else an empty card the game formats as it would a new one.
void Saves::SyncCard() {
    const bool want = mods::AnyEnabled();
    if (want == use_mods_) return;
    if (core_->Busy() || (mods_core_ && mods_core_->Busy())) return;   // between operations only
    if (want && !mods_core_) {
        if (!mc_dir_.empty()) {
            std::error_code ec;
            const std::filesystem::path vanilla(mc_dir_), mods(mc_dir_ + "-mods");
            if (!std::filesystem::exists(mods, ec) && std::filesystem::is_directory(vanilla, ec)) {
                std::filesystem::copy(vanilla, mods, std::filesystem::copy_options::recursive, ec);
                std::fprintf(stderr, "[twinsanityevolved] saves: the -mods card %s seeded from %s%s\n", mods.string().c_str(), vanilla.string().c_str(),
                             ec ? (" (copy failed: " + ec.message() + ")").c_str() : "");
            }
        }
        Storage::Config config;
        config.directory = mc_dir_.empty() ? std::string() : mc_dir_ + "-mods";
        config.present = {true, false, true, false};
        mods_core_.reset(new Storage(config));
    }
    use_mods_ = want;
    if (use_mods_ && started_ && !mods_started_) {
        mods_started_ = mods_core_->Start();
        if (mods_started_ && !region_.empty()) mods_core_->Initialise(region_, product_);
    }
    outputs_ = {};
    std::fprintf(stderr, "[twinsanityevolved] saves: memory card -> %s (%s)\n", CardDirectory().c_str(), use_mods_ ? "the -mods card: a mod is on" : "the vanilla card");
}

title::SaveStorage* Saves::Active() {
    SyncCard();
    return use_mods_ && mods_core_ ? mods_core_.get() : core_.get();
}

namespace {
// One storage's codec bytes (empty when a mutation is in flight: SaveStorage::CanCheckpoint -- the loaded game then sees the
// operation never started and re-issues it; logged so it can be attributed)
std::vector<uint8_t> EncodeStorage(const Storage& storage, const char* which, uint32_t value, uint32_t entry, uint32_t data, uint32_t size) {
    std::vector<uint8_t> codec;
    if (auto captured = storage.Capture()) {
        title::SaveBindings::Snapshot snapshot;
        snapshot.storage = *captured;
        snapshot.outputs.value = value;
        snapshot.outputs.entry = entry;
        snapshot.outputs.data = data;
        snapshot.outputs.size = size;
        std::string error;
        if (!title::SaveStateCodec::Encode(snapshot, codec, &error))
            std::fprintf(stderr, "[twinsanityevolved] saves: %s state not captured: %s\n", which, error.c_str());
    } else if (storage.Busy()) {
        std::fprintf(stderr, "[twinsanityevolved] saves: an operation is in flight on the %s card at the checkpoint -- not captured\n", which);
    }
    return codec;
}
}  // namespace

void Saves::Serialize(std::vector<uint8_t>& out) const {
    // v2: the vanilla card's snapshot, then the card in use and the -mods card's snapshot (empty when never made)
    savestate::Put(out, uint32_t(2));
    savestate::Put(out, uint8_t(started_ ? 1 : 0));
    const std::vector<uint8_t> codec = EncodeStorage(*core_, "vanilla", use_mods_ ? 0 : outputs_.value, use_mods_ ? 0 : outputs_.entry,
                                                     use_mods_ ? 0 : outputs_.data, use_mods_ ? 0 : outputs_.size);
    savestate::Put(out, uint32_t(codec.size()));
    savestate::PutBytes(out, codec.data(), codec.size());
    savestate::Put(out, uint8_t(use_mods_ ? 1 : 0));
    savestate::Put(out, uint8_t(mods_started_ ? 1 : 0));
    std::vector<uint8_t> mods_codec;
    if (mods_core_) mods_codec = EncodeStorage(*mods_core_, "-mods", use_mods_ ? outputs_.value : 0, use_mods_ ? outputs_.entry : 0,
                                               use_mods_ ? outputs_.data : 0, use_mods_ ? outputs_.size : 0);
    savestate::Put(out, uint32_t(mods_codec.size()));
    savestate::PutBytes(out, mods_codec.data(), mods_codec.size());
}

bool Saves::Restore(const savestate::Blob& blob, std::string* err) {
    savestate::PayloadReader in(blob.payload);
    uint32_t version = 0;
    uint8_t started = 0;
    std::vector<uint8_t> codec;
    auto fail = [&](const std::string& why) { if (err) *err = "te::Saves: " + why; return false; };
    if (!in.Get(&version) || (version != 1 && version != 2) || !in.Get(&started) ||
        !savestate::GetVector(in, codec, title::SaveStateCodec::kMaxPayloadBytes))
        return fail("payload");
    if (started && !started_) started_ = core_->Start();
    uint8_t use_mods = 0, mods_started = 0;
    std::vector<uint8_t> mods_codec;
    if (version == 2) {
        if (!in.Get(&use_mods) || !in.Get(&mods_started) || !savestate::GetVector(in, mods_codec, title::SaveStateCodec::kMaxPayloadBytes))
            return fail("payload (v2)");
    }
    outputs_ = {};
    if (!codec.empty()) {   // empty: nothing was captured (idle before Initialise, or a mutation in flight)
        title::SaveBindings::Snapshot snapshot;
        std::string error;
        if (!title::SaveStateCodec::Decode(codec, snapshot, &error)) return fail(error);
        if (!core_->Restore(snapshot.storage, snapshot.storage.last_boundary)) return fail("title::SaveStorage rejected the snapshot");
        region_ = snapshot.storage.region;
        product_ = snapshot.storage.product;
        if (!use_mods) {
            outputs_.value = snapshot.outputs.value;
            outputs_.entry = snapshot.outputs.entry;
            outputs_.data = snapshot.outputs.data;
            outputs_.size = snapshot.outputs.size;
        }
    }
    // the -mods card as it was (the mods' switches come from the settings, not the state: the card follows them at the
    // next operation)
    use_mods_ = use_mods != 0;
    if (use_mods_ || mods_started || !mods_codec.empty()) {
        if (!mods_core_) {
            Storage::Config config;
            config.directory = mc_dir_.empty() ? std::string() : mc_dir_ + "-mods";
            config.present = {true, false, true, false};
            mods_core_.reset(new Storage(config));
        }
        if (mods_started && !mods_started_) mods_started_ = mods_core_->Start();
        if (!mods_codec.empty()) {
            title::SaveBindings::Snapshot snapshot;
            std::string error;
            if (!title::SaveStateCodec::Decode(mods_codec, snapshot, &error)) return fail(error);
            if (!mods_core_->Restore(snapshot.storage, snapshot.storage.last_boundary)) return fail("title::SaveStorage (-mods) rejected the snapshot");
            if (use_mods_) {
                outputs_.value = snapshot.outputs.value;
                outputs_.entry = snapshot.outputs.entry;
                outputs_.data = snapshot.outputs.data;
                outputs_.size = snapshot.outputs.size;
            }
        }
    }
    return true;
}

int32_t Saves::Initialise(uint32_t region, uint32_t product) {
    std::string r, p;
    if (!String(region, 7, r) || !String(product, 15, p)) return 0;
    region_ = r;
    product_ = p;
    if (!started_) started_ = core_->Start();   // mcInit: the card roots
    if (!started_) return 0;
    const bool ok = core_->Initialise(r, p);
    if (mods_core_) {   // the -mods card follows (made before Initialise: a mod on from the settings)
        if (!mods_started_) mods_started_ = mods_core_->Start();
        if (mods_started_) mods_core_->Initialise(r, p);
    }
    return ok ? 1 : 0;
}

void Saves::Publish() {
    auto completion = Active()->TakeCompletion();
    if (!completion) return;
    if (GetSettings().verbose)
        std::fprintf(stderr, "[twinsanityevolved] saves:   completed: succeeded %d step %d error %d (%zu bytes)\n",
                     completion->result.succeeded, completion->result.step, completion->result.error, completion->bytes.size());
    if (completion->result.succeeded == 1) {
        if (outputs_.data && !completion->bytes.empty())
            WriteGuest(outputs_.data, completion->bytes.data(), std::min<size_t>(completion->bytes.size(), outputs_.size));
        if (outputs_.value) WriteGuest(outputs_.value, &completion->value, sizeof(completion->value));
        if (outputs_.entry && completion->entry_present)
            WriteGuest(outputs_.entry, &completion->entry, sizeof(completion->entry));
    }
    outputs_ = {};
}

int32_t Saves::Update(int32_t port, int32_t slot, uint32_t info) {
    Storage* card = Active();
    const auto operation = card->Update(unsigned(port), unsigned(slot), clock_.BoundaryCount());
    Publish();
    if (info) {
        const Storage::Info i = card->StorageInfo(unsigned(port), unsigned(slot));
        const int32_t words[3] = {i.type, i.free_kilobytes, i.formatted};
        WriteGuest(info, words, sizeof(words));   // Platform::Saves::StorageInfo: exactly 12 bytes
    }
    return static_cast<int32_t>(operation);
}

void Saves::GetResult(uint32_t out, int32_t operation) {
    const Storage::Result r = Active()->GetResult(static_cast<Storage::Operation>(operation));
    const int32_t words[3] = {r.succeeded, r.step, r.error};
    WriteGuest(out, words, sizeof(words));
}

namespace {
const char* OperationName(Storage::Operation op) {
    switch (op) {
    case Storage::Operation::Format: return "Format";
    case Storage::Operation::Create: return "Create";
    case Storage::Operation::Measure: return "Measure";
    case Storage::Operation::Find: return "Find";
    case Storage::Operation::Write: return "Write";
    case Storage::Operation::Read: return "Read";
    default: return "?";
    }
}
int32_t SubmitRequest(Storage& storage, Storage::Request request, uint64_t boundary, bool valid) {
    if (GetSettings().verbose)
        std::fprintf(stderr, "[twinsanityevolved] saves: %s mc%u/%u save '%s' file '%s' %u bytes @ boundary %llu%s\n",
                     OperationName(request.operation), request.port, request.slot, request.save.c_str(), request.file.c_str(),
                     request.size, (unsigned long long)boundary, valid ? "" : " (INVALID arguments: refused)");
    if (!valid) return 0;
    const bool ok = storage.Submit(std::move(request), boundary);
    if (!ok && GetSettings().verbose)
        std::fprintf(stderr, "[twinsanityevolved] saves:   refused by the storage (error %d)\n", storage.LastSubmitError());
    return ok ? 1 : 0;
}
}  // namespace

int32_t Saves::Format(int32_t port, int32_t slot) {
    Storage::Request request;
    request.operation = Storage::Operation::Format;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), true);
    if (ok) outputs_ = {};
    return ok;
}

int32_t Saves::CreateSave(int32_t port, int32_t slot, uint32_t save) {
    Storage::Request request;
    request.operation = Storage::Operation::Create;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const bool valid = String(save, 31, request.save);
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), valid);
    if (ok) outputs_ = {};
    return ok;
}

int32_t Saves::MeasureSave(int32_t port, int32_t slot, uint32_t save, uint32_t kilobytes) {
    Storage::Request request;
    request.operation = Storage::Operation::Measure;
    request.port = unsigned(port); request.slot = unsigned(slot);
    const bool valid = String(save, 31, request.save) && GuestPointer(kilobytes, 4) != nullptr;
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), valid);
    if (ok) { outputs_ = {}; outputs_.value = kilobytes; }
    return ok;
}

int32_t Saves::FindFile(int32_t port, int32_t slot, uint32_t file, uint32_t save, uint32_t size, uint32_t entry) {
    Storage::Request request;
    request.operation = Storage::Operation::Find;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = String(file, 31, request.file) && String(save, 31, request.save);
    valid = valid && (!size || GuestPointer(size, 4)) && (!entry || GuestPointer(entry, sizeof(Storage::FileEntry)));
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), valid);
    if (ok) { outputs_ = {}; outputs_.value = size; outputs_.entry = entry; }
    return ok;
}

int32_t Saves::Write(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t data, int32_t size) {
    Storage::Request request;
    request.operation = Storage::Operation::Write;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = size >= 0 && String(save, 31, request.save) && String(file, 31, request.file);
    request.size = static_cast<uint32_t>(size < 0 ? 0 : size);
    valid = valid && request.size <= Storage::kMaxBytes;
    if (valid && request.size) {
        const uint8_t* p = GuestPointer(data, request.size);
        valid = p != nullptr;
        if (valid) request.bytes.assign(p, p + request.size);
    }
    const std::string save_name = request.save, file_name = request.file;
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), valid);
    if (ok) {
        outputs_ = {};
        mods::OnSave(save_name, file_name);   // the save hook (the card the bytes go to is Active()'s)
    }
    return ok;
}

int32_t Saves::Read(int32_t port, int32_t slot, uint32_t save, uint32_t file, uint32_t buffer, int32_t size) {
    Storage::Request request;
    request.operation = Storage::Operation::Read;
    request.port = unsigned(port); request.slot = unsigned(slot);
    bool valid = size >= 0 && String(save, 31, request.save) && String(file, 31, request.file);
    request.size = static_cast<uint32_t>(size < 0 ? 0 : size);
    valid = valid && request.size <= Storage::kMaxBytes && (!request.size || GuestPointer(buffer, request.size));
    const std::string save_name = request.save, file_name = request.file;
    const uint32_t read_size = request.size;
    const int32_t ok = SubmitRequest(*Active(), std::move(request), clock_.BoundaryCount(), valid);
    if (ok) {
        outputs_ = {};
        outputs_.data = buffer;
        outputs_.size = read_size;
        mods::OnLoad(save_name, file_name);   // the load hook
    }
    return ok;
}

int32_t Saves::FileKilobytes(int32_t size) {
    return static_cast<int32_t>(Storage::FileKilobytes(static_cast<uint64_t>(static_cast<uint32_t>(size))));
}

int32_t Saves::SaveBytes(int32_t kilobytes, int32_t files) {
    return static_cast<int32_t>(Storage::SaveBytes(static_cast<uint32_t>(kilobytes), static_cast<uint32_t>(files)));
}

}  // namespace te

extern "C" {

ps2e_s32 ps2e_saves_initialise(ps2e_u32 region, ps2e_u32 product) { return te::Services::Get().saves().Initialise(region, product); }
ps2e_s32 ps2e_saves_update(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 info) { return te::Services::Get().saves().Update(port, slot, info); }
void ps2e_saves_get_result(ps2e_u32 out, ps2e_s32 operation) { te::Services::Get().saves().GetResult(out, operation); }
ps2e_s32 ps2e_saves_format(ps2e_s32 port, ps2e_s32 slot) { return te::Services::Get().saves().Format(port, slot); }
ps2e_s32 ps2e_saves_create_save(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save) { return te::Services::Get().saves().CreateSave(port, slot, save); }
ps2e_s32 ps2e_saves_measure_save(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 kilobytes) { return te::Services::Get().saves().MeasureSave(port, slot, save, kilobytes); }
ps2e_s32 ps2e_saves_find_file(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 file, ps2e_u32 save, ps2e_u32 size, ps2e_u32 entry) { return te::Services::Get().saves().FindFile(port, slot, file, save, size, entry); }
ps2e_s32 ps2e_saves_write(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 file, ps2e_u32 data, ps2e_s32 size) { return te::Services::Get().saves().Write(port, slot, save, file, data, size); }
ps2e_s32 ps2e_saves_read(ps2e_s32 port, ps2e_s32 slot, ps2e_u32 save, ps2e_u32 file, ps2e_u32 buffer, ps2e_s32 size) { return te::Services::Get().saves().Read(port, slot, save, file, buffer, size); }
ps2e_s32 ps2e_saves_file_kilobytes(ps2e_s32 size) { return te::Saves::FileKilobytes(size); }
ps2e_s32 ps2e_saves_save_bytes(ps2e_s32 kilobytes, ps2e_s32 files) { return te::Saves::SaveBytes(kilobytes, files); }

}  // extern "C"
