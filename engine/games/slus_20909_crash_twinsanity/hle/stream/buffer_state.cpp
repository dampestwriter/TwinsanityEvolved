#include "games/slus_20909_crash_twinsanity/hle/stream/buffer_state.h"

#include <utility>
#include <set>

#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
constexpr uint32_t kMaxBytes = 72 * 1024 * 1024, kStorageBytes = 64 * 1024 * 1024;
bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
bool BufferState::Encode(const MusicBuffers::State& state, std::vector<uint8_t>& bytes, std::string* error) {
    if (!MusicBuffers::ValidateState(state, error)) return false;
    const auto& s = state.storage;
    std::vector<uint8_t> out;
    const auto put = [&](uint32_t word) { StateBytes::Put(out, word); };
    put(1); put(s.capacity); put(s.next_handle);
    StateBytes::Put64(out, s.generation); StateBytes::Put64(out, s.sequence);
    for (const auto& slot : s.slots) {
        put(slot.handle); put(slot.bytes); put(slot.window); put(slot.sound_address); put(slot.allocated);
    }
    put(static_cast<uint32_t>(s.storage.size()));
    for (const auto& entry : s.storage) {
        put(entry.first); StateBytes::Put64(out, entry.second.generation);
        put(entry.second.loans); put(entry.second.owned); put(static_cast<uint32_t>(entry.second.bytes.size()));
        out.insert(out.end(), entry.second.bytes.begin(), entry.second.bytes.end());
    }
    put(static_cast<uint32_t>(s.loans.size()));
    for (const auto& entry : s.loans) {
        const auto& loan = entry.second;
        StateBytes::Put64(out, loan.sequence); StateBytes::Put64(out, loan.generation);
        put(loan.handle); put(loan.offset); put(loan.bytes); put(static_cast<uint32_t>(loan.part));
    }
    for (const auto selected : state.selected) put(selected);
    put(state.free_word); put(state.free_value);
    if (out.size() > kMaxBytes) return Fail(error, "buffer snapshot exceeds bound");
    bytes = std::move(out);
    return true;
}
bool BufferState::Decode(const std::vector<uint8_t>& bytes, MusicBuffers::State& state, std::string* error) {
    if (bytes.size() > kMaxBytes) return Fail(error, "buffer snapshot exceeds bound");
    MusicBuffers::State snapshot;
    auto& s = snapshot.storage;
    StateBytes in(bytes);
    uint32_t version = 0, count = 0;
    if (!in.Get(version) || version != 1 || !in.Get(s.capacity) || !in.Get(s.next_handle) ||
        !in.Get64(s.generation) || !in.Get64(s.sequence)) return Fail(error, "invalid buffer snapshot header");
    for (auto& slot : s.slots)
        if (!in.Get(slot.handle) || !in.Get(slot.bytes) || !in.Get(slot.window) ||
            !in.Get(slot.sound_address) || !in.GetBool(slot.allocated)) return Fail(error, "invalid buffer slot");
    if (!in.Get(count) || count > 65536) return Fail(error, "invalid buffer storage count");
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t handle = 0, size = 0;
        StreamBuffers::Storage storage;
        if (!in.Get(handle) || !in.Get64(storage.generation) || !in.Get8(storage.loans) ||
            !in.GetBool(storage.owned) || !in.Get(size) || (total += size) > kStorageBytes ||
            !in.GetBytes(size, storage.bytes) || !s.storage.emplace(handle, std::move(storage)).second)
            return Fail(error, "invalid buffer storage payload");
    }
    if (!in.Get(count) || count > 65536) return Fail(error, "invalid Movie loan count");
    for (uint32_t i = 0; i < count; ++i) {
        StreamBuffers::Loan loan;
        uint32_t part = 0;
        if (!in.Get64(loan.sequence) || !in.Get64(loan.generation) || !in.Get(loan.handle) ||
            !in.Get(loan.offset) || !in.Get(loan.bytes) || !in.Get(part) || part > 1)
            return Fail(error, "invalid Movie loan payload");
        loan.part = static_cast<StreamBuffers::MoviePart>(part);
        if (!s.loans.emplace(loan.sequence, loan).second) return Fail(error, "duplicate Movie loan");
    }
    for (auto& selected : snapshot.selected)
        if (!in.GetBool(selected)) return Fail(error, "invalid buffer selection");
    if (!in.Get(snapshot.free_word) || !in.Get(snapshot.free_value) || in.Remaining())
        return Fail(error, "invalid buffer reply metadata or trailing bytes");
    if (!MusicBuffers::ValidateState(snapshot, error)) return false;
    state = std::move(snapshot);
    return true;
}
bool BufferState::ValidateMovie(const MusicBuffers::State& state,
                               const std::vector<StreamBuffers::Loan>& loans, std::string* error) {
    if (!MusicBuffers::ValidateState(state, error)) return false;
    if (loans.size() != state.storage.loans.size()) return Fail(error, "Movie/buffer loan ownership is incomplete");
    std::set<uint64_t> seen;
    for (const auto& loan : loans) {
        const auto found = state.storage.loans.find(loan.sequence);
        if (found == state.storage.loans.end() || !seen.insert(loan.sequence).second)
            return Fail(error, "Movie loan is missing or duplicated");
        const auto& saved = found->second;
        if (saved.generation != loan.generation || saved.handle != loan.handle ||
            saved.part != loan.part || saved.offset != loan.offset || saved.bytes != loan.bytes)
            return Fail(error, "Movie loan does not match saved buffer ownership");
    }
    return true;
}
} // namespace title
