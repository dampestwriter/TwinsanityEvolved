#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.h"

void TextureTestData::Put(std::vector<uint8_t>& data, size_t at, uint64_t word, unsigned size) {
    for (unsigned i = 0; i < size; ++i) data.at(at + i) = uint8_t(word >> (i * 8));
}
uint32_t TextureTestData::Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
std::vector<uint8_t> TextureTestData::Record(bool indexed) {
    const uint32_t width = 64, height = indexed ? 96 : 32, bytes = width * height * 4;
    std::vector<uint8_t> data(132 + 96 + bytes);
    Put(data, 0, data.size() - 4);
    Put(data, 8, indexed ? 7 : 6, 2); Put(data, 10, indexed ? 7 : 5, 2);
    data[12] = indexed ? 5 : 1; data[13] = indexed ? 0x13 : 0; data[15] = 1;
    Put(data, 4 + 0x2c, indexed ? 2 : 1);
    if (indexed) {
        const unsigned blocks[]{0, 64, 80, 88, 90};
        for (unsigned i = 1; i < 5; ++i) {
            Put(data, 4 + 0x10 + i * 4, blocks[i]);
            Put(data, 4 + 0x2c + i * 4, 1);
        }
        Put(data, 4 + 0x48, 84);
    }
    const uint32_t at = 132, count = bytes / 16 + 5;
    Put(data, at, 0x60000000 | count); Put(data, at + 12, 0x50000000 | count);
    Put(data, at + 16, 0x1000000000000003ull, 8); Put(data, at + 24, 0xe, 8);
    Put(data, at + 40, 0x51, 8); Put(data, at + 48, width); Put(data, at + 52, height);
    Put(data, at + 56, 0x52, 8); Put(data, at + 72, 0x53, 8);
    Put(data, at + 80, 0x0800000000008000ull | (bytes / 16), 8);
    uint32_t random = 0x1234abcd;
    for (size_t i = at + 96; i < data.size(); ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        data[i] = uint8_t(random);
    }
    return data;
}
