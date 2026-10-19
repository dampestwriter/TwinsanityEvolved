#pragma once
// free_list_0x181560 -- HOOK_RECLASSIFICATION section 3.1 row 0x181560
// (EMULATION-BUG-WORKAROUND, behavioral dual-hook): the hook bodies that feed
// MetaArena. Installed by the composition root through the manifest's
// address-keyed TITLE lane (the two addresses live here, never in engine/).
//   0x181560 FUN_00181560(size): when MetaArena::RoutesAllocation(size, ra)
//            and the arena has a cell, v0 = arena payload, return to $ra
//            (the slab allocator body is skipped); otherwise the generated body.
//   0x181590 (slab free, ptr): an arena pointer is freed in the arena and the
//            generated body is skipped; anything else falls through.
// RETIREMENT: with MetaArena's row (boot/level legs match OLD's baselines with
// the hook uninstalled); both rows retire together.
//
// Toggle (Config `hle.meta_arena`): with the routing OFF
// the allocation hook is NOT installed -- FUN_00181560 runs as recompiled and
// every node lands on the title's page pool, as the console keeps it (PCSX2
// rec159: the 385 record descriptors in slab class 7/32 B). Only the free body
// stays installed, and only for pointers already inside the arena: a restored
// image written with the routing on still holds live nodes there (the META
// section restores their bytes, the HEAP tail their bookkeeping), and the
// title's own free FUN_00181590 -> FUN_002045a8(pool, ptr) would treat an arena
// pointer as a page of the pool. Every non-arena pointer falls through, so on
// a fresh boot the retained body is inert.
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"

class MetaArena;

namespace free_list_0x181560 {

using Body = std::function<void(CpuContext&, uint32_t)>;
using Capture = std::function<Body(uint32_t addr)>;          // body installed at addr (the generated one)
using Install = std::function<bool(uint32_t addr, Body body)>;

// Builds the bodies over `arena` wrapping the captured generated bodies and
// installs them; returns the number installed: 2 with `route_allocations`
// (alloc + free), 1 without (the free body alone -- arena retention only).
int InstallHooks(MetaArena& arena, const Capture& capture, const Install& install, bool route_allocations = true);

// The bodies themselves (exposed for the unit test): `orig` is the generated body.
Body AllocHook(MetaArena& arena, Body orig);
Body FreeHook(MetaArena& arena, Body orig);

}  // namespace free_list_0x181560
