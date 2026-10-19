#include "games/slus_20909_crash_twinsanity/hle/audio/music_asset.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace title {
namespace {
bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
uint32_t Big32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
}
SampleVoice::Samples Own(std::vector<uint8_t> bytes, bool looping) {
    // STREAM changes marker bytes to chain its upload halves. The title's
    // music intent instead has one beginning/end and an explicit loop mode.
    // Predictor/nibble bytes and decoder history are unchanged.
    for (size_t at = 0; at < bytes.size(); at += VagDecoder::kBlockBytes) bytes[at + 1] = 0;
    bytes[1] = VagDecoder::kFlagLoopStart;
    bytes[bytes.size() - VagDecoder::kBlockBytes + 1] |= VagDecoder::kFlagLoopEnd |
        (looping ? VagDecoder::kFlagLoopRepeat : 0);
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}
}
uint32_t MusicAsset::HeaderBytes(const uint8_t* bytes, size_t size) {
    // Original NEW STREAM 4c14 recognizes exactly these two signatures and
    // advances the supplied file window by 0x30, independently of its header
    // size field. MIB interleaved data is headerless.
    return bytes && size >= 48 &&
        (std::memcmp(bytes, "VAGp", 4) == 0 || std::memcmp(bytes, "MSVp", 4) == 0) ? 48 : 0;
}
bool MusicAsset::Read(const std::vector<uint8_t>& bytes, Layout layout,
                     Data& out, std::string* error) {
    if (layout.tracks < 1 || layout.tracks > 2 || bytes.empty() || bytes.size() > UINT32_MAX)
        return Fail(error, "invalid music extent or track count");
    Data result;
    result.count = layout.tracks;
    result.source_bytes = bytes.size();
    if (layout.tracks == 1) {
        result.header_bytes = HeaderBytes(bytes.data(), bytes.size());
        if (bytes.size() <= result.header_bytes ||
            (bytes.size() - result.header_bytes) % VagDecoder::kBlockBytes)
            return Fail(error, "mono music is not a complete ADPCM extent");
        if (result.header_bytes) result.source_rate = Big32(bytes.data() + 16);
        result.tracks[0] = Own({bytes.begin() + result.header_bytes, bytes.end()}, layout.looping);
    } else {
        const uint32_t block = layout.block_bytes;
        const uint32_t final = layout.final_bytes ? layout.final_bytes : block;
        if (block == 0 || block % VagDecoder::kBlockBytes || final == 0 ||
            final > block || final % VagDecoder::kBlockBytes ||
            bytes.size() % (static_cast<uint64_t>(block) * 2))
            return Fail(error, "unbalanced music interleave or final block");
        const size_t pairs = bytes.size() / (static_cast<uint64_t>(block) * 2);
        if (!pairs) return Fail(error, "empty music pair");
        std::array<std::vector<uint8_t>, 2> tracks;
        for (size_t pair = 0; pair < pairs; ++pair) {
            const size_t count = pair + 1 == pairs ? final : block;
            for (size_t side = 0; side < tracks.size(); ++side) {
                const size_t at = (pair * 2 + side) * block;
                tracks[side].insert(tracks[side].end(), bytes.begin() + at, bytes.begin() + at + count);
            }
        }
        for (size_t side = 0; side < tracks.size(); ++side)
            result.tracks[side] = Own(std::move(tracks[side]), layout.looping);
    }
    out = std::move(result);
    return true;
}
} // namespace title
