#include "engine/title/title_profile.h"

// Boot environment (retail, PINE capture of the OSD launch): argc=1,
// argv[0]="cdrom0:\SLUS_209.09;1"; crt0 reads the block at 0x30a980. The
// game's parser then falls back to its built-in command line -- retail runs
// with BOTH "rb" and "batch=Crash6\Crash" (OLD ps2_argv.h; PS2_ARGV NORM=retail).
// sbrk break word 0x2ea844 = libc's break pointer the kernel HLE seeds at
// InitHeap (OLD ps2_threading.cpp initHeap). heap_limit_cap 0x01F00000 = OLD's
// top-1-MB reserve (PTC-stub view slots) kept for boot-image parity; a Phase-4
// adjudication item (retire when the stub views move off the guest heap).
// The engine DECLARES ActiveTitleProfile() (engine/title/title_profile.h); this title DEFINES it.
const TitleProfile& ActiveTitleProfile() {
    static const TitleProfile p = [] {
        TitleProfile t;
        t.serial = "SLUS_209.09";
        t.crc = 0x8CFAB4EA;
        t.elf_path = "SLUS_209.09";
        t.elf_dir = "Crash_TwinSanity.rep";
        // sha256sum SLUS_209.09 (2,151,940 bytes; md5 36583bd799a803cae7b8f2a7b2ca6592 per).
        t.elf_sha256 = "8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f";
        t.main_stack_top = 0x01FFFF00u;  // top of the 32 MiB image minus the kernel reserve
        t.entry = 0x00100008;
        t.load_base = 0x00100000;
        t.load_end = 0x003DAE80;
        t.file_end = 0x0030A0B8;
        t.sdk_version = "3.0.1";
        t.boot.argv = {"cdrom0:\\SLUS_209.09;1"};
        t.boot.argv_block = 0x0030a980;
        t.boot.sbrk_break_word = 0x002ea844;
        t.boot.heap_limit_cap = 0x01F00000;
        t.boot.default_heap_base = 0x00200000;
        t.sdk_bindings_file = "hle/sdk_bindings.tsv";
        t.stream_irx_file = "STREAM.IRX";
        t.disc_image_file = "Crash_TwinSanity.rep/Crash Twinsanity.iso";
        t.quirks.xgkick_defer = true;
        t.quirks.round_sprite_mode = 1;
        t.quirks.boot_t0_mode = 0x0e82;     // FUN_001919c8: T0_COUNT=0, T0_MODE=0xe82 (OLD ee_timers.cpp:443-444)
        t.quirks.budget.anchor = 0;         // the boundary tick's T0 count (DerivedClocks::BudgetAnchorCounts)
        // The four budget defaults are the original's fallbacks in main.cpp:
        // base 8800 (:3540), step 96 (:3515), lowdraw 48 (:3575), base_low 0 (:3581).
        t.quirks.budget.base = 8800;
        t.quirks.budget.base_low = 0;
        t.quirks.budget.step = 96;
        t.quirks.budget.lowdraw = 48;
        t.save.schema_version = 4;          // every frozen anchor is a v4 file (TIMR + SNDC present)
        t.save.anchor_dir = "save_states";
        t.save.expected_hashes = "verify/restore_mhash_expected.tsv";
        t.save.manual_body_ledger = "verify/manual_body_ledger.tsv";
        t.recomp.shard_count = 32;
        t.pad.port_table = 0x003c6e90;      // scePadPortOpen (0x2c27c0) stores area/static/id here; readers gate on +0x10
        t.pad.open_flags = 0x003c6ea0;
        t.pad.static_area = 0x003c6f80;
        t.pad.ctrl_sm_entry = 0x00177168;   // the controller state machine; case = ((*(*0x309508 + 4)) >> 12) & 0x3f
        t.pad.ctrl_object_ptr = 0x00309508;
        t.gs.gparam_addr = 0x002e7ff0;      // sceGsGParam: {interlace, mode, ffmode, syncv_fn, syncv_id} words
        t.mc.client_addr = 0x003c8580;      // libmc's sceSifClientData; sceMcGetInfo (0x2c5940) gates on its +0x24 word
        return t;
    }();
    return p;
}
