// platform/nativeee (host side) -- files.h: Platform::Files over the engine's title::Files (the synchronous disc
// descriptors over IsoFs). The game's buffers are guest addresses: title::Files::Read(handle, uint32_t destination,
// bytes) already takes one (FIT_MEMORY row 1), through a GuestMemory adapter over the module's linear memory. Paths
// are made the way the PS2 arm made them ("cdrom0:\\CRASH6\\CRASH.BH;1": upper case, backslashes, the version), so
// the descriptor table's recorded paths read the same as the recomp's.
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"
#include "ps2e_seam.h"
#include "savestate.h"
#include "te_services.h"

namespace te {

struct Files::Memory final : ps2r::ipu::GuestMemory {
    uint8_t* Translate(uint32_t addr, size_t len) override { return GuestPointer(addr, len); }
    uint32_t Read32(uint32_t addr) override {
        uint32_t v = 0;
        if (uint8_t* p = GuestPointer(addr, 4)) std::memcpy(&v, p, 4);
        return v;
    }
    void Write32(uint32_t addr, uint32_t value) override {
        if (uint8_t* p = GuestPointer(addr, 4)) std::memcpy(p, &value, 4);
    }
};

namespace {
// The PS2 arm's Platform::Files::Open path: "cdrom0:\" + the path upper-cased with '/' -> '\' + ";1", 256 bytes at most.
std::string DevicePath(const char* path) {
    constexpr size_t kMax = 256;
    std::string out = "cdrom0:\\";
    for (const char* c = path; c && *c != '\0' && out.size() < kMax - 3; ++c) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        else if (ch == '/') ch = '\\';
        out.push_back(ch);
    }
    out += ";1";
    return out;
}
}  // namespace

Files::Files(Disc& disc) : memory_(new Memory) {
    disc.Initialise();   // the image must be open before title::Files reads its root (a failed open leaves it empty)
    core_.reset(new title::Files(*memory_, disc.Iso()));
    // The descriptor table (title::Files::Encode: media identity + {handle, location, size, position, path}); the
    // engine's Restore validates each descriptor against the disc that is open now.
    savestate::Register(savestate::kTagFiles, "te::Files", false,
        [](std::vector<uint8_t>& out) {
            std::string error;
            if (!Services::Get().files().Core().Encode(out, &error))
                std::fprintf(stderr, "[twinsanityevolved] files: state not captured: %s\n", error.c_str());
        },
        [](const savestate::Blob& blob, std::string* err) {
            title::Files& core = Services::Get().files().Core();
            title::Files::State state;
            std::string error;
            if (!core.Decode(blob.payload, state, &error) || !core.Restore(state, &error)) {
                if (err) *err = "te::Files: " + error;
                return false;
            }
            return true;
        });
}
Files::~Files() = default;

void Files::Reset() { core_->Reset(); }

int32_t Files::Open(const char* path, int32_t flags) {
    if (!path) return -22;
    return core_->Open(DevicePath(path), flags);
}

int32_t Files::Close(int32_t file) { return core_->Close(file); }

int32_t Files::Read(int32_t file, uint32_t buffer, int32_t size) { return core_->Read(file, buffer, size); }

int32_t Files::Write(int32_t file, uint32_t, int32_t) { return core_->Write(file); }   // refused: the disc is read-only

int32_t Files::Seek(int32_t file, int32_t offset, int32_t whence) {
    return core_->Seek(file, offset, static_cast<uint32_t>(whence));
}

}  // namespace te

extern "C" {

void ps2e_files_reset(void) { te::Services::Get().files().Reset(); }

ps2e_s32 ps2e_files_open(ps2e_u32 path, ps2e_s32 flags) {
    const char* s = te::GuestString(path, 1024);
    if (!s) return -22;
    return te::Services::Get().files().Open(s, flags);
}

ps2e_s32 ps2e_files_close(ps2e_s32 file) { return te::Services::Get().files().Close(file); }

ps2e_s32 ps2e_files_read(ps2e_s32 file, ps2e_u32 buffer, ps2e_s32 size) {
    return te::Services::Get().files().Read(file, buffer, size);
}

ps2e_s32 ps2e_files_write(ps2e_s32 file, ps2e_u32 buffer, ps2e_s32 size) {
    return te::Services::Get().files().Write(file, buffer, size);
}

ps2e_s32 ps2e_files_seek(ps2e_s32 file, ps2e_s32 offset, ps2e_s32 whence) {
    return te::Services::Get().files().Seek(file, offset, whence);
}

}  // extern "C"
