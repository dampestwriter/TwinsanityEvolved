// Vu0RegistryCensus: the VU0 macro-program census mechanism (Vu0Registry) on
// the title table (Vu0Programs) -- lookup by (vu, pc), first-call / periodic /
// unknown events, the fingerprint check (the libsin/cos row's fingerprint IS
// the LibVu0SinCos SDK bundle pair; a contradicting image warns exactly once
// per entry), and the table's shape (15 entries, 1 subroutine, kinds).
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/vu/vu0/lib_vu0_sin_cos.h"
#include "engine/vu/vu0/vu0_registry.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu0_programs.h"

static int g_fail = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                               \
        }                                                           \
    } while (0)

namespace {

using Event = Vu0Registry::Event;
using Kind = Vu0Registry::Kind;

void PutBundle(std::vector<uint8_t>& micro, uint32_t pc, uint32_t lower, uint32_t upper) {
    std::memcpy(micro.data() + pc, &lower, 4);
    std::memcpy(micro.data() + pc + 4, &upper, 4);
}

}  // namespace

int main() {
    Vu0Registry reg;
    Vu0Programs::Register(reg);

    // Table shape.
    size_t n_entries = 0, n_subs = 0;
    Vu0Programs::Entries(n_entries);
    Vu0Programs::Subroutines(n_subs);
    CHECK(n_entries == 15);
    CHECK(n_subs == 1);
    CHECK(reg.Entries().size() == 15);
    CHECK(reg.Subroutines().size() == 1);
    size_t hle = 0, interp = 0, unimpl = 0;
    for (const Vu0Registry::Descriptor& d : reg.Entries()) {
        if (d.kind == Kind::Hle) ++hle;
        if (d.kind == Kind::Interpreted) ++interp;
        if (d.kind == Kind::Unimplemented) ++unimpl;
    }
    CHECK(hle == 1 && interp == 1 && unimpl == 13);

    // Lookup rules.
    const Vu0Registry::Descriptor* sincos = reg.Lookup(0, LibVu0SinCos::kEntryPc);
    CHECK(sincos != nullptr && sincos->kind == Kind::Hle);
    CHECK(reg.Lookup(1, LibVu0SinCos::kEntryPc) == nullptr);  // keyed per unit
    CHECK(reg.Lookup(1, 0) != nullptr && reg.Lookup(1, 0)->kind == Kind::Interpreted);
    CHECK(reg.LookupSubroutine(1, 0x1720) != nullptr);
    CHECK(reg.LookupSubroutine(0, 0x1720) == nullptr);
    CHECK(std::strcmp(Vu0Registry::KindName(Kind::Hle), "HLE") == 0);
    CHECK(std::strcmp(Vu0Registry::KindName(Kind::Interpreted), "INTERP") == 0);
    CHECK(std::strcmp(Vu0Registry::KindName(Kind::Unimplemented), "UNIMPL") == 0);

    // The libsin/cos row's fingerprint is the SDK bundle pair -- a micro image
    // carrying LibVu0SinCos::kFingerprint matches it (no mismatch).
    std::vector<uint8_t> micro(4096, 0);
    for (int i = 0; i < 3; ++i)
        PutBundle(micro, LibVu0SinCos::kEntryPc + 8u * static_cast<uint32_t>(i), LibVu0SinCos::kFingerprint[i][0],
                  LibVu0SinCos::kFingerprint[i][1]);
    CHECK(LibVu0SinCos::Matches(micro.data(), micro.size(), LibVu0SinCos::kEntryPc));
    Vu0Registry::Note n = reg.NoteEntry(0, LibVu0SinCos::kEntryPc, micro.data(), micro.size());
    CHECK(n.event == Event::FirstKnown);
    CHECK(n.descriptor == sincos);
    CHECK(n.call_number == 1);
    CHECK(!n.fingerprint_mismatch);
    CHECK(n.live_lo == sincos->fingerprint_lo && n.live_hi == sincos->fingerprint_hi);

    // Repeat calls: silent until the sparse marks.
    for (uint64_t c = 2; c <= 1000; ++c) {
        n = reg.NoteEntry(0, LibVu0SinCos::kEntryPc, micro.data(), micro.size());
        CHECK(n.event == (c == 1000 ? Event::Periodic : Event::None));
        CHECK(!n.fingerprint_mismatch);
    }
    CHECK(n.call_number == 1000);

    // Unknown entry: first call reports, later calls are silent.
    n = reg.NoteEntry(0, 0x0f00, micro.data(), micro.size());
    CHECK(n.event == Event::FirstUnknown && n.descriptor == nullptr);
    n = reg.NoteEntry(0, 0x0f00, micro.data(), micro.size());
    CHECK(n.event == Event::None);

    // Fingerprint mismatch: a different program at 0x790 warns ONCE, the first
    // call mark still fires, and an unchecked (0/0) row never warns.
    Vu0Registry reg2;
    Vu0Programs::Register(reg2);
    n = reg2.NoteEntry(0, 0x0790, micro.data(), micro.size());  // zeros != declared fingerprint
    CHECK(n.event == Event::FirstKnown);
    CHECK(n.fingerprint_mismatch);
    n = reg2.NoteEntry(0, 0x0790, micro.data(), micro.size());
    CHECK(n.event == Event::None && !n.fingerprint_mismatch);
    n = reg2.NoteEntry(0, 0x0500, micro.data(), micro.size());  // 0/0 = unchecked
    CHECK(n.event == Event::FirstKnown && !n.fingerprint_mismatch);
    // The matching bytes at 0x790 never warn.
    Vu0Registry reg3;
    Vu0Programs::Register(reg3);
    const Vu0Registry::Descriptor* concat = reg3.Lookup(0, 0x0790);
    CHECK(concat != nullptr);
    std::memcpy(micro.data() + 0x790, &concat->fingerprint_lo, 8);
    std::memcpy(micro.data() + 0x790 + 8, &concat->fingerprint_hi, 8);
    n = reg3.NoteEntry(0, 0x0790, micro.data(), micro.size());
    CHECK(n.event == Event::FirstKnown && !n.fingerprint_mismatch);
    // An entry past the image end reads no fingerprint (and stays a plain note).
    n = reg3.NoteEntry(0, static_cast<uint32_t>(micro.size()) - 8, micro.data(), micro.size());
    CHECK(n.event == Event::FirstUnknown && n.live_lo == 0 && n.live_hi == 0);

    if (g_fail) {
        std::printf("Vu0RegistryCensus FAIL (%d)\n", g_fail);
        return 1;
    }
    std::printf("Vu0RegistryCensus PASS (entries=%zu subroutines=%zu)\n", reg.Entries().size(),
                reg.Subroutines().size());
    return 0;
}
