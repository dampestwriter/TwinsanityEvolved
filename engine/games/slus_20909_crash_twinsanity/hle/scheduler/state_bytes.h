#pragma once

#include <cstdint>
#include <vector>

namespace title {

// Explicit little-endian service payloads, with no host layout or pointers.
class StateBytes {
public:
    explicit StateBytes(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    static void Put(std::vector<uint8_t>& out, uint32_t value);
    static void Put64(std::vector<uint8_t>& out, uint64_t value);
    static void PutFloat(std::vector<uint8_t>& out, float value);
    bool Get(uint32_t& value);
    bool Get64(uint64_t& value);
    bool Get16(uint16_t& value);
    bool Get8(uint8_t& value);
    bool GetBool(bool& value);
    bool GetFloat(float& value);
    bool GetBytes(uint32_t size, std::vector<uint8_t>& out);
    size_t Remaining() const { return bytes_.size() - cursor_; }

private:
    const std::vector<uint8_t>& bytes_;
    size_t cursor_ = 0;
};
} // namespace title
