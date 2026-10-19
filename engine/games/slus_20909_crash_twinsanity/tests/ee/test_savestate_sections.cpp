// SavestateSections -- unit proofs for the Phase-3 section providers and the
// title workaround bodies (no anchor files needed):
//   1 MetaArena HEAP-tail codec: SerializeState -> RestoreState -> SerializeState identical;
//   2 StateRestorer runs every validate BEFORE pre_restore_ (a THRD record
//     with a foreign ctx_size never reaches the pre-restore step);
//   3 THRD / SEMA / EVFL / INTC synthetic tables: SaveImage -> RestoreImage ->
//     SaveImage byte-identical and the live tables carry the values;
//   4 free_list_0x181560 bodies route / skip exactly as the ledger row says;
//   5 KeyRedirect::Scan repairs only a garbage key after beach was seen;
//   6 HeapSection: the HEAP payload = the original HLE-heap bookkeeping
//     (parsed structurally, skipped) + the META arena tail; an image carrying it
//     restores the arena bump/quarantine so post-restore allocations never
//     re-vend a restored cell; without the owner the bump stays 0 (the RED).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/savestate/savestate_format.h"
#include "engine/ee/hle/event_flags.h"
#include "engine/ee/hle/semaphores.h"
#include "engine/ee/hle/syscall_table.h"
#include "engine/ee/hle/thread_manager.h"
#include "engine/ee/intc/intc.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/heap_section.h"
#include "engine/ee/savestate/intc_section.h"
#include "engine/ee/savestate/kernel_sections.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ee/savestate/thread_section.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/free_list_0x181560.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/key_redirect.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++g_fail;
}

struct Rig {
    MainMemory ram;
    Scratchpad spad;
    AddressMap map{ram, spad};
    MetaArena arena;
    MemorySections mem{ram, spad, arena.Data(), arena.Size()};
    StateRestorer restorer{mem};
    ThreadSection thrd{ThreadManager::Instance()};
    KernelSections kernel{Semaphores::Instance(), EventFlags::Instance()};
    Intc intc;
    IntcSection intc_section{intc, SyscallTable::Instance()};
};

void Wire(Rig& rig) {
    rig.arena.Register(rig.map);
    rig.thrd.Register(rig.restorer.Registry());
    rig.kernel.Register(rig.restorer.Registry());
    rig.intc_section.Register(rig.restorer.Registry());
}

void TestMetaArenaCodec() {
    std::puts("MetaArenaCodec");
    MetaArena a;
    std::vector<uint32_t> live;
    for (int i = 0; i < 300; i++) live.push_back(a.Alloc());
    for (int i = 0; i < 300; i += 2) a.Free(live[i]);
    std::vector<uint8_t> first;
    a.SerializeState(first);
    MetaArena b;
    size_t consumed = 0;
    std::string err;
    Check(b.ValidateState(first.data(), first.size(), &consumed, &err) && consumed == first.size(), "ValidateState accepts the tail");
    Check(b.RestoreState(first.data(), first.size(), &consumed) && consumed == first.size(), "RestoreState consumes the tail");
    std::vector<uint8_t> second;
    b.SerializeState(second);
    Check(first == second, "SerializeState after RestoreState is byte-identical");
    Check(b.GetStats().allocs == a.GetStats().allocs && b.GetStats().frees == a.GetStats().frees &&
              b.GetStats().quarantined == a.GetStats().quarantined,
          "counters round-trip");
    Check(b.ValidateState(first.data(), first.size() - 1, &consumed, &err) == false, "a truncated tail is rejected");
}

