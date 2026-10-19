#pragma once

#include <string>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"

namespace title {

// Title-owned run.meta publication of the Graphics replaced-model census.
// The reviewed counter catalog (games/<t>/verify/retirement_counters.json)
// binds each claimed replaced operation to its increment site AND its
// publication site, both inside the title. The increments live in the
// services below; graphics_report.cpp is their single publication site. Keys
// and `name=value,...` formats are the gate's parsed contract -- change them
// only together with the catalog. Counts are whatever the caller already
// composed (live owner plus retired-owner census); nothing is read or inferred.
class GraphicsReport {
public:
    using Meta = ps2r::verify::RunMeta;
    // Per-variant Strip producers (TR draw2d.cpp Strip::Draw overloads):
    // graphics_strip_{plain,tinted,placed,placed_tinted}.
    static void Strips(Meta& meta, const GraphicsService::Counters& counts);
    static void Sprites(Meta& meta, const SpriteService::Counters& counts);
    static void Text(Meta& meta, const TextService::Counters& counts);
    // The three Sky boundaries: graphics_sky_{start,model,finish}.
    static void Sky(Meta& meta, const SkyPass::Counters& counts);
    // Source frame boundaries: graphics_frame_{setup,send,interrupt,clear,layout}.
    static void Frame(Meta& meta, const FrameService::Counters& counts);
    // The owned mesh bucket: graphics_meshes -- the four pass
    // boundaries' calls/replaced/model_calls (rigid 0x1bd000, placed 0x1be7f0,
    // flush 0x1ba3a8, direct 0x1be9f8), blocks/instances, the draws recorded
    // (`draws`, the root's count) and faults.
    static void Meshes(Meta& meta, const MeshPass::Counters& counts, bool selected, uint64_t draws);

private:
    static std::string Row(const GraphicsService::VariantCounters& variant);
    static std::string Row(const SkyPass::Counts& count);
    static std::string Row(const FrameService::Count& count);
    static std::string Row(const char* name, const MeshPass::Count& count);
};

}  // namespace title
