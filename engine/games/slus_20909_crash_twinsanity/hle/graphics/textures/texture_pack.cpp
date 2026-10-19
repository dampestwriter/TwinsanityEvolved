#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pack.h"

#include <cstdio>
#include <filesystem>

#include "engine/core/image/png.h"

namespace title {
namespace {
bool ParseHex(const std::string& stem, uint64_t& hash) {
    if (stem.size() != 16) return false;
    uint64_t value = 0;
    for (const char c : stem) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A') + 10;
        else return false;
        value = value << 4 | digit;
    }
    hash = value;
    return true;
}
}  // namespace

TexturePack::~TexturePack() { Join(); }

std::string TexturePack::HexOf(uint64_t hash) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

bool TexturePack::Open(const std::string& directory, std::string* error) {
    Join();
    std::lock_guard<std::mutex> lock(mutex_);
    files_.clear();
    cache_.clear();
    counts_ = Counters{};
    directory_ = directory;
    std::error_code code;
    if (directory.empty() || !std::filesystem::is_directory(directory, code)) {
        if (error) *error = "texture pack directory does not exist: " + directory;
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (!entry.is_regular_file(code)) continue;
        const auto& path = entry.path();
        if (path.extension() != ".png" && path.extension() != ".PNG") continue;
        uint64_t hash = 0;
        if (!ParseHex(path.stem().string(), hash)) continue;
        files_[hash] = path.string();
    }
    counts_.files = files_.size();
    if (files_.empty()) {
        if (error) *error = "texture pack holds no <hash>.png: " + directory;
        return false;
    }
    if (error) error->clear();
    return true;
}

std::string TexturePack::PathOf(uint64_t hash) const {
    const auto at = files_.find(hash);
    return at == files_.end() ? std::string() : at->second;
}

std::shared_ptr<const TexturePack::Replacement> TexturePack::Decode(uint64_t hash, const std::string& path, bool prewarm) {
    ps2r::image::Png::Image image;
    std::string error;
    std::shared_ptr<const Replacement> result;
    if (ps2r::image::Png::DecodeFile(path, image, &error)) {
        auto replacement = std::make_shared<Replacement>();
        replacement->width = image.width;
        replacement->height = image.height;
        replacement->rgba = std::move(image.rgba);
        result = std::move(replacement);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (result) ++counts_.decoded; else ++counts_.decode_failures;
    if (prewarm) ++counts_.prewarmed;
    const auto at = cache_.find(hash);
    if (at != cache_.end()) return at->second;   // a concurrent decode won
    const uint64_t bytes = result ? result->rgba.size() : 0;
    if (counts_.cached_bytes + bytes > budget_) {
        ++counts_.uncached;
        return result;
    }
    counts_.cached_bytes += bytes;
    cache_.emplace(hash, result);
    return result;
}

void TexturePack::Prewarm(uint64_t budget) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prewarming_ || files_.empty()) return;
        prewarming_ = true;
        budget_ = budget;
    }
    prewarm_ = std::thread([this] {
        for (const auto& [hash, path] : files_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cache_.count(hash) || counts_.cached_bytes >= budget_) continue;
            }
            Decode(hash, path, true);
        }
    });
}

void TexturePack::Join() {
    if (prewarm_.joinable()) prewarm_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    prewarming_ = false;
}

std::shared_ptr<const TexturePack::Replacement> TexturePack::Lookup(uint64_t hash) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counts_.lookups;
        const auto file = files_.find(hash);
        if (file == files_.end()) { ++counts_.misses; return nullptr; }
        const auto cached = cache_.find(hash);
        if (cached != cache_.end()) {
            if (cached->second) ++counts_.hits; else ++counts_.misses;
            return cached->second;
        }
        path = file->second;
    }
    auto result = Decode(hash, path, false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (result) ++counts_.hits; else ++counts_.misses;
    return result;
}

TexturePack::Counters TexturePack::Counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

}  // namespace title
