// CutsceneFastForward: the decision on a fake title RAM.
//   * factor 1 (off) / no reader: never any extra field, census "off";
//   * factor 4: R2 held while the video controller's state is Playing (3) -> 3 extra
//     fields per boundary, counted; released, or Queued/Paused/Finished, or no
//     controller yet (null pointer), or an unreadable word -> 0;
//   * the factor is clamped to kFactorMax; the census names factor/boundaries/fields.
#include <cstdio>
#include <map>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/qol/cutscene_fast_forward.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

using title::CutsceneFastForward;

struct TestCutsceneFastForward {
    std::map<uint32_t, uint32_t> ram;
    CutsceneFastForward::Read32 reader() {
        return [this](uint32_t a, uint32_t* v) { auto it = ram.find(a); if (it == ram.end()) return false; *v = it->second; return true; };
    }
};

int main() {
    constexpr uint32_t kController = 0x00a01000u;
    {
        TestCutsceneFastForward t;
        t.ram[CutsceneFastForward::kVideoControllerPtr] = kController;
        t.ram[kController + CutsceneFastForward::kStateOffset] = CutsceneFastForward::kStatePlaying;
        CutsceneFastForward off(1, t.reader());
        off.NoteButtons(CutsceneFastForward::kHoldButton);
        CHECK(!off.Enabled() && off.ExtraFields() == 0 && off.Census() == "off", "factor_1_is_off");
        CutsceneFastForward noreader(4, {});
        noreader.NoteButtons(CutsceneFastForward::kHoldButton);
        CHECK(!noreader.Enabled() && noreader.ExtraFields() == 0, "no_reader_is_off");
    }
    {
        TestCutsceneFastForward t;
        CutsceneFastForward ff(4, t.reader());
        CHECK(ff.Enabled() && ff.Factor() == 4, "enabled_factor_4");
        ff.NoteButtons(CutsceneFastForward::kHoldButton);
        CHECK(ff.ExtraFields() == 0, "no_controller_word_yet_0");        // the pointer word unreadable
        t.ram[CutsceneFastForward::kVideoControllerPtr] = 0;
        CHECK(ff.ExtraFields() == 0 && !ff.Playing(), "null_controller_0");
        t.ram[CutsceneFastForward::kVideoControllerPtr] = kController;
        t.ram[kController + CutsceneFastForward::kStateOffset] = 1;        // Queued
        CHECK(ff.ExtraFields() == 0, "queued_0");
        t.ram[kController + CutsceneFastForward::kStateOffset] = CutsceneFastForward::kStatePlaying;
        CHECK(ff.Playing() && ff.ExtraFields() == 3 && ff.ExtraFields() == 3, "playing_held_3_extra_per_boundary");
        ff.NoteButtons(static_cast<uint16_t>(1u << 14));                  // CROSS, not R2
        CHECK(!ff.Held() && ff.ExtraFields() == 0, "released_0");
        ff.NoteButtons(static_cast<uint16_t>(CutsceneFastForward::kHoldButton | (1u << 14)));
        t.ram[kController + CutsceneFastForward::kStateOffset] = 4;        // Paused
        CHECK(ff.ExtraFields() == 0, "paused_0");
        t.ram[kController + CutsceneFastForward::kStateOffset] = CutsceneFastForward::kStatePlaying;
        CHECK(ff.ExtraFields() == 3, "playing_again_3");
        CHECK(ff.Boundaries() == 3 && ff.Fields() == 9 && ff.Census() == "factor=4,boundaries=3,fields=9,controller=0xa01000,state=3", "census_counts");
        t.ram[CutsceneFastForward::kVideoControllerPtr] = 0x7fffff00u;     // not RAM
        CHECK(ff.ExtraFields() == 0, "pointer_outside_ram_0");
    }
    {
        TestCutsceneFastForward t;
        CutsceneFastForward big(1000, t.reader());
        CHECK(big.Factor() == CutsceneFastForward::kFactorMax, "factor_clamped");
    }
    if (g_fail) { std::printf("test_cutscene_fast_forward: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("test_cutscene_fast_forward: all checks passed\n");
    return 0;
}
