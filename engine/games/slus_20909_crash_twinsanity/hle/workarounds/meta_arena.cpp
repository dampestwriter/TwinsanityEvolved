#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

#include <cstring>

#include "engine/core/savestate/payload_reader.h"
#include "engine/ee/mem/address_map.h"

namespace {
void Put32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
void Put64(std::vector<uint8_t>& v, uint64_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 8); }

}  // namespace

using Cursor = ps2r::savestate::PayloadReader;

MetaArena::MetaArena() : bytes_(kSize, 0), state_(kCapacity, 0) {}

void MetaArena::Register(AddressMap& map) { map.RegisterExtraRegion(kBase, kSize, bytes_.data(), "META"); }

bool MetaArena::RoutesAllocation(uint32_t size, uint32_t ra) {
    ra &= 0x1FFFFFFFu;
    return size != 0 && size <= kPayloadMax && ra >= kPoolCodeLo && ra < kPoolCodeHi;
}

bool MetaArena::Contains(uint32_t guest_addr) {
    const uint32_t p = guest_addr & 0x1FFFFFFFu;
    return p >= kBase && p < kBase + kSize;
}

void MetaArena::Tombstone(uint32_t cell_off) {
    uint8_t* p = bytes_.data() + cell_off + kSkew;
    std::memset(p + 0x10, 0, 12);  // +0x10 bucket next, +0x14 chain prev, +0x18 chain next
    uint32_t flags = 0;
    std::memcpy(&flags, p + 0x1c, 4);
    flags &= 0xFFFC0000u;          // keep bits >= 0x40000 (the recycled-flags read)
    std::memcpy(p + 0x1c, &flags, 4);
}

uint32_t MetaArena::Alloc() {
    uint32_t idx;
    if (quarantine_.size() > kQuarantineMin) {
        idx = quarantine_.front();
        quarantine_.pop_front();
    } else if (bump_ < kCapacity) {
        idx = bump_++;
    } else if (!quarantine_.empty()) {
        idx = quarantine_.front();
        quarantine_.pop_front();
    } else {
        return 0;
    }
    state_[idx] = 1;
    ++stats_.allocs;
    ++stats_.live;
    if (stats_.live > stats_.live_peak) stats_.live_peak = stats_.live;
    return kBase + idx * kBlock + kSkew;
}

void MetaArena::Free(uint32_t guest_addr) {
    const uint32_t p = guest_addr & 0x1FFFFFFFu;
    if (p < kBase || p >= kBase + kSize) { ++stats_.invalid_frees; return; }
    const uint32_t off = (p - kBase) & ~(kBlock - 1u);
    const uint32_t idx = off / kBlock;
    if (idx >= kCapacity || state_[idx] != 1) { ++stats_.double_frees; return; }
    Tombstone(off);
    state_[idx] = 2;
    quarantine_.push_back(idx);
    ++stats_.frees;
    --stats_.live;
}

MetaArena::Stats MetaArena::GetStats() const {
    Stats s = stats_;
    s.quarantined = static_cast<uint32_t>(quarantine_.size());
    s.bump_used = bump_;
    return s;
}

void MetaArena::SerializeState(std::vector<uint8_t>& out) const {
    Put32(out, kCapacity);
    out.insert(out.end(), state_.begin(), state_.end());
    Put32(out, bump_);
    Put32(out, static_cast<uint32_t>(quarantine_.size()));
    for (uint32_t idx : quarantine_) Put32(out, idx);
    Put64(out, stats_.allocs); Put64(out, stats_.frees);
    Put64(out, stats_.double_frees); Put64(out, stats_.invalid_frees);
    Put32(out, static_cast<uint32_t>(stats_.live)); Put32(out, static_cast<uint32_t>(stats_.live_peak));
}

bool MetaArena::ValidateState(const uint8_t* p, size_t len, size_t* consumed, std::string* err) const {
    Cursor c(p, len);
    uint32_t cap = 0, qn = 0, u32 = 0; uint64_t u64 = 0;
    bool ok = c.Get(&cap) && cap == kCapacity && c.Skip(kCapacity) && c.Get(&u32) &&
              c.Get(&qn) && qn <= kCapacity;
    for (uint32_t i = 0; ok && i < qn; ++i) ok = c.Get(&u32);
    for (int i = 0; ok && i < 4; ++i) ok = c.Get(&u64);
    ok = ok && c.Get(&u32) && c.Get(&u32);
    if (!ok) { if (err) *err = "META arena bookkeeping (HEAP tail) malformed"; return false; }
    if (consumed) *consumed = c.Offset();
    return true;
}

bool MetaArena::RestoreState(const uint8_t* p, size_t len, size_t* consumed) {
    if (!ValidateState(p, len, consumed, nullptr)) return false;
    Cursor c(p, len);
    uint32_t cap = 0, qn = 0, u32 = 0;
    c.Get(&cap);
    c.GetBytes(state_.data(), kCapacity);
    c.Get(&bump_);
    c.Get(&qn);
    quarantine_.clear();
    for (uint32_t i = 0; i < qn; ++i) { c.Get(&u32); quarantine_.push_back(u32); }
    c.Get(&stats_.allocs); c.Get(&stats_.frees); c.Get(&stats_.double_frees); c.Get(&stats_.invalid_frees);
    c.Get(&u32); stats_.live = static_cast<int32_t>(u32);
    c.Get(&u32); stats_.live_peak = static_cast<int32_t>(u32);
    return true;
}

void MetaArena::ResetForTest() {
    std::fill(bytes_.begin(), bytes_.end(), 0);
    std::fill(state_.begin(), state_.end(), 0);
    quarantine_.clear();
    bump_ = 0;
    stats_ = Stats{};
}
