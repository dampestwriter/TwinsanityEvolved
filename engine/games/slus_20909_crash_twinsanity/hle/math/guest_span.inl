// GuestSpan template bodies (textual sibling of guest_span.h).
#include <algorithm>
#include <cstring>

namespace title {

template <class Memory>
bool GuestSpan<Memory>::Quads(uint32_t address, uint64_t size, uint32_t& first, uint64_t& count) {
    if (size == 0 || static_cast<uint64_t>(address) + size > (uint64_t{1} << 32)) return false;
    first = address & ~(kQuad - 1u);
    const uint64_t end = static_cast<uint64_t>(address) + size;
    count = (end - first + kQuad - 1) / kQuad;
    return true;
}

template <class Memory>
bool GuestSpan<Memory>::Mapped(Memory& memory, uint32_t address, uint64_t size) {
    uint32_t first;
    uint64_t count;
    if (!Quads(address, size, first, count)) return false;
    for (uint64_t quad = 0; quad < count; ++quad)
        if (!memory.Translate(static_cast<uint32_t>(first + quad * kQuad), kQuad)) return false;
    return true;
}

template <class Memory>
bool GuestSpan<Memory>::Aliases(Memory& memory, uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size) {
    uint32_t a_first, b_first;
    uint64_t a_count, b_count;
    if (!Quads(a, a_size, a_first, a_count) || !Quads(b, b_size, b_first, b_count)) return false;
    for (uint64_t i = 0; i < a_count; ++i) {
        const uint8_t* host = memory.Translate(static_cast<uint32_t>(a_first + i * kQuad), kQuad);
        if (!host) continue;
        for (uint64_t j = 0; j < b_count; ++j)
            if (host == memory.Translate(static_cast<uint32_t>(b_first + j * kQuad), kQuad)) return true;
    }
    return false;
}

template <class Memory>
bool GuestSpan<Memory>::Read(Memory& memory, uint32_t address, void* out, size_t size) {
    auto* bytes = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < size) {
        const uint32_t at = static_cast<uint32_t>(address + done);
        const uint32_t quad = at & ~(kQuad - 1u);
        const uint8_t* host = memory.Translate(quad, kQuad);
        if (!host) return false;
        const size_t offset = at - quad, chunk = std::min<size_t>(kQuad - offset, size - done);
        std::memcpy(bytes + done, host + offset, chunk);
        done += chunk;
    }
    return true;
}

template <class Memory>
bool GuestSpan<Memory>::Write(Memory& memory, uint32_t address, const void* in, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(in);
    size_t done = 0;
    while (done < size) {
        const uint32_t at = static_cast<uint32_t>(address + done);
        const uint32_t quad = at & ~(kQuad - 1u);
        uint8_t* host = memory.Translate(quad, kQuad);
        if (!host) return false;
        const size_t offset = at - quad, chunk = std::min<size_t>(kQuad - offset, size - done);
        std::memcpy(host + offset, bytes + done, chunk);
        done += chunk;
    }
    return true;
}

}  // namespace title
