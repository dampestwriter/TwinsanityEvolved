#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace title {

// Bounded field encoding used by the two service codecs. Scalars are always
// little endian; booleans have only the canonical byte values zero and one.
// This owns no service and never accesses memory outside the supplied values.
class SnapshotBytes {
public:
    explicit SnapshotBytes(const std::vector<uint8_t>& input);
    SnapshotBytes(std::vector<uint8_t>& output, size_t limit);
    bool U8(uint8_t& value);
    bool U16(uint16_t& value);
    bool U32(uint32_t& value);
    bool U64(uint64_t& value);
    bool I32(int32_t& value);
    bool Boolean(bool& value);
    bool Raw(void* data, size_t size);
    bool Text(std::string& text, size_t limit);
    bool Bytes(std::vector<uint8_t>& bytes, size_t limit);
    bool Done() const;

private:
    bool Integer(uint64_t& value, unsigned width);
    bool Room(size_t count) const;
    const std::vector<uint8_t>* input_ = nullptr;
    std::vector<uint8_t>* output_ = nullptr;
    size_t offset_ = 0, limit_ = 0;
};

}  // namespace title
