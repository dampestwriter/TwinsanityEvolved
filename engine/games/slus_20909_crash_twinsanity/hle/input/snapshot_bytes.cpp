#include "games/slus_20909_crash_twinsanity/hle/input/snapshot_bytes.h"

#include <cstring>

namespace title {

SnapshotBytes::SnapshotBytes(const std::vector<uint8_t>& input) : input_(&input), limit_(input.size()) {}
SnapshotBytes::SnapshotBytes(std::vector<uint8_t>& output, size_t limit) : output_(&output), limit_(limit) {}
bool SnapshotBytes::Room(size_t count) const {
    const size_t used = input_ ? offset_ : output_->size();
    return used <= limit_ && count <= limit_ - used;
}
bool SnapshotBytes::Integer(uint64_t& value, unsigned width) {
    if (!Room(width)) return false;
    if (input_) {
        value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t((*input_)[offset_++]) << (i * 8);
    } else {
        for (unsigned i = 0; i < width; ++i) output_->push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
    return true;
}
bool SnapshotBytes::U8(uint8_t& value) {
    uint64_t bits = value;
    if (!Integer(bits, 1)) return false;
    value = static_cast<uint8_t>(bits);
    return true;
}
bool SnapshotBytes::U16(uint16_t& value) {
    uint64_t bits = value;
    if (!Integer(bits, 2)) return false;
    value = static_cast<uint16_t>(bits);
    return true;
}
bool SnapshotBytes::U32(uint32_t& value) {
    uint64_t bits = value;
    if (!Integer(bits, 4)) return false;
    value = static_cast<uint32_t>(bits);
    return true;
}
bool SnapshotBytes::U64(uint64_t& value) { return Integer(value, 8); }
bool SnapshotBytes::I32(int32_t& value) {
    uint32_t bits = static_cast<uint32_t>(value);
    if (!U32(bits)) return false;
    value = bits <= INT32_MAX ? static_cast<int32_t>(bits) : -1 - static_cast<int32_t>(UINT32_MAX - bits);
    return true;
}
bool SnapshotBytes::Boolean(bool& value) {
    uint8_t byte = value ? 1 : 0;
    if (!U8(byte) || byte > 1) return false;
    value = byte != 0;
    return true;
}
bool SnapshotBytes::Raw(void* data, size_t size) {
    if (!Room(size)) return false;
    if (size == 0) return true;
    if (input_) {
        std::memcpy(data, input_->data() + offset_, size);
        offset_ += size;
    } else {
        const auto* first = static_cast<const uint8_t*>(data);
        output_->insert(output_->end(), first, first + size);
    }
    return true;
}
bool SnapshotBytes::Text(std::string& text, size_t limit) {
    if (!input_ && text.size() > limit) return false;
    uint32_t size = static_cast<uint32_t>(text.size());
    if (!U32(size) || size > limit || !Room(size)) return false;
    if (input_) text.resize(size);
    return Raw(text.data(), size);
}
bool SnapshotBytes::Bytes(std::vector<uint8_t>& bytes, size_t limit) {
    if (!input_ && bytes.size() > limit) return false;
    uint32_t size = static_cast<uint32_t>(bytes.size());
    if (!U32(size) || size > limit || !Room(size)) return false;
    if (input_) bytes.resize(size);
    return Raw(bytes.data(), size);
}
bool SnapshotBytes::Done() const { return !input_ || offset_ == limit_; }

}  // namespace title
