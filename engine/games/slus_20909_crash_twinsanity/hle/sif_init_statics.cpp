#include "games/slus_20909_crash_twinsanity/hle/sif_init_statics.h"

// Values read off the ELF's sceSifInitRpc body (the original runtime's
// sif_hle.cpp:63-118 wrote the same set): the ring base/end in the uncached
// KSEG (0x2000_0000 | addr), the 0x20-byte packet step, the queue head, and
// the two libkernl handlers with their argument word.
SceSif::InitRpcStatics SifInitStatics::CrashTwinsanityUS() {
    SceSif::InitRpcStatics s;
    s.init_flag_addr = 0x002ea850;
    s.queue_clear_addr = 0x003caa80;
    s.queue_clear_size = 0x180;
    s.words = {
        {0x003caa58, 0x203ca980}, {0x003caa5c, 0x203caa00}, {0x003caa60, 0x00000000}, {0x003caa64, 0x003caa80},
        {0x003caa68, 0x00000020}, {0x003caa6c, 0x00000000}, {0x003caa70, 0x00000000}, {0x003caa74, 0x003cac00},
        {0x003caa80, 0x002d1a18}, {0x003caa84, 0x003caa58}, {0x003caa8c, 0x002d19f8}, {0x003caa90, 0x003caa58},
    };
    return s;
}
