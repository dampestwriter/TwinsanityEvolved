#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_library.h"

namespace title {

class CullingCheckpoint {
public:
    // Register only when culling is selected. The library must outlive the
    // registry, and dispatch must be quiescent during capture and restore.
    // Every new capture carries CULL (capture-required); an older image without
    // it is admitted only under the legacy import contract below, never by
    // seeding boot or model state into an owned result.
    // One relaunched thread's shadow call chain (innermost first), decoded from
    // the THRD payload by the application's ThreadSection codec; `live` marks a
    // started record with a parked context (the ones the relauncher walks).
    struct ThreadChain {
        int32_t tid = 0;
        bool live = false;
        std::vector<uint32_t> functions;
    };
    using ChainParser = std::function<bool(const std::vector<uint8_t>& payload, std::vector<ThreadChain>& chains,
                                           std::string* error)>;
    static bool Register(ps2r::savestate::SectionRegistry& registry, CullingLibrary& library,
                         ChainParser chains = {});

    // Legacy import contract (source lifetime, not fabrication): the chunk view
    // and every culling result are locals of DrawScene (ChunkView,
    // TR chunkscenery.cpp:1595; LinkedChunkView in DrawLinkedChunk :1442),
    // constructed, loaded, consumed and destroyed inside one frame. An image
    // whose THRD section proves that no relaunched thread chain carries a frame
    // inside DrawScene therefore holds no live culling state, and the owned
    // state at restore is the source's own boundary state: empty. An image
    // without THRD, without a chain parser, or with any chain frame inside
    // DrawScene or a culling entry, is refused with the reason. Pure over the
    // parsed sections (no live state is touched).
    static bool LegacyImportAdmissible(const std::vector<ps2r::savestate::Blob>& sections,
                                       const ChainParser& chains, std::string* reason);
    // NTSC DrawScene: the single direct caller of ChunkView::Load 1f4e50.
    static constexpr uint32_t kDrawScene = 0x1f11d8;
};

}  // namespace title
