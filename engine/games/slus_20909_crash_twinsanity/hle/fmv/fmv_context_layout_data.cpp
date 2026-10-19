#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"

// Offsets are bytes from the SceMpeg context the game passes as `mpeg`
// (the FMV player's context at FUN_002b01e8 / FUN_002b0660 in this ELF).
ps2r::ipu::FmvContextLayout FmvContextLayoutData::CrashTwinsanityUS() {
    ps2r::ipu::FmvContextLayout l{};
    // Bitstream ring the demux callback (FUN_002b0660) fills for the IPU.
    l.ring_base = 0xF0;
    l.ring_size = 0xF4;
    l.ring_write = 0xF8;
    l.ring_read = 0xFC;
    l.ring_anchor = 0x100;
    // Pointer to the inner work area; its first word is the end-of-stream status.
    l.inner_status_ptr = 0x40;
    // "frames to show", compared signed against the presented count by the
    // player's completion gate; held at a large positive value while playing.
    l.total = 0x50;
    l.play_to_eof_total = 0x40000000;
    // Presented-picture counter (bit 0 selects RAW8 double buffering).
    l.presented = 0x54;
    // Output plane geometry the player allocated.
    l.target_w = 0x58;
    l.target_h = 0x5C;
    // The two PSMCT16 output planes.
    l.plane0 = 0x94;
    l.plane1 = 0x98;
    // Player mode word; 2 is the RAW8 plane path (the movie player).
    l.mode = 0x148;
    l.mode_raw8 = 2;
    // Alternate-plane flag read when double buffering is on.
    l.plane_select = 0x158;
    // The scene updater's abort request (latched when the refill callback
    // reports end of file).
    l.abort_flag = 0x15C;
    // sceMpegAlalc allocator words (offsets within the allocator struct).
    l.alalc_base = 0x00;
    l.alalc_capacity = 0x04;
    l.alalc_watermark = 0x08;
    l.alalc_base_copy = 0x0C;
    // Reference-picture slot pointers inside the inner work area: luma Y0/Y1/Y2
    // then chroma C0/C1/C2; the status word sits at +0x28 of each slot.
    l.ref_slot[0] = 0x1C8;
    l.ref_slot[1] = 0x1D8;
    l.ref_slot[2] = 0x1E8;
    l.ref_slot[3] = 0x1CC;
    l.ref_slot[4] = 0x1DC;
    l.ref_slot[5] = 0x1EC;
    l.ref_slot_status = 0x28;
    return l;
}
