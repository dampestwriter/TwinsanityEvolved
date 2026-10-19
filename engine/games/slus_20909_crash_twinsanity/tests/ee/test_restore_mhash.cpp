// RestoreMhashAnchors -- the restore.mhash gate.
// For each of the 11 frozen anchor states (OLD/save_states, read-only): parse
// the envelope, restore RAM0/SPAD/META into NEW's MainMemory / Scratchpad /
// MetaArena through StateRestorer, hash the restored memory exactly as
// MemoryCheckpointer does, and require it to equal (a) FNV-1a64 over the
// file's raw payloads and (b) verify/state_hash.py's number from the frozen
// table; then re-serialize the three memory sections and require the bytes
// to equal the file's payloads. Every Phase-3 provider is registered (THRD,
// SEMA, EVFL, TIMR, INTC): each anchor's THRD records must carry ctx_size ==
// sizeof(CpuContext) (the provider rejects otherwise), and EVERY owned section
// must re-serialize byte-identical after the restore.
// argv: <expected.tsv> <anchor_dir>.
// Exit 77 (SKIP) when the anchor directory is absent.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/core/hash/hash.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ee/savestate/thread_section.h"
#include "engine/ee/savestate/kernel_sections.h"
#include "engine/ee/savestate/timer_section.h"
#include "engine/ee/savestate/intc_section.h"
#include "engine/ee/hle/thread_manager.h"
#include "engine/ee/hle/semaphores.h"
#include "engine/ee/hle/event_flags.h"
#include "engine/ee/hle/syscall_table.h"
#include "engine/ee/intc/intc.h"
#include "engine/ee/intc/ee_timers.h"
#include "engine/ee/intc/timer_codec.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/title/title_profile.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace {
struct Row { std::string name; uint64_t hash; };

std::vector<Row> ReadTable(const std::string& path) {
    std::vector<Row> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Row r; std::string h;
        ss >> r.name >> h;
        r.hash = std::stoull(h, nullptr, 16);
        rows.push_back(r);
    }
    return rows;
}

