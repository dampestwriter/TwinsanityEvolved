#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_selection.h"

namespace title {

// Derives the converted texture owner (TXAS) a FROZEN image never carried from
// the image's own memory: the title's TextureTable rows and deletion-queue
// items name every live texture object; a selected object's record is the
// header the original TextureRead copied into it (+0x0c, TR renderer.h
// Texture) and the storage image in the DiskManager block its transfer handle
// (+0x60) names (TR resources.cpp TextureRead -> ReadPacket; disk.cpp
// DiskLoadedMemory: handles at +0x58, DiskNode 0x20). The same TextureCodec
// conversion the owned TextureRead performs produces the asset. Nothing is
// inferred from VRAM, GS registers or an empty live owner; an object the
// image cannot account for refuses by name.
//
// Publication is the owned TextureRead's commit on the object words only
// (+0x60 no transfer, +0x64 no slot, +0x6a first context): the original's
// transfer block stays allocated in the disk pool exactly as the image left
// it (the original released it only at TextureDestroy, which the owned
// adapter now answers).
class TextureImport {
public:
    using Read = MaterialState::Read;
    static constexpr uint32_t kDiskManager = 0x0031b138;   // GetDiskManager's static singleton
    static constexpr uint32_t kDiskHandles = 5500;
    struct Derived {
        TextureAssets::Snapshot snapshot;
        std::vector<uint32_t> objects;     // physical addresses to migrate, snapshot order
        uint64_t table_rows = 0, queued = 0, retained = 0;   // retained = live unselected textures
    };
    // Pure. `read` serves the image's RAM0 and, above 0x02000000, its META
    // arena (the DiskManager nodes live there in images written with the
    // routing on). False names what the image lacks.
    static bool Derive(const Read& read, TextureSelection::Services ready, Derived& out,
                       std::string* error = nullptr);
    // The owned commit words, applied to a RAM image (physical addressing).
    // False when an object lies outside `size`.
    static bool Migrate(uint8_t* ram, size_t size, const std::vector<uint32_t>& objects);
};

}  // namespace title
