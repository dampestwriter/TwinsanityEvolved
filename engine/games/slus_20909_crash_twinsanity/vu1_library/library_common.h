#pragma once
// The game-convention helpers every native VU1 program of this title shares:
// the raw 128-bit register record, the title's matrix-row transform orders
// (MULAw/MADDAy/MADDAz/MADDx for the emitters, MULAw/MADDAx/MADDAy/MADDz for
// the clip gate), the guard scale, the OPMULA/OPMSUB cross products in the
// operand order the bodies use, the ADC mark, the XGKICK packet lift from a
// data-memory address, the VU fast inverse square root, the QW1 output
// double-buffer rotation and the working-copy -> data-write diff.
//
// Float rules come from the engine (VuFloat): every FMAC operand read and
// result write clamps (NaN/Inf -> +/-MAX, denormal -> +/-0); fused steps use
// std::fmaf exactly where the interpreter's MADD chain fuses; MOVE/LQ/SQ/MAX/
// MINI copy raw bits. The short names below are the transcription
// vocabulary of the programs; each is one call into VuFloat. Zero primary
// types (free functions + PODs).
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/vu/hle/vu1_hle_contract.h"
#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.h"

namespace vu1lib {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct V4 {
    u32 b[4];
};  // raw 128-bit register (bit-exact lanes)

inline float bits2f(u32 b) { return VuFloat::Bits2F(b); }
inline u32 f2bits(float f) { return VuFloat::F2Bits(f); }
inline float CF(float val) { return VuFloat::ClampFloat(val); }
inline float VuDiv(float num, float den) { return VuFloat::VuDiv(num, den); }
inline u32 Ftoi0(u32 lane_bits) { return VuFloat::Ftoi0(lane_bits); }
inline u32 ClipNf(u32 xb, u32 yb, u32 zb, u32 wb) { return VuFloat::ClipNf(xb, yb, zb, wb); }
inline u32 Fmin_raw(u32 a, u32 b) { return VuFloat::FpMin(a, b); }
inline u32 Fmax_raw(u32 a, u32 b) { return VuFloat::FpMax(a, b); }

// MFIR.w vf, vi(0x8000): the sign-extended ADC mark.
constexpr u32 kAdcMark = 0xFFFF8000u;
// Data-driven mesh-ring walks terminate on their own sentinel; this is the
// safety bound (4x the 1024 physical quadwords).
constexpr int kMeshRingBound = 4096;

// Object-matrix transform, emitter order: ACC = row4*1; += row2*y; += row3*z;
// res = ACC + row1*x  (rows vf04, vf02, vf03, vf01).
V4 XformEmit(const V4& r1, const V4& r2, const V4& r3, const V4& r4, float vx, float vy, float vz);
// Clip-gate transform: ACC = row3*1; += row0*x; += row1*y; res = ACC + row2*z.
V4 XformClip(const V4& r0, const V4& r1, const V4& r2, const V4& r3, float vx, float vy, float vz);
// g = CF(row4 * p) componentwise.
V4 GuardScale(const V4& r4, const V4& p);
// OPMULA/OPMSUB z-lane sign verdict: 32 when the raw sign bit is set, else 0.
u32 CrossZSign(const V4& e16, const V4& e17);
// The full OPMULA/OPMSUB cross (xyz; .w = 0).
V4 CrossXYZ(const V4& e16, const V4& e17);
// The VU fast inverse square root step: FTOI0(c7.x + ITOF0(bits) * c7.w).
u32 Rsq(u32 d2_bits, const V4& c7);

// Lift the GIF packet at QW `qw` (length from the FIRST GIFtag only, as
// XGKICK sizes it; wraps at 16 KB) into `out.kicks`; false when longer than
// data memory.
bool KickAt(QwPtr m, u32 qw, Vu1HleOutput& out);
// LQ vf, 1(vi00); MR32; SQ 1(vi00): the output double-buffer rotation.
void RotateQw1(QwPtr m);

// The FMAC flag commit for programs that reproduce registers: MAC replaces,
// STATUS Z/S/U/O (bits 0-3) are rewritten from the MAC nibbles and the sticky
// twins (bits 6-9) OR in; the FDIV unit's D/I + DS/IS bits are untouched.
void ApplyMac(uint16_t& mac_flag, uint16_t& status, u32 mac);
// One masked FMAC result push (mask bit3=X .. bit0=W) from the RAW result lanes.
void PushMac(uint16_t& mac_flag, uint16_t& status, const u32 raw[4], u8 mask);
// The FDIV status commit: D/I (bits 4-5) replace, DS/IS (bits 10-11) OR in.
// Returns the quotient; `den == 0` yields the sign-xor MAX with I|IS
// (num == 0) or D|DS.
float DivQ(uint16_t& status, u32 num_bits, u32 den_bits);

// The per-call working copy of data memory (one 16 KB buffer per thread).
std::vector<u8>& WorkingCopy(const Vu1HleInput& in);
// Every 16-byte-granular run where `mem` differs from `in.data` -> data_writes.
void EmitDataWrites(const u8* mem, const Vu1HleInput& in, Vu1HleOutput& out);

}  // namespace vu1lib
