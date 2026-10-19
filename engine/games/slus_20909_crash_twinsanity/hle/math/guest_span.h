#pragma once

#include <cstddef>
#include <cstdint>

namespace title {

// Guest object spans as the generated loads see them: the address map serves
// every aligned 16-byte quad on its own, so a multi-quad object contiguous in
// guest space is not necessarily contiguous in host memory. The title's stack
// lives in the mirrored scratchpad (anchor `$sp` above 0x70004000), so stack
// objects straddle the 0x70004000 mirror seam; a single host translation of
// the whole object refuses them while the source reads them quad by quad.
// Every operation here walks quads; no byte is reached through a host span.
template <class Memory>
class GuestSpan {
public:
    static constexpr uint32_t kQuad = 16;
    // Every quad of [address, address + size) translates.
    static bool Mapped(Memory& memory, uint32_t address, uint64_t size);
    // Some quad of the two guest spans shares a host quad (aliasing through
    // the mirror counts; distinct host regions never alias).
    static bool Aliases(Memory& memory, uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size);
    // Byte copies through the quads of a Mapped span; false leaves `out` or
    // guest memory partially written only when a quad stopped translating
    // (callers validate with Mapped first).
    static bool Read(Memory& memory, uint32_t address, void* out, size_t size);
    static bool Write(Memory& memory, uint32_t address, const void* in, size_t size);

private:
    static bool Quads(uint32_t address, uint64_t size, uint32_t& first, uint64_t& count);
};

}  // namespace title

#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.inl"
