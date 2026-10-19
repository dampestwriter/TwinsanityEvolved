#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

#include <new>
#include <utility>

namespace title {
StreamBuffers::StreamBuffers(uint32_t capacity) : capacity_(capacity & ~63u) {}
uint32_t StreamBuffers::Create(uint32_t bytes, bool owned) {
    const uint64_t aligned = (uint64_t{bytes} + 63) & ~uint64_t{63};
    if (!bytes || aligned > Available() || next_handle_ == 0x0fffffff) return 0;
    Storage storage;
    storage.generation = generation_;
    storage.owned = owned;
    try {
        storage.bytes.resize(static_cast<size_t>(aligned));
        // Separate from the guest's address spaces. No pointer arithmetic is
        // permitted on this value, including the source Movie ring offset.
        const auto handle = 0x40000000u | (next_handle_ + 1);
        storage_.emplace(handle, std::move(storage));
        ++next_handle_;
        used_ += static_cast<uint32_t>(aligned);
        return handle;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}
bool StreamBuffers::Allocate(uint32_t stream, uint32_t bytes, uint32_t sound_address) {
    if (stream >= kStreams) return false;
    auto& slot = slots_[stream];
    if (slot.allocated) return true;
    const auto handle = bytes ? Create(bytes, true) : 0;
    if (bytes && !handle) return false;
    slot = {handle, bytes, bytes, sound_address, true};
    return true;
}
bool StreamBuffers::Resize(uint32_t stream, uint32_t bytes) {
    if (stream >= kStreams || (bytes != 0 && bytes < 1024)) return false;
    const auto aligned = (uint64_t{bytes} + 63) & ~uint64_t{63};
    auto& slot = slots_[stream];
    if (!slot.allocated || !slot.bytes || aligned > slot.bytes || !CanPlay(stream)) return false;
    slot.window = static_cast<uint32_t>(aligned);
    return true;
}
void StreamBuffers::Collect(uint32_t handle) {
    const auto found = storage_.find(handle);
    if (found == storage_.end() || found->second.owned || found->second.loans) return;
    used_ -= static_cast<uint32_t>(found->second.bytes.size());
    storage_.erase(found);
}
bool StreamBuffers::Close(uint32_t stream) {
    if (stream >= kStreams) return false;
    const auto handle = slots_[stream].handle;
    slots_[stream] = {};
    const auto found = storage_.find(handle);
    if (found != storage_.end()) found->second.owned = false;
    Collect(handle);
    return true;
}
bool StreamBuffers::CanPlay(uint32_t stream) const {
    if (stream >= kStreams || !slots_[stream].allocated) return false;
    const auto found = storage_.find(slots_[stream].handle);
    return slots_[stream].bytes == 0 ||
        (found != storage_.end() && found->second.owned && found->second.loans == 0);
}
StreamBuffers::Slot StreamBuffers::Inspect(uint32_t stream) const {
    return stream < kStreams ? slots_[stream] : Slot{};
}
uint32_t StreamBuffers::Available() const { return capacity_ - used_; }
uint32_t StreamBuffers::Used() const { return used_; }
bool StreamBuffers::Empty() const {
    for (const auto& slot : slots_) if (slot.allocated) return false;
    return storage_.empty() && loans_.empty();
}
StreamBuffers::Loan StreamBuffers::BorrowMovie(uint32_t handle, MoviePart part) {
    if ((part != MoviePart::SoundRing && part != MoviePart::DiscStream) || sequence_ == UINT64_MAX) return {};
    const auto bytes = part == MoviePart::SoundRing ? kSoundBytes : kDiscBytes;
    const auto offset = handle && part == MoviePart::SoundRing ? kDiscBytes : 0;
    const bool scratch = handle == 0;
    if (scratch) handle = Create(bytes, false);
    const auto found = storage_.find(handle);
    if (found == storage_.end()) return {};
    auto& storage = found->second;
    const auto bit = static_cast<uint8_t>(1u << static_cast<unsigned>(part));
    if ((!scratch && !storage.owned) || (storage.loans & bit) ||
        uint64_t{offset} + bytes > storage.bytes.size()) return {};
    const Loan loan{sequence_ + 1, storage.generation, handle, offset, bytes, part};
    try {
        loans_.emplace(loan.sequence, loan);
    } catch (const std::bad_alloc&) {
        if (scratch) Collect(handle);
        return {};
    }
    ++sequence_;
    storage.loans |= bit;
    return loan;
}
bool StreamBuffers::Same(const Loan& a, const Loan& b) {
    return a.sequence == b.sequence && a.generation == b.generation && a.handle == b.handle &&
        a.offset == b.offset && a.bytes == b.bytes && a.part == b.part;
}
StreamBuffers::View StreamBuffers::Resolve(const Loan& loan) {
    const auto found = loans_.find(loan.sequence);
    if (found == loans_.end() || !Same(found->second, loan)) return {};
    auto& storage = storage_.at(loan.handle);
    return {storage.bytes.data() + loan.offset, loan.bytes};
}
bool StreamBuffers::ReturnMovie(const Loan& loan) {
    const auto found = loans_.find(loan.sequence);
    if (found == loans_.end() || !Same(found->second, loan)) return false;
    auto& storage = storage_.at(loan.handle);
    storage.loans &= static_cast<uint8_t>(~(1u << static_cast<unsigned>(loan.part)));
    loans_.erase(found);
    Collect(loan.handle);
    return true;
}
uint32_t StreamBuffers::ActiveLoans() const { return static_cast<uint32_t>(loans_.size()); }
bool StreamBuffers::Reset() {
    if (!loans_.empty() || generation_ == UINT64_MAX) return false;
    slots_ = {};
    storage_.clear();
    used_ = 0;
    ++generation_;
    // Handle and loan sequences do not rewind. A delayed old return cannot
    // release storage belonging to a reopened movie or restored game.
    return true;
}
} // namespace title
