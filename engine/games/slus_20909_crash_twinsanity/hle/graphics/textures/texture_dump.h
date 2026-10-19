#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

namespace title {

// --dump-textures <dir>: every converted texture the run loads, once per
// TextureContentHash, as `<dir>/<hash>.png` (level 0, the authored texels,
// GPU alpha) plus `<dir>/index.tsv` (hash, width, height, levels, format,
// first asset id) and, at Finish, `<dir>/contact_sheet.png`: 64x64 thumbnails
// in rows of 16 with the hash under each (a built-in 3x5 hex glyph set) so a
// modder can find the texture to replace and name the file. The files are
// the pack's input: `textures/<pack>/<hash>.png` with the same hash.
class TextureDump {
public:
    static constexpr uint32_t kTile = 64, kLabel = 8, kColumns = 16;
    struct Counters { uint64_t records = 0, written = 0, failures = 0; };

    // Creates the directory. False (and inactive) when it cannot be created.
    bool Open(const std::string& directory, std::string* error = nullptr);
    bool Active() const { return !directory_.empty(); }
    // The converted image of one load; a hash seen before is counted, not rewritten.
    void Record(const TextureCodec::Image& image, uint32_t asset_id);
    // Writes the contact sheet and closes the index. Idempotent.
    void Finish();
    Counters Counts() const;
    // The sheet's RGBA for `thumbnails` (each a top-first RGBA8 image, nearest-
    // sampled into a kTile square) with their hex labels; exposed for the test.
    struct Thumbnail { uint64_t hash; uint32_t width, height; const std::vector<uint8_t>* rgba; };
    static void ContactSheet(const std::vector<Thumbnail>& thumbnails, uint32_t& width, uint32_t& height,
                             std::vector<uint8_t>& rgba);
    // The 3x5 glyph of a hex digit as 5 rows of 3 bits (MSB = left column).
    static const uint8_t* Glyph(unsigned digit);

private:
    struct Entry { uint64_t hash; uint32_t width, height; std::vector<uint8_t> thumb; };
    std::string directory_;
    mutable std::mutex mutex_;
    std::set<uint64_t> seen_;
    std::vector<Entry> entries_;
    std::string index_;
    Counters counts_;
    bool finished_ = false;
};

}  // namespace title
