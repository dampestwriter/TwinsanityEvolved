// PS2SDK <tamtypes.h> shim for the wasm32 build: the fixed-width aliases the decomp's common.h expects.
// Layout facts the game pins against: pointers 4 bytes, u64 aligned 8, u128 aligned 16 — all true on wasm32.
#pragma once
#include <stdint.h>
typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;
// The game code never names u128/s128 (grep: 0 uses); PS2SDK's exist for the EE's 128-bit registers. Where the target
// has __int128 (wasm32, x86-64, arm64) use it; a 32-bit ARM syntax lane (vitasdk) gets a 16-byte-aligned pair.
#ifdef __SIZEOF_INT128__
typedef unsigned __int128 u128; typedef __int128 s128;
#else
typedef struct { uint64_t lo, hi; } __attribute__((aligned(16))) u128; typedef u128 s128;
#endif
typedef volatile u8 vu8;   typedef volatile s8 vs8;
typedef volatile u16 vu16; typedef volatile s16 vs16;
typedef volatile u32 vu32; typedef volatile s32 vs32;
typedef volatile u64 vu64; typedef volatile s64 vs64;
typedef volatile u128 vu128; typedef volatile s128 vs128;
