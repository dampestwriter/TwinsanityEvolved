#pragma once

#include <cstdint>
#include <vector>
#include "engine/ee/cpu/cpu_state.h"

namespace stream_oracle {
constexpr uint32_t kGp = 0x003114f0;              // the title's $gp (the STREAM client statics are $gp-relative)
constexpr uint32_t kSlotMaxAddr = kGp - 0x794c;   // the client's slot count word FUN_001e3308 bounds the slot by
constexpr uint32_t kHandleCounter = kGp - 0x74d8; // the loader's handle counter word
constexpr uint32_t kClientRecord = 0x003b27c0;    // the STREAM client's sceSifBindRpc record (FUN_001e11e8)
constexpr uint32_t kStreamManagerPtr = 0x0030a748; // the title's stream-manager pointer (FUN_002ad560 stores it; FUN_002ab160 loads it)
constexpr uint32_t kRingSizeTable = 0x002e7740;   // the client's per-slot ring size words FUN_001e1ca8 fills (slot * 4)
constexpr uint32_t kFnStreamManagerCtor = 0x2add88;   // FUN_002add88(mgr, capacity)
constexpr uint32_t kFnStreamOpen = 0x2ad650;      // FUN_002ad650(mgr, ring_size, 0, 0) -> fs
constexpr uint32_t kStreamManagerCapacity = 9;    // FUN_002ad560's argument
constexpr uint32_t kStreamManagerBytes = 0x1c;    // FUN_002ad560: alloc(0x1c) then the ctor
constexpr uint32_t kRingSize = 0x800;             // FUN_002ab160: FUN_002ad650(mgr, 0x800, 0, 0)
constexpr uint32_t kFlagRingOpen = 0x00020000u;   // FUN_002ad7c8 sets it once the ring stream is staged
constexpr uint32_t kCmdRingStreamOpen = 13;       // STREAM cmd 13 = open a ring stream for the slot
constexpr uint32_t kCmdRegisterFile = 31;         // STREAM cmd 31 = register a file handle (the seam's Open)
constexpr uint32_t kBatchRecords = kGp - 0x78f8;  // the client's staged-record count FUN_001e13b0 bumps on commit
constexpr uint32_t kBatchBuffer = 0x003b17c0;     // the client's batch buffer {u16 hdr, u16 count, records...}
constexpr uint32_t kFsControl = 0x00512000;       // a second fs object with nothing staged
constexpr uint32_t kDest = 0x00600000;
constexpr uint32_t kSp = 0x00700000;
constexpr uint32_t kRigHeap = 0x00800000;         // the test-local allocator's region (the title heap stands in)
constexpr uint32_t kRigHeapEnd = 0x00900000;
constexpr uint8_t kControlSlot = 7;
constexpr uint32_t kReadBytes = 0x800;            // one sector of the file: the module's DMA chunk

void Prep(CpuContext& ctx);
void SetArg(CpuContext& ctx, int reg, uint32_t value);
uint32_t TitlePoll(CpuContext& ctx, uint32_t fs);
bool RangeEquals(uint32_t addr, const std::vector<uint8_t>& oracle, uint32_t off, uint32_t n);
bool RangeIs(uint32_t addr, uint8_t value, uint32_t n);
}  // namespace stream_oracle
