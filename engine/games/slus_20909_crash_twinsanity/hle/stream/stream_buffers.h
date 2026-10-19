#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace title {

// Host storage for the title's stream capacity and Movie lending intents.
// Handles are opaque service identities, never guest/IOP addresses. All
// callers, including both Movie consumers, must use this owner to resolve them.
// Mutated only by the game execution owner.
class StreamBuffers {
public:
    static constexpr uint32_t kStreams = 48, kDiscBytes = 0x28040, kSoundBytes = 0x6000;
    enum class MoviePart : uint8_t { SoundRing, DiscStream };
    struct Loan {
        uint64_t sequence = 0, generation = 0;
        uint32_t handle = 0, offset = 0, bytes = 0;
        MoviePart part = MoviePart::SoundRing;
        explicit operator bool() const { return sequence != 0; }
    };
    struct View {
        uint8_t* data = nullptr;
        uint32_t bytes = 0;
        explicit operator bool() const { return data != nullptr; }
    };
    struct Slot {
        uint32_t handle = 0, bytes = 0, window = 0, sound_address = 0;
        bool allocated = false;
    };
    struct Storage {
        uint64_t generation = 0;
        std::vector<uint8_t> bytes;
        uint8_t loans = 0;
        bool owned = false;
    };
    struct Snapshot {
        uint32_t capacity = 0, next_handle = 0;
        uint64_t generation = 1, sequence = 0;
        std::array<Slot, kStreams> slots{};
        std::map<uint32_t, Storage> storage;
        std::map<uint64_t, Loan> loans;
    };
    using RenewedLoans = std::map<uint64_t, Loan>;

    explicit StreamBuffers(uint32_t capacity);
    StreamBuffers(const StreamBuffers&) = delete;
    StreamBuffers& operator=(const StreamBuffers&) = delete;
    // A duplicate attach preserves the already allocated storage. Failure
    // leaves capacity and the previous slot unchanged.
    bool Allocate(uint32_t stream, uint32_t bytes, uint32_t sound_address);
    bool Resize(uint32_t stream, uint32_t bytes);
    bool Close(uint32_t stream);
    bool CanPlay(uint32_t stream) const;
    Slot Inspect(uint32_t stream) const;
    uint32_t Available() const;
    uint32_t Used() const;
    bool Empty() const;

    // Prepare and OpenStream each get a distinct lease. Zero is the source's
    // no-lent-buffer case: allocate independent Movie scratch storage. An
    // unknown nonzero handle is an error, never a fallback allocation.
    Loan BorrowMovie(uint32_t handle, MoviePart part);
    View Resolve(const Loan& loan);
    bool ReturnMovie(const Loan& loan);
    uint32_t ActiveLoans() const;
    // Close/reset cannot make a live view dangle. Close defers freeing until
    // both loans return; reset refuses until the Movie owner returns them.
    bool Reset();
    // Capture bytes only on the parked game/Movie execution owner. A restore
    // refuses live views. Once both old providers detach, it renews the saved
    // loan tokens and returns them keyed by the saved sequence for reattachment.
    bool Capture(Snapshot& state, std::string* error = nullptr) const;
    bool Validate(const Snapshot& state, std::string* error = nullptr) const;
    bool Restore(const Snapshot& state, RenewedLoans& renewed, std::string* error = nullptr);

private:
    uint32_t Create(uint32_t bytes, bool owned);
    void Collect(uint32_t handle);
    static bool Same(const Loan& a, const Loan& b);
    uint32_t capacity_ = 0, used_ = 0, next_handle_ = 0;
    uint64_t generation_ = 1, sequence_ = 0;
    std::array<Slot, kStreams> slots_{};
    std::map<uint32_t, Storage> storage_;
    std::map<uint64_t, Loan> loans_;
};
} // namespace title