void TestValidateBeforePreRestore() {
    std::puts("ValidateBeforePreRestore");
    ThreadManager::Instance().ResetForTest();
    Semaphores::Instance().ResetForTest();
    EventFlags::Instance().ResetForTest();
    SyscallTable::Instance().ResetForTest();
    Rig rig;
    Wire(rig);
    ps2r::savestate::FileMeta meta;
    std::vector<uint8_t> image = rig.restorer.SaveImage(meta);
    // Corrupt the THRD record: a foreign ctx_size (record 0 lives right after the count).
    ps2r::savestate::FileMeta m2;
    std::vector<ps2r::savestate::Blob> sections;
    std::string err;
    Check(ps2r::savestate::ParseStateFile(image.data(), image.size(), &m2, &sections, &err), "own image parses");
    ThreadManager::Instance().AdoptCurrentThread(1, 1);
    image = rig.restorer.SaveImage(meta);
    Check(ps2r::savestate::ParseStateFile(image.data(), image.size(), &m2, &sections, &err), "image with one thread parses");
    for (auto& b : sections)
        if (b.tag == ps2r::savestate::TAG_THRD) {
            Check(b.payload.size() >= 4 + ThreadSection::kRecordHeaderBytes, "THRD carries one record");
            const uint32_t bad = 1601;
            std::memcpy(&b.payload[4 + 72], &bad, 4);  // ctx_size field
        }
    const std::vector<uint8_t> bad_image = ps2r::savestate::SerializeStateFile(m2, sections);
    int pre = 0;
    rig.restorer.SetPreRestore([&](std::string*) { ++pre; return true; });
    Check(!rig.restorer.RestoreImage(bad_image.data(), bad_image.size(), &err), "foreign ctx_size rejects the load");
    Check(err.find("CpuContext size mismatch") != std::string::npos, "the error names the ctx_size guard");
    Check(pre == 0, "pre_restore_ never ran for the rejected file");
    Check(rig.restorer.RestoreImage(image.data(), image.size(), &err), "the intact image restores");
    Check(pre == 1, "pre_restore_ ran once for the accepted file");
}

void TestSyntheticRoundTrip() {
    std::puts("SyntheticRoundTrip");
    ThreadManager::Instance().ResetForTest();
    Semaphores::Instance().ResetForTest();
    EventFlags::Instance().ResetForTest();
    SyscallTable::Instance().ResetForTest();
    Rig rig;
    Wire(rig);
    auto main_thread = ThreadManager::Instance().AdoptCurrentThread(1, 1);
    main_thread->heap_base = 0x00400000;
    main_thread->heap_end = 0x01FFF000;
    ThreadParam p;
    p.entry = 0x00123450; p.stack = 0x01F00000; p.stack_size = 0x8000; p.priority = 40;
    const int tid = ThreadManager::Instance().CreateThread(p);
    SemaParam sp; sp.max_count = 3; sp.init_count = 2; sp.attr = 0x11;
    const int sid = Semaphores::Instance().Create(sp);
    EventFlagParam ep; ep.init_bits = 0x5; ep.attr = 0x2;
    const int eid = EventFlags::Instance().Create(ep);
    EventFlags::Instance().Lookup(eid)->bits |= 0x8;  // direct: Set() from a host thread is routed to the event queue
    rig.intc.RestoreRegisters(0x0004, 0x000C);
    std::vector<SyscallTable::KernelHandler> hs(2);
    hs[0].id = 7; hs[0].cause = 3; hs[0].pc = 0x00200000; hs[0].gp = 0; hs[0].arg = 0x1234; hs[0].flag = 1;
    hs[1].id = 9; hs[1].cause = 2; hs[1].pc = 0x00200100; hs[1].active = false;
    SyscallTable::Instance().InstallIntcHandlers(hs, 10);

    ps2r::savestate::FileMeta meta;
    const std::vector<uint8_t> first = rig.restorer.SaveImage(meta);
    // Wipe every table, restore, re-save.
    ThreadManager::Instance().ResetForTest();
    Semaphores::Instance().ResetForTest();
    EventFlags::Instance().ResetForTest();
    SyscallTable::Instance().ResetForTest();
    rig.intc.RestoreRegisters(0, 0);
    std::string err;
    Check(rig.restorer.RestoreImage(first.data(), first.size(), &err), err.empty() ? "restore ok" : err.c_str());
    const std::vector<uint8_t> second = rig.restorer.SaveImage(meta);
    Check(first == second, "SaveImage -> RestoreImage -> SaveImage is byte-identical");
    auto t = ThreadManager::Instance().Lookup(tid);
    Check(t && t->entry == 0x00123450 && t->stack_size == 0x8000 && t->priority == 40, "thread record values restored");
    auto m = ThreadManager::Instance().Lookup(1);
    Check(m && m->heap_base == 0x00400000 && m->heap_end == 0x01FFF000, "heap window fields round-trip");
    auto s = Semaphores::Instance().Lookup(sid);
    Check(s && s->count == 2 && s->max_count == 3 && s->attr == 0x11, "semaphore values restored");
    auto f = EventFlags::Instance().Lookup(eid);
    Check(f && f->bits == 0xD && f->init_bits == 0x5 && f->attr == 0x2, "event flag values restored");
    Check(rig.intc.Stat() == 0x0004 && rig.intc.Mask() == 0x000C, "INTC STAT/MASK restored");
    const auto& back = SyscallTable::Instance().IntcHandlers();
    Check(back.size() == 2 && back[0].cause == 2 && back[0].id == 9 && !back[0].active && back[1].id == 7 &&
              back[1].arg == 0x1234 && SyscallTable::Instance().NextIntcHandlerId() == 10,
          "INTC handler records (grouped by cause) + next id restored");
    Check(Semaphores::Instance().Create(sp) > sid, "id allocation continues above the restored ids");
}

