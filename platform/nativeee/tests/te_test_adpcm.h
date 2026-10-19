// te_test_adpcm.h -- synthetic SPU2 ADPCM for the audio/stream tests: `blocks` 16-byte blocks of a constant nibble
// (shift 0, predictor 0: decodes to a loud non-zero PCM), the last block carrying LOOP_END (+ LOOP_REPEAT when
// `loop`). No game data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace te_test {

inline std::vector<uint8_t> AdpcmBlocks(std::size_t blocks, uint8_t nibble, bool loop) {
    std::vector<uint8_t> bytes(blocks * 16, 0);
    for (std::size_t b = 0; b < blocks; ++b) {
        bytes[b * 16] = 0x00;   // shift 0, predictor 0: sample = nibble << 12 (sign-extended)
        bytes[b * 16 + 1] = 0;
        for (std::size_t i = 2; i < 16; ++i) bytes[b * 16 + i] = static_cast<uint8_t>((nibble & 0xF) | ((nibble & 0xF) << 4));
    }
    if (blocks) bytes[(blocks - 1) * 16 + 1] = static_cast<uint8_t>(0x01 | (loop ? 0x02 : 0));
    return bytes;
}

}  // namespace te_test
