#pragma once
// FmvContextLayoutData: the SceMpeg context layout of SLUS_209.09 (Crash
// Twinsanity, US) as the engine's ps2r::ipu::FmvContextLayout parameter. The
// libmpeg library is statically linked into this ELF, so the byte offsets of the
// context fields the HLE maintains, the sceMpegAlalc allocator words, the inner
// work area's reference-picture slots and the two guest word VALUES the HLE
// recognises are this title's ABI (the values are the
// reference runtime's mpeg_hle.cpp / libmpeg.cpp constants, read-only).
#include "engine/ipu/hle/fmv_context_layout.h"

struct FmvContextLayoutData {
    // The one layout of this title (every SceMpeg context the game creates uses
    // the same libmpeg build).
    static ps2r::ipu::FmvContextLayout CrashTwinsanityUS();
};
