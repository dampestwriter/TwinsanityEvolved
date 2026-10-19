#pragma once
// ClothProducers -- the title's cloth material producers (shader types 0x17 and
// 0x1A: TR shaders.cpp WriteClothPacket / the per-frame wave update), at their
// NTSC entries (pal_ntsc_map.tsv `high` rows, pinned by ClothBoundaries). The
// composition counts their EXECUTION (the audited DispatchTable::CountCalls
// census pattern; never a behaviour change) so a route search can read whether
// a cloth material ever reached the renderer: run.meta graphics_cloth_producer=.
#include <cstdint>

namespace title {

struct ClothProducers {
    static constexpr uint32_t kType17Packet = 0x001d1d48;   // ShaderType17Packet (PAL FUN_001d24d8)
    static constexpr uint32_t kType17Update = 0x001d20b8;   // ShaderType17Update
    static constexpr uint32_t kType1APacket = 0x001d11b8;   // ShaderType1APacket (PAL FUN_001d1948)
    static constexpr uint32_t kType1AUpdate = 0x001d1560;   // ShaderType1AUpdate
};

}  // namespace title
