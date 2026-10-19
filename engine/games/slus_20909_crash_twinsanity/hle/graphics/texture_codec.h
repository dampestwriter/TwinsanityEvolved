#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace title {

// The title's authored texture record -> owned, top-first RGBA8 levels.
// This recognizes a fixed file schema, not an executable transfer chain.
// No mirror, frame state, slot address or borrowed asset pointer survives.
class TextureCodec {
public:
    enum class Format { Rgba8, Indexed8 };
    enum class Result { Ok, InvalidAsset, UnsupportedFormat, UnsupportedLayout, LimitExceeded };
    struct Level {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    struct Image {
        Format format = Format::Rgba8;
        bool alpha = false;
        uint8_t function = 0; // source material colour function, 0..3
        std::vector<Level> levels; // every AUTHORED level, never a generated replacement
        std::vector<std::array<uint8_t, 4>> palette; // indexed assets only; GPU alpha
        // TextureContentHash: xxHash64 over the authored levels (count, each
        // level's width, height and RGBA8 bytes). The identity of the converted
        // texture independent of the object, asset id, slot, run or load
        // order: the texture-pack key (`textures/<pack>/<hash>.png`) and the
        // --dump-textures file name. Set by Decode/DecodeParts and ContentHash.
        uint64_t content_hash = 0;
        // A texture pack's replacement pixels for this image (empty: none).
        // Level 0 is the pack's PNG at ANY resolution, level i its box-halved
        // chain, pack_levels.size() == levels.size(). Only the GPU upload
        // paths read it (Pixels()); identity, matching, checkpoints and every
        // reader of the AUTHORED texels keep using `levels`.
        std::vector<Level> pack_levels;
        const std::vector<Level>& Pixels() const { return pack_levels.empty() ? levels : pack_levels; }
    };
    static constexpr size_t kHeaderBytes = 0x60;
    static constexpr size_t kToolsBytes = 0x20;
    static constexpr size_t kMaxRecordBytes = 1024 * 1024;
    static constexpr uint64_t kMaxPixels = 1024 * 1024;

    // TextureRead's complete stream record: LE u32 size, header, tool bytes,
    // fixed storage image. The size excludes its own u32. Success replaces
    // out; every failure leaves it intact with a specific diagnostic.
    static Result Decode(const uint8_t* record, size_t size, Image& out,
                         std::string* error = nullptr);
    // Same conversion for a reader that already owns the header and storage.
    // Header's last twelve game-owned bytes and tool leftovers are not identity.
    static Result DecodeParts(const uint8_t* header, size_t header_size,
                              const uint8_t* storage, size_t storage_size,
                              Image& out, std::string* error = nullptr);
    // The TextureContentHash of `image.levels` (pure; what Decode stores).
    static uint64_t ContentHash(const Image& image);
    // Replace the image's pixels with `rgba` (width x height, top-first RGBA8,
    // any size >= 1): level 0 the replacement, levels 1..n-1 its box-halved
    // chain so every authored mip the title samples has a pack level. False
    // (nothing changed) for an empty image or a size/byte-count disagreement.
    static bool ApplyReplacement(Image& image, uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
};

}  // namespace title
