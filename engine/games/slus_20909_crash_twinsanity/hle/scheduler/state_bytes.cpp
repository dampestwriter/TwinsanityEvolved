#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

#include <cstring>

namespace title {
void StateBytes::Put(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
void StateBytes::Put64(std::vector<uint8_t>& out, uint64_t value) {
    Put(out, static_cast<uint32_t>(value)); Put(out, static_cast<uint32_t>(value >> 32));
}
void StateBytes::PutFloat(std::vector<uint8_t>& out, float value) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "32-bit float required");
    std::memcpy(&bits, &value, sizeof(bits));
    Put(out, bits);
}
bool StateBytes::Get(uint32_t& value) {
    if (Remaining() < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<uint32_t>(bytes_[cursor_++]) << shift;
    return true;
}
bool StateBytes::Get64(uint64_t& value) {
    uint32_t lo = 0, hi = 0;
    if (!Get(lo) || !Get(hi)) return false;
    value = uint64_t{hi} << 32 | lo;
    return true;
}
bool StateBytes::Get16(uint16_t& value) {
    uint32_t word = 0;
    if (!Get(word) || word > UINT16_MAX) return false;
    value = static_cast<uint16_t>(word);
    return true;
}
bool StateBytes::Get8(uint8_t& value) {
    uint32_t word = 0;
    if (!Get(word) || word > UINT8_MAX) return false;
    value = static_cast<uint8_t>(word);
    return true;
}
bool StateBytes::GetBool(bool& value) {
    uint32_t word = 0;
    if (!Get(word) || word > 1) return false;
    value = word != 0;
    return true;
}
bool StateBytes::GetFloat(float& value) {
    uint32_t bits = 0;
    if (!Get(bits)) return false;
    std::memcpy(&value, &bits, sizeof(bits));
    return true;
}
bool StateBytes::GetBytes(uint32_t size, std::vector<uint8_t>& out) {
    if (size > Remaining()) return false;
    out.assign(bytes_.begin() + cursor_, bytes_.begin() + cursor_ + size);
    cursor_ += size;
    return true;
}
} // namespace title