const ps2r::savestate::Blob* Find(const std::vector<ps2r::savestate::Blob>& v, uint32_t tag) {
    for (const auto& b : v) if (b.tag == tag) return &b;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test_restore_mhash <expected.tsv> <anchor_dir>\n"); return 2; }
    const std::vector<Row> rows = ReadTable(argv[1]);
    if (rows.empty()) { std::fprintf(stderr, "no rows in %s\n", argv[1]); return 1; }
    {
        FILE* probe = std::fopen((std::string(argv[2]) + "/" + rows[0].name).c_str(), "rb");
        if (!probe) { std::printf("SKIP: anchor dir %s absent\n", argv[2]); return 77; }
        std::fclose(probe);
    }
    MainMemory ram;
    Scratchpad spad;
    AddressMap map(ram, spad);
    MetaArena arena;
    arena.Register(map);
    MemorySections mem(ram, spad, arena.Data(), arena.Size());
    StateRestorer restorer(mem);
    // Phase-3 providers in the original's section order (DoSave savestate.cpp:1123-1215 /
    // DoLoad :1746-1926): memory, THRD, SEMA, EVFL, INTC, then TIMR last (after the counters).
    ThreadSection thrd(ThreadManager::Instance());
    KernelSections kernel(Semaphores::Instance(), EventFlags::Instance());
    EmuDomain dom;
    DerivedClocks clocks(dom);
    Intc intc;
    EeTimers timers(clocks, intc);
    TimerSection timr(timers, ActiveTitleProfile().quirks.boot_t0_mode);
    IntcSection intc_section(intc, SyscallTable::Instance());
    thrd.Register(restorer.Registry());
    kernel.Register(restorer.Registry());
    intc_section.Register(restorer.Registry());
    timr.Register(restorer.Registry());
    int pre_restore_calls = 0;
    restorer.SetPreRestore([&](std::string*) { ++pre_restore_calls; return true; });

    int failures = 0;
    size_t anchors = 0;
    std::printf("| anchor | restore.mhash (NEW) | state_hash.py | payload FNV | re-serialize |\n|---|---|---|---|---|\n");
    for (const Row& r : rows) {
        std::string err;
        const std::string path = std::string(argv[2]) + "/" + r.name;
        // The table also approves boot imports that live outside the anchor
        // directory (the coldboot probe under verify/baseline/coldboot/probe);
        // only rows present in <anchor_dir> are this gate's anchors.
        if (FILE* present = std::fopen(path.c_str(), "rb")) std::fclose(present);
        else { std::printf("| %s | not in anchor dir, skipped | | | |\n", r.name.c_str()); continue; }
        ++anchors;
        if (!restorer.RestoreFile(path, &err)) {
            std::printf("| %s | RESTORE FAILED: %s | | | |\n", r.name.c_str(), err.c_str());
            ++failures;
            continue;
        }
        const uint64_t restored = restorer.RestoredMemoryHash();
        const auto& sections = restorer.LastSections();
        uint64_t payload = ps2r::hash::kFnv1a64Offset;
        const uint32_t tags[3] = {ps2r::savestate::TAG_RAM, ps2r::savestate::TAG_SPAD, ps2r::savestate::TAG_META};
        bool reser_ok = true;
        const std::vector<ps2r::savestate::Blob> captured = restorer.Registry().Capture();
        for (uint32_t t : tags) {
            const auto* b = Find(sections, t);
            const auto* c = Find(captured, t);
            if (!b || !c) { reser_ok = false; continue; }
            payload = ps2r::hash::Fnv1a64(b->payload.data(), b->payload.size(), payload);
            reser_ok = reser_ok && c->payload.size() == b->payload.size() &&
                       std::memcmp(c->payload.data(), b->payload.data(), b->payload.size()) == 0;
        }
        // Every owned section (THRD/SEMA/EVFL/INTC/TIMR too) re-serializes byte-identical.
        bool all_ok = true;
        int owned = 0;
        for (const auto& b : sections) {
            if (!restorer.Registry().Has(b.tag)) continue;
            ++owned;
            const auto* c = Find(captured, b.tag);
            bool same = c && c->payload.size() == b.payload.size() &&
                        std::memcmp(c->payload.data(), b.payload.data(), b.payload.size()) == 0;
            if (!same && c && b.tag == ps2r::savestate::TAG_TIMR) {
                // EeTimers::Restore rebases baseline_tick to the domain's now
                // (the original restored TIMR after CNTR for the same reason);
                // every other field must round-trip exactly.
                TimerCodec::Regs file[4], again[4];
                same = TimerCodec::Decode(b.payload.data(), b.payload.size(), file) &&
                       TimerCodec::Decode(c->payload.data(), c->payload.size(), again);
                for (int i = 0; same && i < 4; ++i)
                    same = file[i].mode == again[i].mode && file[i].comp == again[i].comp &&
                           file[i].hold == again[i].hold && file[i].count_base == again[i].count_base &&
                           file[i].pending_ovf == again[i].pending_ovf && file[i].pending_cmp == again[i].pending_cmp &&
                           again[i].baseline_tick == dom.DomainTick();
            }
            if (!same) {
                all_ok = false;
                std::printf("  section %s differs after re-serialize (%zu vs %zu bytes)\n",
                            ps2r::savestate::TagName(b.tag).c_str(), b.payload.size(), c ? c->payload.size() : 0);
            }
        }
        std::vector<ThreadSection::Record> recs;
        std::string perr;
        const auto* thrd_blob = Find(sections, ps2r::savestate::TAG_THRD);
        const bool thrd_ok = thrd_blob && ThreadSection::Parse(thrd_blob->payload, recs, &perr) &&
                             recs.size() == thrd.RestoredCount() && recs.size() == ThreadManager::Instance().SnapshotTable().size();
        const bool ok = restored == r.hash && restored == payload && reser_ok && all_ok && thrd_ok;
        if (!ok) ++failures;
        std::printf("| %s | 0x%016llx | 0x%016llx | 0x%016llx | %s | %s | THRD %zu records ctx_size==%zu, %d sections round-trip (TIMR modulo the baseline rebase)\n",
                    r.name.c_str(), static_cast<unsigned long long>(restored),
                    static_cast<unsigned long long>(r.hash), static_cast<unsigned long long>(payload),
                    reser_ok ? "identical" : "DIFFERS", ok ? "OK" : "MISMATCH", recs.size(), sizeof(CpuContext),
                    all_ok ? owned : -1);
    }
    std::printf("restore.mhash: %zu/%zu anchors match (pre_restore ran %d times, once per accepted file)\n",
                anchors - failures, anchors, pre_restore_calls);
    if (anchors == 0 || pre_restore_calls != static_cast<int>(anchors)) ++failures;
    return failures ? 1 : 0;
}