void TestRequiredOwnership() {
    std::puts("RequiredOwnership");
    MainMemory ram;
    Scratchpad spad;
    MemorySections memory(ram, spad, nullptr, 0);
    StateRestorer restorer(memory);
    restorer.RequireSectionOwners({ps2r::savestate::TAG_CNTR});
    ps2r::savestate::FileMeta meta;
    auto sections = restorer.Registry().Capture();
    ps2r::savestate::Blob service;
    service.tag = ps2r::savestate::Tag('I', 'N', 'P', 'T');
    service.payload = {1, 2, 3, 4};
    sections.push_back(service);
    // Valid CRCs isolate ownership from envelope corruption. The old reader
    // discarded this required service payload before the registry saw it.
    ram.Data()[0] = 0x91;
    int pre = 0;
    restorer.SetPreRestore([&](std::string*) { ++pre; return true; });
    const auto restore = [&]() {
        const auto image = ps2r::savestate::SerializeStateFile(meta, sections);
        std::string error;
        const bool ok = restorer.RestoreImage(image.data(), image.size(), &error);
        return std::make_pair(ok, error);
    };
    const auto rejected = restore();
    Check(!rejected.first && rejected.second.find("INPT") != std::string::npos,
          "required Input state without its selected owner rejects by name");
    Check(pre == 0 && ram.Data()[0] == 0x91 && restorer.LastSections().empty(),
          "ownership rejection leaves live memory, teardown and last image untouched");
    sections.back().flags = 0;
    Check(restore().first && pre == 1, "unknown optional diagnostic state may be skipped");
    sections.back().flags = ps2r::savestate::kSectionRequired;
    sections.back().tag = ps2r::savestate::TAG_CNTR;
    Check(restore().first && pre == 2, "explicit frozen legacy exception is accepted");
    sections.back().tag = ps2r::savestate::TAG_HEAP;
    Check(!restore().first && pre == 2,
          "BUG-085: missing HEAP owner is not covered by a different legacy exception");

    ps2r::savestate::SectionSpec owned;
    owned.tag = ps2r::savestate::TAG_CNTR;
    owned.required = false;
    owned.owner = "counter fixture";
    owned.validate = [](const ps2r::savestate::Blob&, std::string*) { return false; };
    Check(restorer.Registry().Register(owned), "register an owner for the legacy exception");
    sections.back().tag = ps2r::savestate::TAG_CNTR;
    Check(!restore().first && pre == 2, "an active owner's validation cannot be bypassed by the exception");
}

void TestFreeListHook() {
    std::puts("FreeListHook");
    MetaArena arena;
    int orig_calls = 0;
    auto orig = [&](CpuContext&, uint32_t) { ++orig_calls; };
    auto alloc = free_list_0x181560::AllocHook(arena, orig);
    auto free_ = free_list_0x181560::FreeHook(arena, orig);
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[4].UL[0] = 0x20;                    // size
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x80202100;             // ra inside the pool code (kseg0 form)
    alloc(ctx, MetaArena::kAllocSite);
    const uint32_t block = ctx.cpuRegs.GPR.r[2].UL[0];
    Check(MetaArena::Contains(block) && orig_calls == 0 && ctx.cpuRegs.pc == 0x80202100, "routed alloc: v0 in the arena, generated body skipped, pc = ra");
    ctx.cpuRegs.GPR.r[4].UL[0] = 0x40;                    // too large: falls through
    alloc(ctx, MetaArena::kAllocSite);
    Check(orig_calls == 1, "unrouted size falls through to the generated body");
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x00150000;             // ra outside the pool code
    ctx.cpuRegs.GPR.r[4].UL[0] = 0x20;
    alloc(ctx, MetaArena::kAllocSite);
    Check(orig_calls == 2, "unrouted caller falls through");
    ctx.cpuRegs.GPR.r[4].UL[0] = block;
    free_(ctx, MetaArena::kFreeSite);
    Check(orig_calls == 2 && arena.GetStats().frees == 1, "arena pointer freed in the arena, generated free skipped");
    ctx.cpuRegs.GPR.r[4].UL[0] = 0x00300000;
    free_(ctx, MetaArena::kFreeSite);
    Check(orig_calls == 3, "slab pointer falls through to the generated free");
    int installed = 0;
    std::vector<uint32_t> addrs;
    installed = free_list_0x181560::InstallHooks(
        arena, [&](uint32_t) { return free_list_0x181560::Body(orig); },
        [&](uint32_t addr, free_list_0x181560::Body) { addrs.push_back(addr); return true; });
    Check(installed == 2 && addrs.size() == 2 && addrs[0] == 0x00181560 && addrs[1] == 0x00181590,
          "InstallHooks binds exactly the two ledger addresses");
    // hle.meta_arena=off: the allocation hook is NOT installed (FUN_00181560 runs
    // as recompiled -- the page pool holds the nodes, as the console does); the
    // free body alone stays, for pointers a restored image already holds in the arena.
    addrs.clear();
    installed = free_list_0x181560::InstallHooks(
        arena, [&](uint32_t) { return free_list_0x181560::Body(orig); },
        [&](uint32_t addr, free_list_0x181560::Body) { addrs.push_back(addr); return true; }, /*route_allocations=*/false);
    Check(installed == 1 && addrs.size() == 1 && addrs[0] == 0x00181590,
          "route_allocations=false installs only the free body (0x181590), never the alloc site");
}

