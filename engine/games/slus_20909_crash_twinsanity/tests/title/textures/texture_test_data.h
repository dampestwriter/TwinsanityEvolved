#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

class TextureTestData {
public:
    static void Put(std::vector<uint8_t>& data, size_t at, uint64_t word, unsigned size = 4);
    static uint32_t Word(const uint8_t* p);
    static std::vector<uint8_t> Record(bool indexed = true);
};
