#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace title {

// A texture pack: `<directory>/<TextureContentHash as 16 lower-case hex>.png`
// replaces the converted texture with that hash at load (TextureAssets::Load
// -> TextureCodec::ApplyReplacement), at ANY resolution. The directory is
// `<user data dir>/textures/<video.texture_pack>/`.
//
// Lifecycle (the AssetLifecycle contract): Open indexes the
// files once (no decode); Prewarm decodes every indexed file on a background
// thread into a cache bounded by a byte budget; Lookup -- on the serialized
// game owner at the record's Convert -- serves the cache or, for a file the
// prewarm has not reached, decodes it now. A file that is not a PNG the codec
// takes is counted and never served (the converted texture stays). Nothing
// here touches guest memory or the backend.
class TexturePack {
public:
    struct Replacement {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;   // width*height*4, top row first
    };
    struct Counters {
        uint64_t files = 0;            // indexed <hash>.png files
        uint64_t lookups = 0, hits = 0, misses = 0;
        uint64_t decoded = 0, decode_failures = 0, prewarmed = 0;
        uint64_t cached_bytes = 0;     // RGBA bytes held by the cache
        uint64_t uncached = 0;         // replacements served past the budget (decoded, not retained)
    };
    static constexpr uint64_t kDefaultBudget = 1024ull * 1024 * 1024;

    TexturePack() = default;
    ~TexturePack();
    TexturePack(const TexturePack&) = delete;
    TexturePack& operator=(const TexturePack&) = delete;

    // Indexes `directory`. False (and inactive) when it does not exist or holds
    // no <hash>.png; `error` names which. Other files are ignored.
    bool Open(const std::string& directory, std::string* error = nullptr);
    bool Active() const { return !files_.empty(); }
    const std::string& Directory() const { return directory_; }
    // The hash's file path, or empty.
    std::string PathOf(uint64_t hash) const;
    static std::string HexOf(uint64_t hash);
    // Begins decoding every indexed file into the cache (up to `budget` bytes of
    // RGBA) on a background thread. Idempotent.
    void Prewarm(uint64_t budget = kDefaultBudget);
    void Join();
    // Null: no file for this hash, or its PNG was refused (counted once per
    // lookup). Never blocks on the prewarm thread beyond the one file.
    std::shared_ptr<const Replacement> Lookup(uint64_t hash);
    Counters Counts() const;

private:
    std::shared_ptr<const Replacement> Decode(uint64_t hash, const std::string& path, bool prewarm);
    std::string directory_;
    std::unordered_map<uint64_t, std::string> files_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<const Replacement>> cache_;   // null = refused
    uint64_t budget_ = kDefaultBudget;
    Counters counts_;
    std::thread prewarm_;
    bool prewarming_ = false;
};

}  // namespace title