void TestKeyRedirect() {
    std::puts("KeyRedirect");
    MainMemory ram;
    KeyRedirect kr(ram);
    const uint32_t obj = 0x00500000, global_str = 0x00600000, own_str = 0x00610000, garbage = 0x00620000;
    std::memcpy(ram.Data() + global_str, "Levels\\Earth\\Hub\\Beach", 23);
    std::memcpy(ram.Data() + own_str, "levels\\earth\\hub\\beach", 23);
    std::memcpy(ram.Data() + garbage, "d\0\0\0", 4);
    std::memcpy(ram.Data() + KeyRedirect::kGlobalBeachPtr, &global_str, 4);
    std::memcpy(ram.Data() + KeyRedirect::kLevelObjectPtr, &obj, 4);
    uint32_t key = garbage;
    std::memcpy(ram.Data() + obj + KeyRedirect::kKeyOffset, &key, 4);
    Check(!kr.Scan(), "garbage before beach was ever seen: no repair");
    key = own_str;
    std::memcpy(ram.Data() + obj + KeyRedirect::kKeyOffset, &key, 4);
    Check(!kr.Scan() && kr.GetStats().seen_beach, "the object's own beach key is seen and left alone");
    key = garbage;
    std::memcpy(ram.Data() + obj + KeyRedirect::kKeyOffset, &key, 4);
    Check(kr.Scan(), "garbage after beach was seen: repaired");
    std::memcpy(&key, ram.Data() + obj + KeyRedirect::kKeyOffset, 4);
    Check(key == global_str, "the pointer now names the stable global string");
    std::memcpy(ram.Data() + 0x00630000, "Levels\\Ice\\Hub", 15);
    key = 0x00630000;
    std::memcpy(ram.Data() + obj + KeyRedirect::kKeyOffset, &key, 4);
    Check(!kr.Scan(), "another levels\\ path is a legitimate key: untouched");
    for (int i = 0; i < 512; i++) kr.NoteWrite();
    Check(kr.GetStats().scans == 4 + 2, "NoteWrite scans every 256th write (4 explicit scans + writes 0 and 256)");
}
}  // namespace

HeapSection::TailCodec ArenaCodec(MetaArena& arena) {
    return HeapSection::TailCodec{
        [&arena](std::vector<uint8_t>& out) { arena.SerializeState(out); },
        [&arena](const uint8_t* p, size_t len, size_t* consumed, std::string* err) {
            return arena.ValidateState(p, len, consumed, err);
        },
        [&arena](const uint8_t* p, size_t len, size_t* consumed) { return arena.RestoreState(p, len, consumed); }};
}

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4);
}

