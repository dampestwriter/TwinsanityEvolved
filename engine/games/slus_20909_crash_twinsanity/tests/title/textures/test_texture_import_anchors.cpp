// TextureImportAnchors: TextureImport::Derive over every FROZEN anchor the
// title inventories (verify/restore_mhash_expected.tsv: the OLD save_states
// and the coldboot b120 probe) plus the b60 probe. No game process: each image
// is parsed, its RAM0 (+META arena, where the DiskManager nodes live) is read,
// the selected startup textures are derived from the TextureTable rows and the
// DiskManager blocks their transfer handles name, converted, encoded as TXAS,
// and -- after the owned-commit migration of the object words -- pass the same
// TextureSelection::PreflightSnapshot the restore applies. Skips (77) without
// the inventory or anchors.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/core/savestate/savestate_format.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_import.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_owners.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"

using namespace title;
namespace {
int failures = 0, checks = 0;
std::string error;
#define CHECK(value, name) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %s: %s\n", name, error.c_str()); } \
    else std::printf("PASS %s\n", name); } while (false)

const ps2r::savestate::Blob* Find(const std::vector<ps2r::savestate::Blob>& image, uint32_t tag) {
    for (const auto& blob : image) if (blob.tag == tag) return &blob;
    return nullptr;
}
// RAM0 at 0, META at 0x02000000 (MetaArena::kBase), physical addressing.
TextureImport::Read Reader(const std::vector<uint8_t>& ram, const std::vector<uint8_t>& meta) {
    return [&ram, &meta](uint32_t address, void* out, size_t size) {
        const uint32_t at = address & 0x1fffffff;
        const std::vector<uint8_t>* span = &ram;
        uint32_t base = 0;
        if (at >= 0x02000000) { span = &meta; base = 0x02000000; }
        const uint64_t offset = uint64_t(at) - base;
        if (offset + size > span->size()) return false;
        std::memcpy(out, span->data() + offset, size);
        return true;
    };
}
size_t Family(const TextureImport::Derived& derived, TextureOwners::Family family) {
    size_t n = 0;
    for (const auto& asset : derived.snapshot.assets) {
        const auto* owner = TextureOwners::Find(asset.asset_id);
        n += owner && owner->family == family;
    }
    return n;
}
// Derived assets outside the startup catalogue: each must be a PSM picture pair
// Owned under the Sprite family alone (TextureConsumers), never anything else.
size_t Pictures(const TextureImport::Derived& derived, bool* all_owned = nullptr) {
    size_t n = 0;
    for (const auto& asset : derived.snapshot.assets) {
        if (TextureOwners::Find(asset.asset_id)) continue;
        const auto* pair = TexturePairs::Find(asset.asset_id);
        const auto ready = TextureConsumers::Inspect(TextureConsumers::RetailArchive(), asset.asset_id,
                                                     TextureConsumers::Sprite, (1ull << 0x0d) | (1ull << 0x0e));
        if (all_owned && !(pair && pair->kind == TexturePairs::Kind::Picture &&
                           ready.result == TextureConsumers::Result::Owned)) *all_owned = false;
        ++n;
    }
    return n;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("TextureImportAnchors: SKIP (no inventory)\n"); return 77; }
    const std::string inventory = argv[1], old_root = argv[2];
    const std::string probe_dir = argc > 3 ? argv[3] : "";
    if (!std::filesystem::exists(inventory)) { std::printf("TextureImportAnchors: SKIP (inventory missing)\n"); return 77; }
    std::vector<std::string> names;
    std::ifstream rows(inventory);
    std::string line;
    while (std::getline(rows, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string name;
        row >> name;
        names.push_back(name);
    }
    names.push_back("old_b60.ps2st");   // the probe before the fonts load: icons only
    unsigned anchors = 0;
    for (const auto& name : names) {
        std::string path = old_root + "/save_states/" + name;
        if (!std::filesystem::exists(path) && !probe_dir.empty()) path = probe_dir + "/" + name;
        if (!std::filesystem::exists(path)) { std::printf("SKIP %s: anchor file absent\n", name.c_str()); continue; }
        ++anchors;
        ps2r::savestate::FileMeta meta;
        std::vector<ps2r::savestate::Blob> image;
        if (!ps2r::savestate::ReadStateFile(path, &meta, &image, &error)) { CHECK(false, name.c_str()); continue; }
        const auto* ram = Find(image, ps2r::savestate::TAG_RAM);
        const auto* arena = Find(image, ps2r::savestate::TAG_META);
        if (!ram || !arena) { error = "image lacks RAM0/META"; CHECK(false, name.c_str()); continue; }
        CHECK(!Find(image, TextureStateCodec::kTag), (name + ": frozen image carries no TXAS").c_str());
        const auto read = Reader(ram->payload, arena->payload);
        TextureImport::Derived both;
        const bool derived = TextureImport::Derive(read, {true, true}, both, &error);
        CHECK(derived, (name + ": derived (text+sprites)").c_str());
        if (!derived) continue;
        const size_t hud = Family(both, TextureOwners::Family::Hud), crash = Family(both, TextureOwners::Family::CrashFont);
        bool owned_pictures = true;
        const size_t pictures = Pictures(both, &owned_pictures);
        const bool fonts_loaded = meta.vsync_frame > 100;   // the b60 probe precedes the font pages
        error = "hud=" + std::to_string(hud) + " crash=" + std::to_string(crash) + " pictures=" +
                std::to_string(pictures) + " assets=" + std::to_string(both.snapshot.assets.size());
        // The widened UI selection: the 40 icons, the 3 Crash pages
        // and every live PSM picture whose only retail consumer is the Sprite
        // family (TexturePairs + TextureConsumers Owned); Arial stays deferred.
        CHECK(hud == 40 && crash == (fonts_loaded ? 3u : 0u) && both.snapshot.assets.size() == hud + crash + pictures &&
              owned_pictures && Family(both, TextureOwners::Family::ArialFont) == 0,
              (name + ": the 40 icons, the 3 Crash pages and the owned pictures, Arial deferred").c_str());
        std::printf("  %s vsync_frame=%u rows=%llu queued=%llu retained=%llu assets=%zu pictures=%zu\n", name.c_str(),
                    meta.vsync_frame, (unsigned long long)both.table_rows, (unsigned long long)both.queued,
                    (unsigned long long)both.retained, both.snapshot.assets.size(), pictures);
        TextureImport::Derived sprites;
        CHECK(TextureImport::Derive(read, {false, true}, sprites, &error) &&
              sprites.snapshot.assets.size() == 40 + pictures && Family(sprites, TextureOwners::Family::Hud) == 40 &&
              Pictures(sprites) == pictures, (name + ": sprites alone derive the 40 icons and the owned pictures").c_str());
        TextureImport::Derived none;
        CHECK(TextureImport::Derive(read, {false, false}, none, &error) && none.snapshot.assets.empty() &&
              none.retained == both.retained + both.snapshot.assets.size(),
              (name + ": nothing selected derives nothing (every live texture retained)").c_str());
        // The restore's own preflight: before the owned commit the original's
        // transfer word refuses; after it the derived TXAS pairs with
        // every table row and queued item.
        auto migrated = ram->payload;
        TextureBindings::Memory incoming{migrated.data(), migrated.size()};
        CHECK(!TextureSelection::PreflightSnapshot(both.snapshot, incoming, {true, true}, &error) &&
              error.find("transfer ownership") != std::string::npos,
              (name + ": unmigrated objects refuse by name").c_str());
        CHECK(TextureImport::Migrate(migrated.data(), migrated.size(), both.objects),
              (name + ": owned commit words written").c_str());
        CHECK(TextureSelection::PreflightSnapshot(both.snapshot, incoming, {true, true}, &error),
              (name + ": derived TXAS passes the restore preflight on the migrated image").c_str());
        if (crash)
            CHECK(!TextureSelection::PreflightSnapshot(both.snapshot, incoming, {false, true}, &error),
                  (name + ": a narrower selection refuses the font pages in TXAS").c_str());
        std::vector<uint8_t> bytes;
        TextureAssets::Snapshot decoded;
        CHECK(TextureStateCodec::Encode(both.snapshot, bytes, &error) && TextureStateCodec::Decode(bytes, decoded, &error) &&
              decoded.assets.size() == both.snapshot.assets.size() && decoded.next_generation == both.snapshot.next_generation,
              (name + ": TXAS encodes/decodes").c_str());
        // A corrupted transfer handle refuses by name, never a fabricated asset.
        auto broken = ram->payload;
        const uint32_t first = both.objects.front();
        broken[first + 0x60] = 0xff; broken[first + 0x61] = 0xff; broken[first + 0x62] = 0xff; broken[first + 0x63] = 0xff;
        TextureImport::Derived refused;
        CHECK(!TextureImport::Derive(Reader(broken, arena->payload), {true, true}, refused, &error) &&
              error.find("no transfer chain") != std::string::npos,
              (name + ": a selected texture without its transfer refuses by name").c_str());
    }
    CHECK(anchors > 0, "inventory names at least one present anchor");
    std::printf("TextureImportAnchors: %u anchors, %d checks, %d failures\n", anchors, checks, failures);
    return failures ? 1 : 0;
}
