#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"

#include <algorithm>
#include <set>
#include <utility>

namespace title {
namespace {
bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
bool StreamBuffers::Capture(Snapshot& out, std::string* error) const {
    Snapshot state{capacity_, next_handle_, generation_, sequence_, slots_, storage_, loans_};
    if (!Validate(state, error)) return false;
    out = std::move(state);
    return true;
}
bool StreamBuffers::Validate(const Snapshot& state, std::string* error) const {
    if (state.capacity != capacity_ || state.next_handle > 0x0fffffff || !state.generation ||
        std::max(generation_, state.generation) == UINT64_MAX ||
        state.storage.size() > 65536 || state.loans.size() > 65536)
        return Fail(error, "buffer capacity or identity mismatch");
    uint64_t bytes = 0;
    std::set<uint32_t> owned;
    std::map<uint32_t, uint8_t> loan_bits;
    for (const auto& entry : state.storage) {
        const auto handle = entry.first;
        const auto& storage = entry.second;
        if ((handle & 0xf0000000) != 0x40000000 || !(handle & 0x0fffffff) ||
            (handle & 0x0fffffff) > state.next_handle || storage.generation == 0 ||
            storage.generation != state.generation || storage.bytes.empty() ||
            storage.bytes.size() % 64 || storage.loans > 3 ||
            (!storage.owned && !storage.loans))
            return Fail(error, "invalid buffer storage");
        bytes += storage.bytes.size();
        if (bytes > state.capacity || bytes > 64 * 1024 * 1024)
            return Fail(error, "buffer storage exceeds snapshot bound");
        if (storage.owned) owned.insert(handle);
    }
    for (const auto& slot : state.slots) {
        if (!slot.allocated) {
            if (slot.handle || slot.bytes || slot.window || slot.sound_address)
                return Fail(error, "closed buffer has metadata");
            continue;
        }
        if (slot.window > slot.bytes || (slot.window != slot.bytes &&
            (slot.window % 64 || (slot.window != 0 && slot.window < 1024))))
            return Fail(error, "invalid buffer window");
        if (!slot.bytes) {
            if (slot.handle) return Fail(error, "empty child has storage");
            continue;
        }
        const auto storage = state.storage.find(slot.handle);
        if (storage == state.storage.end() || !owned.erase(slot.handle) ||
            storage->second.bytes.size() != ((uint64_t{slot.bytes} + 63) & ~uint64_t{63}))
            return Fail(error, "buffer slot has no unique storage");
    }
    if (!owned.empty()) return Fail(error, "owned buffer has no slot");
    for (const auto& entry : state.loans) {
        const auto& loan = entry.second;
        const auto storage = state.storage.find(loan.handle);
        if (!loan.sequence || loan.sequence != entry.first || loan.sequence > state.sequence ||
            storage == state.storage.end() || loan.generation != storage->second.generation ||
            (loan.part != MoviePart::SoundRing && loan.part != MoviePart::DiscStream))
            return Fail(error, "invalid Movie loan identity");
        const auto expected = loan.part == MoviePart::SoundRing ? kSoundBytes : kDiscBytes;
        if (loan.bytes != expected || uint64_t{loan.offset} + loan.bytes > storage->second.bytes.size() ||
            (loan.part == MoviePart::DiscStream && loan.offset != 0) ||
            (loan.part == MoviePart::SoundRing && loan.offset != 0 && loan.offset != kDiscBytes))
            return Fail(error, "invalid Movie loan range");
        if (loan.part == MoviePart::SoundRing && loan.offset == 0 &&
            (storage->second.owned || storage->second.bytes.size() != kSoundBytes))
            return Fail(error, "invalid Movie scratch loan");
        const auto bit = static_cast<uint8_t>(1u << static_cast<unsigned>(loan.part));
        if (loan_bits[loan.handle] & bit) return Fail(error, "duplicate Movie loan part");
        loan_bits[loan.handle] |= bit;
    }
    for (const auto& entry : state.storage)
        if (entry.second.loans != loan_bits[entry.first]) return Fail(error, "unbalanced Movie loan count");
    return true;
}
bool StreamBuffers::Restore(const Snapshot& state, RenewedLoans& renewed, std::string* error) {
    if (!Validate(state, error)) return false;
    if (!loans_.empty()) return Fail(error, "detach both current Movie providers before buffer restore");
    const auto generation = std::max(generation_, state.generation);
    if (generation == UINT64_MAX) return Fail(error, "buffer generations exhausted");
    // Prepare every allocation before replacing the pool. Guest buffer handles
    // stay stable for the restored RAM, but no old token can release new storage.
    auto storage = state.storage;
    auto loans = state.loans;
    uint32_t used = 0;
    for (auto& entry : storage) {
        entry.second.generation = generation + 1;
        used += static_cast<uint32_t>(entry.second.bytes.size());
    }
    for (auto& entry : loans) entry.second.generation = generation + 1;
    auto tokens = loans;
    storage_ = std::move(storage); loans_ = std::move(loans); slots_ = state.slots;
    generation_ = generation + 1;
    next_handle_ = std::max(next_handle_, state.next_handle);
    sequence_ = std::max(sequence_, state.sequence);
    used_ = used;
    renewed = std::move(tokens);
    return true;
}
} // namespace title