void TestHeapSection() {
    std::puts("HeapSection");
    // The original tree's prefix grammar: bounds, n_free {addr,size}*, n_alloc {addr,size}*.
    std::vector<uint8_t> prefix;
    for (uint32_t w : {0x01C00000u, 0x02000000u, 0x01C00000u, 0x01FFFFF0u}) PutU32(prefix, w);
    PutU32(prefix, 2); PutU32(prefix, 0x01F00000); PutU32(prefix, 0x100); PutU32(prefix, 0x01F10000); PutU32(prefix, 0x40);
    PutU32(prefix, 1); PutU32(prefix, 0x01FFF000); PutU32(prefix, 0x1000);
    size_t len = 0;
    std::string err;
    Check(HeapSection::PrefixLength(prefix, &len, &err) && len == prefix.size(), "the original prefix parses to its own length");
    std::vector<uint8_t> truncated(prefix.begin(), prefix.end() - 4);
    Check(!HeapSection::PrefixLength(truncated, &len, &err), "a truncated block list is rejected");
    std::vector<uint8_t> empty;
    HeapSection::EmitEmptyPrefix(empty);
    Check(HeapSection::PrefixLength(empty, &len, &err) && len == 24, "the empty prefix this runtime writes is 24 bytes of the same grammar");

    // A saver arena with 644 cells vended (the coldboot probe's count) and a few freed.
    MetaArena saver;
    std::vector<uint32_t> live;
    for (int i = 0; i < 644; i++) live.push_back(saver.Alloc());
    for (int i = 0; i < 10; i++) saver.Free(live[i * 7]);
    std::vector<uint8_t> payload(prefix);
    saver.SerializeState(payload);

    // The owner over a fresh arena: validate + restore reproduce the bookkeeping.
    MetaArena arena;
    HeapSection section(ArenaCodec(arena));
    Check(section.Validate(payload, &err), ("the HEAP payload validates: " + err).c_str());
    Check(section.Restore(payload, &err), "the HEAP payload restores");
    Check(arena.GetStats().bump_used == 644 && arena.GetStats().quarantined == 10, "bump 644 / quarantine 10 restored");
    const uint32_t next = arena.Alloc();
    Check(next == MetaArena::kBase + 644 * MetaArena::kBlock + MetaArena::kSkew,
          "the first post-restore allocation is cell 644, not cell 0 (the restored title's live job)");
    std::vector<uint8_t> again;
    section.Serialize(again);
    std::vector<uint8_t> expect;
    HeapSection::EmitEmptyPrefix(expect);
    arena.SerializeState(expect);
    Check(again == expect, "Serialize = empty prefix + the arena tail");
    std::vector<uint8_t> trailing(payload);
    trailing.push_back(0);
    Check(!section.Validate(trailing, &err), "trailing bytes after the tail are rejected");

    // Through the restorer: the registry without the owner skips HEAP (bump stays 0 = the
    // BUG-085 shape); with the owner registered the same image restores the bump.
    for (int with_owner = 0; with_owner < 2; with_owner++) {
        MainMemory ram;
        Scratchpad spad;
        MetaArena a2;
        MemorySections mem{ram, spad, a2.Data(), a2.Size()};
        StateRestorer restorer{mem};
        HeapSection hs(ArenaCodec(a2));
        if (with_owner) hs.Register(restorer.Registry());
        ps2r::savestate::FileMeta meta;
        std::vector<ps2r::savestate::Blob> blobs;
        for (uint32_t tag : {ps2r::savestate::TAG_RAM, ps2r::savestate::TAG_SPAD, ps2r::savestate::TAG_META}) {
            ps2r::savestate::Blob b;
            b.tag = tag;
            b.payload.assign(tag == ps2r::savestate::TAG_RAM ? ram.Size() : tag == ps2r::savestate::TAG_SPAD ? spad.Size() : a2.Size(), 0);
            blobs.push_back(std::move(b));
        }
        ps2r::savestate::Blob heap;
        heap.tag = ps2r::savestate::TAG_HEAP;
        heap.payload = payload;
        blobs.push_back(std::move(heap));
        const std::vector<uint8_t> image = ps2r::savestate::SerializeStateFile(meta, blobs);
        Check(restorer.RestoreImage(image.data(), image.size(), &err), ("image with HEAP restores: " + err).c_str());
        Check(a2.GetStats().bump_used == (with_owner ? 644u : 0u),
              with_owner ? "with the owner: bump 644 after RestoreImage" : "without the owner: bump 0 (the pre-fix shape)");
        Check(hs.RestoredCount() == (with_owner ? 1u : 0u), "RestoredCount follows the registration");
    }
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "codec") TestMetaArenaCodec();
    if (which == "all" || which == "order") TestValidateBeforePreRestore();
    if (which == "all" || which == "roundtrip") TestSyntheticRoundTrip();
    if (which == "all" || which == "order") TestRequiredOwnership();
    if (which == "all" || which == "freelist") TestFreeListHook();
    if (which == "all" || which == "keyredirect") TestKeyRedirect();
    if (which == "all" || which == "heap") TestHeapSection();
    std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
