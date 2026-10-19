// platform/nativeee (host side) -- io.h: Platform::Io. The I/O processor does not exist natively (the play engine has
// iop_linked=0: every IOP module's intent is served by a host service), so Initialise/Restart return at once,
// LoadDriver returns a positive module id, and the IOP heap -- which the game sizes its sound streams' buffers against
// in IOP bytes -- is a budget of opaque addresses (never guest memory: an IOP address means nothing to the EE side
// and the game only passes it back to Platform::Stream::AttachBuffer / Platform::Audio).
#include <cstdio>

#include "ps2e_seam.h"
#include "savestate.h"
#include "te_services.h"

namespace te {

void IoHeap::Serialize(std::vector<uint8_t>& out) const {
    savestate::Put(out, uint32_t(1));
    savestate::Put(out, next_);
    savestate::Put(out, drivers_);
    savestate::Put(out, uint32_t(blocks_.size()));
    for (const Block& b : blocks_) { savestate::Put(out, b.address); savestate::Put(out, b.size); }
}

bool IoHeap::Restore(const savestate::Blob& blob, std::string* err) {
    savestate::PayloadReader in(blob.payload);
    uint32_t version = 0, count = 0;
    auto fail = [&](const char* why) { if (err) *err = std::string("te::IoHeap: ") + why; return false; };
    std::vector<Block> blocks;
    uint32_t next = 0;
    int32_t drivers = 0;
    if (!in.Get(&version) || version != 1 || !in.Get(&next) || !in.Get(&drivers) || !in.Get(&count) || count > 4096)
        return fail("payload");
    for (uint32_t k = 0; k < count; ++k) {
        Block b{};
        if (!in.Get(&b.address) || !in.Get(&b.size)) return fail("payload truncated");
        blocks.push_back(b);
    }
    if (next < kBase || next - kBase > kSize) return fail("heap break out of range");
    blocks_ = std::move(blocks);
    next_ = next;
    drivers_ = drivers;
    return true;
}

uint32_t IoHeap::Allocate(int32_t size) {
    if (size <= 0) return 0;
    const uint32_t bytes = (static_cast<uint32_t>(size) + 15u) & ~15u;
    if (next_ - kBase + bytes > kSize) return 0;
    const uint32_t address = next_;
    next_ += bytes;
    blocks_.push_back({address, bytes});
    return address;
}

int32_t IoHeap::Free(uint32_t address) {
    for (size_t k = 0; k < blocks_.size(); ++k) {
        if (blocks_[k].address != address) continue;
        // The top block gives its bytes back; an interior one is forgotten (the retail IOP heap was a bump too)
        if (k + 1 == blocks_.size()) next_ = address;
        blocks_.erase(blocks_.begin() + static_cast<long>(k));
        return 0;
    }
    return -1;
}

uint32_t IoHeap::Free() const { return kSize - (next_ - kBase); }

void IoHeap::Reset() { blocks_.clear(); next_ = kBase; }

int32_t IoHeap::LoadDriver(const char* path) {
    ++drivers_;
    if (GetSettings().verbose) std::fprintf(stderr, "[twinsanityevolved] io: LoadDriver(%s) -> %d (native)\n", path ? path : "", drivers_);
    return drivers_;
}

}  // namespace te

extern "C" {

void ps2e_io_initialise(void) {}
void ps2e_io_restart(ps2e_u32 image) { (void)image; }   // the IOPRP image: nothing to reboot natively
ps2e_s32 ps2e_io_load_driver(ps2e_u32 path) { return te::Services::Get().io().LoadDriver(te::GuestString(path, 256)); }
void ps2e_io_initialise_heap(void) { te::Services::Get().io().Reset(); }
ps2e_u32 ps2e_io_allocate_heap(ps2e_s32 size) { return te::Services::Get().io().Allocate(size); }
ps2e_s32 ps2e_io_free_heap(ps2e_u32 address) { return te::Services::Get().io().Free(address); }

}  // extern "C"
