// Vu1Library -- the family signature + the registration table: the 223 OLD
// rows (image / variant keyed) followed by the 19 layout-keyed rows.
//
// Table order = the OLD library's static-initialisation (textual) order,
// replayed verbatim from its registration sites; never sort it. Rows carry
// the OLD program identity (namespace::function) in the trailing comment.
// The layout rows (trailing key = Vu1ImageInfo::layout_hash, the hash of the
// loaded shader-type programs alone: Vu1LoadedPrograms) come after every
// variant row; the registry consults them only when no image / variant row
// matched, so they catch the same programs under a different stale tail
// (the Mecha-Bandicoot boss: 40 images, every one a known layout).
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_library.h"

#include "games/slus_20909_crash_twinsanity/vu1_library/crys_programs.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/family_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/indexed_displace.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/lit_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/poscolor_strip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/sprite_text.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/textured_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"

namespace {

// variant_base 0x1720 (the per-material variable region), dispatcher op
// ILW.x vi14,3(vi00), exit stubs 0x70 / 0x90, family exit TPC 0xA0.
constexpr Vu1FamilySignature kSignature = {0x1720, 0x090E0003u, {0x70, 0x90}, 0xA0};

const Vu1LibraryEntry kEntries[] = {
    {0, 0xd4264095dc680c85ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xbc9849f38ebc0a6eull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xf59d4efe8264262dull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0x0bd46c78a72b5aa7ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xfb73f8e2d4498f9bull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xbe67f811b89bbf56ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xf831d84c4b71339dull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0x09fbbc14e1434b23ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xb6c0a812a25984d8ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0x79bc13673bab878bull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x63342f8ce645acfcull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xe2b8b381e3de10aaull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x0b4bc43f45f28de4ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xa9d9c640b0d1284aull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x0f2d00d0cccd1019ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xe7e827482e8e18fcull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xdbad5a4b245ee695ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xac7822d5948669c6ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x622f007cd1879908ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x2945f9d59cd94125ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x4bba921c0ffde926ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x76fcaf26ad2cfe9eull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x7b049d5af792db12ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x22cbdb291d8ed2cdull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xc6589abead9f8a87ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x24d4c51976887a40ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x1266e845aa7b25c5ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x45bc8b43ccfeee10ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x51ce857cea006f1cull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x5e08c0b91b1db163ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xb872446873f25107ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x94d105f476750350ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xc74e01bf1f05261eull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x60875a4745bf6ea2ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xccb0523660e2825eull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xaf10645e7efc2487ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0xc1bdbce0f14a3645ull, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0},  // pcs::vs_poscolor_strip
    {0, 0x29ef170debfde11cull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x78daad152bb457bfull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xf401b693c8770e18ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xe203d8c3c03f4212ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x39b658565136e769ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xd0962f5b215b8f41ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x7604e8e6c77c258aull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xc6e35025d6611724ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x14e77a0bba6bf6c1ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xd4fb8b9367ecef94ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x15ae59a66953be52ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xa216b2f4dafb3f06ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x7de97db75adefb31ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x8d300029a7d87d7aull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x03088e26f367e44aull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0x996335e7fe379f8cull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x66bfddf51b32521full, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xede0db19a3f795bfull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xd1013668591dbcc5ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xe075fce30780f340ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xd5ab07b6df53af46ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x2eca4c7e86639cebull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x6ee5660115947a86ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x5835b479430d5dd8ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x9fc227e47bf3bcc6ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x9fc456355a6d11dbull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xe2ca0cd8798e3746ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x97d33b44f71dcfedull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xbbe9ee734f6216a3ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x0c7b9c7111cb0593ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x0f9e8908fcd6ae38ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x1aa9b22b265f5ae7ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x7d61ec7a719eafd0ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xd2ce2b9d73cb4331ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x7f013e433439f275ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xa441c06209b2f33eull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xafb5b2672f52ad23ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x85bf26a8ee9a1a93ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xc64214175df95b2dull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xea112696222b76a8ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xff64fdd350d3748full, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x747095a78532368eull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xa28e0aa4b33d3b54ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x007c2975b8789dfeull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x790f32e6ae91ca28ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x9aa4d4fa71e0cbb3ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x1f910f33851ba722ull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0x01e7c7348cf8940bull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0x8b3094b3d9d37630ull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0x2475eaecccf3d1faull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0x6636131c9cdd9952ull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0xd7e02c9607bfae2aull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0},  // idx::vs_indexed_displace_bw
    {0, 0x156a35a758dac5e0ull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunComponentwise, nullptr, 0},  // idx::vs_indexed_displace_cw
    {0, 0xc7f0ebdc81c7540dull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunComponentwise, nullptr, 0},  // idx::vs_indexed_displace_cw
    {0, 0x94b47176b5be607cull, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunComponentwise, nullptr, 0},  // idx::vs_indexed_displace_cw
    {0, 0x24fbe13f60be26f9ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x92ba42dbcc9c7956ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xfa8fd92869ba4557ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xeaa1bee5ee52de39ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xe1d1f8fd0794c709ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xb4c16758d3c6564cull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xc708bf78dd101720ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x8e4d796a1dd57d4bull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xdbc0606ea4e661f3ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x66dc00dd73eaf46eull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x5d91c55d204cb81full, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xcc44257d771cda18ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x6e7e308d61f6dcaaull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xf5557e628822867eull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x3b8beaa2b393bab1ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x38edec96211894d5ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xa40724e4a4921928ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x65a4b57323599908ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x00e18f48fe1fac87ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x513abaf6614f2dceull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x3d077d649b580d1full, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x96a793bf2121a88aull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x96e4ccd001c969e3ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xcef66b4b0614d473ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x7a45051ad94b39bcull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x7c22efaed56b3ed5ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0x342ea759820ad9d0ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xba05aed196fde3cfull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xf7ce18ebfd777c61ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xead11fdca591c42full, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x8e18c862db5132b6ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xe16dee2cb13a2165ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x40b34f327e3c4aa5ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xbfabb60d8bcbd168ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xce1fa79f812d58cfull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x749b567b51c894e3ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x7433fad04c3526efull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xb74ea76ef377ccd5ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xa1990e01175d703aull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x1a8542770aacdfccull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xf55bd6e8aa1cf565ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x4c0437f697217026ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xbf925de8b0f262daull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xdb5374bdfda78f40ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x75fa3d1a36bd3e42ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x2c28ebdf0b989f55ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x6bd96dbefe02aaceull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x8bacd33c9c653de5ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x33aab5121b88c01aull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x0e7987a0e45d7621ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xdf0b7a2caa856e6aull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xf4d617e3177b4ea1ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xcefee708f2bdc7cdull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x937631568fa382a3ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x32f9bfc4c2929092ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x618313cb6a3ec2fcull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xc63bae4f11b5f1faull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x5efe3f850ed79cb8ull, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0},  // zk::vs_zero_kick_a
    {0, 0xdc5d84c360a3cbc9ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x7b1263a9c13b9c81ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0xe616ab6be9ae84e4ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x3ef146683b702c01ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x81d874e3d11f7219ull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x010eb9ebc56aa8acull, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0},  // zk::vs_zero_kick_b
    {0, 0x037b3f421adf17d5ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x59b148e8392fd464ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xa20511270f7a3269ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xa20511270f7a3269ull, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x24fbe13f60be26f9ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x8e4d796a1dd57d4bull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xe1d1f8fd0794c709ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xb4c16758d3c6564cull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x92ba42dbcc9c7956ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xc63bae4f11b5f1faull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x5d91c55d204cb81full, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x66dc00dd73eaf46eull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xdbc0606ea4e661f3ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xfa8fd92869ba4557ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xa248521350e6712aull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xe73e7080e7fea239ull, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x24fbe13f60be26f9ull, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x8e4d796a1dd57d4bull, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xa248521350e6712aull, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0xe73e7080e7fea239ull, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0},  // zk::vs_zero_kick_c_cont
    {0, 0x65a4b57323599908ull, "vs_zero_kick_a_cont", 0x1750, &ZeroKick::RunACont, nullptr, 0},  // zk::vs_zero_kick_a_cont
    {0, 0x38edec96211894d5ull, "vs_zero_kick_a_cont", 0x1750, &ZeroKick::RunACont, nullptr, 0},  // zk::vs_zero_kick_a_cont
    {0, 0x342ea759820ad9d0ull, "vs_zero_kick_a_cont", 0x1750, &ZeroKick::RunACont, nullptr, 0},  // zk::vs_zero_kick_a_cont
    {0, 0x0e7987a0e45d7621ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xf4d617e3177b4ea1ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xcefee708f2bdc7cdull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xdf0b7a2caa856e6aull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x8bacd33c9c653de5ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x33aab5121b88c01aull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xdc5d84c360a3cbc9ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xe16dee2cb13a2165ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x937631568fa382a3ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xbfabb60d8bcbd168ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x1a8542770aacdfccull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x32f9bfc4c2929092ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x2c28ebdf0b989f55ull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x618313cb6a3ec2fcull, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xcefee708f2bdc7cdull, "vs_sprite_text", 0x1980, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0xe16dee2cb13a2165ull, "vs_sprite_text", 0x1980, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x8bacd33c9c653de5ull, "vs_sprite_text", 0x1980, &SpriteText::Run, nullptr, 0},  // spr::vs_sprite_text
    {0, 0x0e07e0f0c55d22a4ull, "vs_crys_dual", 0x0000, &CrysPrograms::RunDual, nullptr, 0},  // crys::vs_crys_dual
    {0, 0x8fd08b8c14bcd597ull, "vs_crys_displace", 0x1400, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0x8fd08b8c14bcd597ull, "vs_crys_displace", 0x12a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0x8fd08b8c14bcd597ull, "vs_crys_displace", 0x15a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0xe1f9ceeaa5c1ef66ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0x677f668c9890a6c2ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0xb652e83e7d7a6ce2ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xada0c87949339abcull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xa248521350e6712aull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xefef1a54a905f16dull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xb1ec7ebc42815279ull, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0},  // ttx::vs_textured_tristrip
    {0, 0x60d4d51974d7a929ull, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0},  // lit::vs_lit_tristrip
    {0, 0xaa4d9034cb258763ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xe73e7080e7fea239ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0xaf75c3cf33d0a783ull, "vs_crys_dual", 0x0000, &CrysPrograms::RunDual, nullptr, 0},  // crys::vs_crys_dual
    {0, 0xe4895868ddc37c1cull, "vs_crys_displace", 0x12a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0xe4895868ddc37c1cull, "vs_crys_displace", 0x1400, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0xe4895868ddc37c1cull, "vs_crys_displace", 0x15a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0x14bf0dcbf2b115d4ull, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x0d81bf222f34a291ull, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0},  // vs_family_tristrip
    {0, 0xa20511270f7a3269ull, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0},  // zk::vs_zero_kick_c
    {0, 0x59b148e8392fd464ull, "vs_crys_displace", 0x12a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0xbcd0ac135c19bee4ull, "vs_crys_displace", 0x12a0, &CrysPrograms::RunDisplace, nullptr, 0},  // crys::vs_crys_displace
    {0, 0x8fd08b8c14bcd597ull, "vs_skinned_lit_strip", 0x0000, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0x8fd08b8c14bcd597ull, "vs_skinned_lit_strip", 0x0090, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xe4895868ddc37c1cull, "vs_skinned_lit_strip", 0x0000, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    {0, 0xe4895868ddc37c1cull, "vs_skinned_lit_strip", 0x0090, &SkinnedLitStrip::Run, nullptr, 0},  // skn::vs_skinned_lit_strip
    // Layout-keyed rows: (the loaded programs from 0x1720, the entry pc) -> the
    // program the corpus registers for that layout (every (layout, entry) of the
    // 3,419 OLD corpus cases + the 159 boss cases maps to ONE program; the join is
    // int_work/m_series/math/boss_vu1/corpus_layouts_all.txt). Comment = the
    // layout and the address past its last program.
    {0, 0, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0, 0xb1c5bddf26cf2701ull},  // layout 01 @0x1e90
    {0, 0, "vs_textured_tristrip", 0x0000, &TexturedTristrip::Run, nullptr, 0, 0x4bba921c0ffde926ull},  // layout 01+0C @0x2c80
    {0, 0, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0, 0xb37a46283257121dull},  // layout 02 @0x1e40
    {0, 0, "vs_lit_tristrip", 0x0000, &LitTristrip::Run, nullptr, 0, 0xb64cc9fc4a8ec58cull},  // layout 02+0C @0x2c30
    {0, 0, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0, 0x98e320959b851f47ull},  // layout 04 @0x1aa0
    {0, 0, "vs_skinned_lit_strip", kVu1AnyEntry, &SkinnedLitStrip::Run, nullptr, 0, 0xa1adc9db650afe84ull},  // layout 04+0F @0x20c0
    {0, 0, "vs_poscolor_strip", 0x0000, &PoscolorStrip::Run, nullptr, 0, 0xd9c9720b81b93b44ull},  // layout 0A @0x1c90
    {0, 0, "vs_family_tristrip", 0x0000, &FamilyTristrip::Run, nullptr, 0, 0x57f3460eb627b506ull},  // layout 0B @0x1e80
    {0, 0, "vs_zero_kick", 0x0000, &ZeroKick::RunB, nullptr, 0, 0xcefee708f2bdc7cdull},  // layout 0D @0x1990
    {0, 0, "vs_sprite_text", 0x1748, &SpriteText::Run, nullptr, 0, 0xcefee708f2bdc7cdull},  // layout 0D @0x1990
    {0, 0, "vs_sprite_text", 0x1980, &SpriteText::Run, nullptr, 0, 0xcefee708f2bdc7cdull},  // layout 0D @0x1990
    {0, 0, "vs_zero_kick", 0x0000, &ZeroKick::RunC, nullptr, 0, 0xee9908300182eea6ull},  // layout 12 @0x1b00
    {0, 0, "vs_zero_kick_cont", 0x1820, &ZeroKick::RunCCont, nullptr, 0, 0xee9908300182eea6ull},  // layout 12 @0x1b00
    {0, 0, "vs_zero_kick_cont", 0x1ac8, &ZeroKick::RunCCont, nullptr, 0, 0xee9908300182eea6ull},  // layout 12 @0x1b00
    {0, 0, "vs_zero_kick", 0x0000, &ZeroKick::RunA, nullptr, 0, 0x8c9259f98205f849ull},  // layout 13 @0x19f0
    {0, 0, "vs_zero_kick_a_cont", 0x1750, &ZeroKick::RunACont, nullptr, 0, 0x8c9259f98205f849ull},  // layout 13 @0x19f0
    {0, 0, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunBroadcast, nullptr, 0, 0x1f910f33851ba722ull},  // layout 17 @0x1ee0
    {0, 0, "vs_indexed_displace", 0x0000, &IndexedDisplace::RunComponentwise, nullptr, 0, 0x171eb01de70d5e8cull},  // layout 1A @0x1ef0
    {0, 0, "vs_crys_dual", 0x0000, &CrysPrograms::RunDual, nullptr, 0, 0xc4161b35c8613387ull},  // layout 10+02 @0x28d0
};

constexpr size_t kOldRows = 223;    // the OLD library's rows (image / variant keyed)
constexpr size_t kLayoutRows = 19;  // the layout-keyed rows after them
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == kOldRows + kLayoutRows, "the registration table has 242 rows");

}  // namespace

const Vu1FamilySignature& Vu1Library::Signature() { return kSignature; }

const Vu1ProgramLibrary& Vu1Library::Get() {
    static const Vu1ProgramLibrary library = {kSignature, kEntries, sizeof(kEntries) / sizeof(kEntries[0]),
                                              Vu1LoadedPrograms::Table(), Vu1LoadedPrograms::Count()};
    return library;
}
