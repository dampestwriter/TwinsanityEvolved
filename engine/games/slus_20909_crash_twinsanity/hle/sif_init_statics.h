#pragma once
// SifInitStatics -- libkernl 3.0.1's sceSifInitRpc statics in SLUS_209.09, as
// the SDK body writes them on retail (the boundary-0 image carries them):
// the is_initialized word DAT_002ea850, the 0x180-byte client-queue block at
// 0x3caa80 it clears, the queue/ring words 0x3caa58..0x3caa74 and the four
// handler words it seeds. Title data consumed by SceSif::InitRpc.
#include "engine/sif/hle/sce_sif.h"

struct SifInitStatics {
    static SceSif::InitRpcStatics CrashTwinsanityUS();
};
