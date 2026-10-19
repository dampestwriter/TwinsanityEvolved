#pragma once
// MenuGuest -- the guest side the menu extension works through: word/byte
// access to the title's RAM and a call into a title function with up to eight
// register arguments (a0-a3, t0-t3), returning v0. The composition root binds
// it to the EE memory and DispatchTable::Invoke; the selftest binds a fake RAM
// and a fake title (the constructors' layout effects) so the extension's
// construction, text-table and value-sync logic is pinned without a game.
#include <cstddef>
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"

namespace title {

struct MenuGuest {
    std::function<uint32_t(uint32_t address)> read32;
    std::function<void(uint32_t address, uint32_t value)> write32;
    std::function<void(uint32_t address, const uint8_t* bytes, size_t count)> write_bytes;
    std::function<uint32_t(uint32_t address, char* out, size_t max)> read_string;   // returns length
    // Calls title function `fn` with `count` (<= 8) arguments; returns v0. The
    // registers the call clobbers are the caller's to save (MenuExtension does).
    std::function<uint32_t(CpuContext& ctx, uint32_t fn, const uint32_t* args, size_t count)> call;
};

}  // namespace title
