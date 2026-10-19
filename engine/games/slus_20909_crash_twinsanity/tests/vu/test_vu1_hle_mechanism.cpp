// Vu1HleMechanism -- the engine's HLE mechanism on synthetic data and on one
// recorded corpus case: Vu1Registry lookup rules (start_pc filter, exact
// image first-match, first variant match in registration order, wildcard),
// Vu1ImageAnalyzer (extent, hashes, the learned prefix, the chain walk to an
// exit stub / unterminated), KickLowering's one-slot-per-kick raw tier, and
// Vu1HleRuntime::Hook through VuTierDispatch on a fresh unit: the kicks,
// the data memory and TPC equal the direct program call plus the family exit,
// a MISS leaves the unit to the tier untouched, the census counts. With a
// second case -- a material pass WITH its register block (`node.w=1`, two kicks:
// the A+D block then the geometry) -- the configured lowering delivers both
// direct (unsupported=0, lowered_state=1) and the drained sink stream equals
// the raw route's for the same kicks.
// Every further argument (a case or a directory of cases) joins the lowering
// parity sweep: direct vs raw register-op stream identical per case, no kick
// of a library program left unsupported.
// Args: <corpus case> [<material-with-registers case> [<case | dir>...]] (SKIP when absent).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/gif_regs.h"
#include "engine/vu/hle/kick_lowering.h"
#include "engine/vu/hle/vu1_hle_runtime.h"
#include "engine/vu/hle/vu1_image_analyzer.h"
#include "engine/vu/hle/vu1_registry.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu1_case.h"
#include "engine/vu/vu_image_hash.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_library.h"
#include "tools/vu1_replay/xgk_capture_sink.h"

static int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

namespace {

bool FnA(const Vu1HleInput&, Vu1HleOutput&) { return true; }
bool FnB(const Vu1HleInput&, Vu1HleOutput&) { return true; }
bool FnC(const Vu1HleInput&, Vu1HleOutput&) { return true; }
bool FnW(const Vu1HleInput&, Vu1HleOutput&) { return true; }

void RegistryRules() {
    // A synthetic library: variant 0x11 twice (order matters), an exact image
    // row, a pc-pinned row, a wildcard.
    static const Vu1LibraryEntry kE[] = {
        {0, 0x11, "first_variant", 0, FnA, nullptr, 0},
        {0, 0x11, "second_variant", 0, FnB, nullptr, 0},
        {0x77, 0, "exact_image", kVu1AnyEntry, FnC, nullptr, 0},
        {0, 0x22, "pinned_pc", 0x100, FnA, nullptr, 0},
        {0, 0, "wildcard", kVu1AnyEntry, FnW, nullptr, 0},
    };
    const Vu1ProgramLibrary lib = {{0x10, 1, {2, 3}, 4}, kE, 5};
    Vu1Registry r(lib);
    CHECK(r.Entries().size() == 5);
    CHECK(r.DistinctVariantHashes() == 2);  // {0x11, 0x22}: the wildcard row carries none
    CHECK(r.LayoutEntries() == 0);
    const Vu1LibraryEntry* e = r.Lookup(0x1234, 0x11, 0);
    CHECK(e && std::strcmp(e->name, "first_variant") == 0);  // registration order wins
    e = r.Lookup(0x77, 0x11, 0);
    CHECK(e && std::strcmp(e->name, "exact_image") == 0);  // exact image beats variant
    e = r.Lookup(0x1234, 0x22, 0);
    CHECK(e && std::strcmp(e->name, "wildcard") == 0);  // pinned pc filtered -> wildcard
    e = r.Lookup(0x1234, 0x22, 0x100);
    CHECK(e && std::strcmp(e->name, "pinned_pc") == 0);
    e = r.Lookup(0x1234, 0x33, 0x200);
    CHECK(e && std::strcmp(e->name, "wildcard") == 0);
    Vu1Registry r2({{0x10, 1, {2, 3}, 4}, kE, 4});  // no wildcard
    CHECK(r2.Lookup(0x1234, 0x33, 0) == nullptr);
    r2.Append(kE[4]);
    CHECK(r2.Lookup(0x1234, 0x33, 0) != nullptr);
}

void AnalyzerRules() {
    // Signature: variant base 0x40, dispatcher op 0xAABBCCDD, exit stubs 0x8 / 0x10.
    const Vu1FamilySignature sig = {0x40, 0xAABBCCDDu, {0x8, 0x10}, 0x18};
    Vu1ImageAnalyzer an(sig);
    static uint8_t micro[kVu1HleMicroSize], data[kVu1HleDataSize];
    std::memset(micro, 0, sizeof(micro));
    std::memset(data, 0, sizeof(data));
    // Not a dispatcher: word 0 != op.
    Vu1ImageInfo i0 = an.Analyze(micro, data);
    CHECK(i0.extent == 0 && i0.variant_hash == 0 && !i0.prefix_is_family && !i0.dispatch_valid);
    CHECK(i0.dispatch_chain.empty());
    CHECK(i0.image_hash == VuImageHash::Fast(micro, kVu1HleMicroSize));
    // A dispatcher image with bytes up to 0x63 -> extent rounds to the bundle (0x68).
    const uint32_t op = 0xAABBCCDDu;
    std::memcpy(micro, &op, 4);
    micro[0x63] = 1;
    // Chain: data[3].x = node 5; node 5 = {pc 0x40/8 = 8, next 6}; node 6 = {0x10/8 = 2 (exit)}.
    uint16_t v = 5;
    std::memcpy(data + 3 * 16, &v, 2);
    v = 8;
    std::memcpy(data + 5 * 16, &v, 2);
    v = 6;
    std::memcpy(data + 5 * 16 + 8, &v, 2);
    v = 2;
    std::memcpy(data + 6 * 16, &v, 2);
    Vu1ImageInfo i1 = an.Analyze(micro, data);
    CHECK(i1.extent == 0x68);
    CHECK(i1.prefix_hash == VuImageHash::Fast(micro, 0x40));
    CHECK(i1.variant_hash == VuImageHash::Fast(micro + 0x40, 0x68 - 0x40));
    CHECK(i1.prefix_is_family);  // the first dispatcher image learns the prefix
    CHECK(i1.dispatch_valid && i1.dispatch_chain.size() == 2 && i1.dispatch_chain[0] == 0x40 &&
          i1.dispatch_chain[1] == 0x10);
    // A second dispatcher image with a different prefix is "other"; a chain
    // pointing past micro memory is unterminated.
    micro[8] = 0x55;
    v = 0x1000;  // pc 0x8000 >= micro size
    std::memcpy(data + 6 * 16, &v, 2);
    Vu1ImageInfo i2 = an.Analyze(micro, data);
    CHECK(!i2.prefix_is_family);
    CHECK(!i2.dispatch_valid && i2.dispatch_chain.size() == 2);
    CHECK(Vu1ImageAnalyzer::ShortHash(0x0123456789abcdefull) == "01234567");
}

struct TestVu1HleMechanism {
    XgkCaptureSink sink;
    GifRegs regs;
    GifParser parser;
    GifArbiter arbiter;
    TestVu1HleMechanism() : parser(sink, regs), arbiter(parser) { sink.Attach(arbiter); }
};

void RuntimeOnCase(const char* path) {
    static Vu1Case c;
    if (!vu1_case::ReadCase(path, c)) {
        std::printf("SKIP: corpus case absent (%s)\n", path);
        return;
    }
    const Vu1ProgramLibrary& lib = Vu1Library::Get();
    Vu1ImageAnalyzer an(lib);
    const Vu1ImageInfo info = an.Analyze(c.micro_mem, c.data_mem);
    Vu1Registry reg(lib);
    const Vu1LibraryEntry* e = reg.Lookup(info, c.start_pc);
    CHECK(e != nullptr);
    if (!e) return;

    // The direct program call (what the corpus tool compares).
    Vu1HleInput in{};
    in.micro = c.micro_mem;
    in.data = c.data_mem;
    in.start_pc = c.start_pc;
    in.top = c.vif1_top;
    in.tops = c.vif1_tops;
    in.itop = c.vif1_itop;
    in.itops = c.vif1_itops;
    in.regs = &c.regs;
    in.image_hash = info.image_hash;
    in.info = &info;
    Vu1HleOutput direct;
    CHECK(e->fn(in, direct));
    CHECK(!direct.kicks.empty());

    // The hook path: fresh unit, library installed, hle_enabled.
    TestVu1HleMechanism rig;
    VuUnit vu(1, &rig.arbiter);
    vu.LoadCase(c);
    Vu1HleRuntime rt(lib, rig.arbiter);
    rt.SetVifTops((uint16_t)c.vif1_tops, (uint16_t)c.vif1_itops);
    vu.dispatch().SetHleHook(&Vu1HleRuntime::Hook, &rt);
    vu.dispatch().settings().hle_enabled = true;
    vu.Execute(c.start_pc);
    rig.arbiter.drain();
    CHECK(rt.census().runs == 1 && rt.census().misses == 0 && rt.census().declines == 0);
    CHECK(vu.dispatch().bundles() == 0);  // the tiers were not entered
    CHECK(rig.sink.kicks() == direct.kicks);
    if (direct.has_regs) {
        CHECK(std::memcmp(&vu.regs(), &direct.regs, sizeof(VUregs)) == 0);
    } else {
        CHECK(vu.regs().TPC == lib.family.family_exit_tpc);
    }
    std::vector<uint8_t> claim(c.data_mem, c.data_mem + Vu1Case::kDataSize);
    for (const auto& w : direct.data_writes) std::memcpy(claim.data() + (w.addr & 0x3FFF), w.bytes.data(), w.bytes.size());
    CHECK(std::memcmp(claim.data(), vu.state().data, Vu1Case::kDataSize) == 0);

    // A second run on the same image hits the hash memo (same generation).
    vu.LoadCase(c);
    rig.sink.Clear();
    vu.Execute(c.start_pc);
    rig.arbiter.drain();
    CHECK(rt.census().runs == 2);
    CHECK(rig.sink.kicks() == direct.kicks);

    // A MISS: an image no entry matches falls through to the tier untouched by the
    // runtime. The perturbed bundle is INSIDE the loaded program at the variant
    // base (so the layout key misses too; a stale-tail byte alone would not miss).
    static Vu1Case miss;
    miss = c;
    miss.micro_mem[0x1728] ^= 0xFF;
    TestVu1HleMechanism rig2;
    VuUnit vu2(1, &rig2.arbiter);
    vu2.LoadCase(miss);
    Vu1HleRuntime rt2(lib, rig2.arbiter);
    vu2.dispatch().SetHleHook(&Vu1HleRuntime::Hook, &rt2);
    vu2.dispatch().settings().hle_enabled = true;
    vu2.dispatch().set_cycle_cap(200000);
    vu2.Execute(miss.start_pc);
    CHECK(rt2.census().misses == 1 && rt2.census().runs == 0);
    CHECK(vu2.dispatch().bundles() > 0);  // the tier ran instead

    // hle_enabled=false never consults the runtime.
    TestVu1HleMechanism rig3;
    VuUnit vu3(1, &rig3.arbiter);
    vu3.LoadCase(c);
    Vu1HleRuntime rt3(lib, rig3.arbiter);
    vu3.dispatch().SetHleHook(&Vu1HleRuntime::Hook, &rt3);
    vu3.dispatch().settings().hle_enabled = false;
    vu3.Execute(c.start_pc);
    CHECK(rt3.census().runs == 0 && rt3.census().misses == 0);
    CHECK(vu3.dispatch().bundles() > 0);
}

// One hooked run: the case loaded on a fresh unit, the library consulted
// through the tier dispatch, the arbiter drained.
void RunHooked(TestVu1HleMechanism& gif, Vu1HleRuntime& rt, VuUnit& vu, const Vu1Case& cs) {
    vu.LoadCase(cs);
    rt.SetVifTops((uint16_t)cs.vif1_tops, (uint16_t)cs.vif1_itops);
    vu.dispatch().SetHleHook(&Vu1HleRuntime::Hook, &rt);
    vu.dispatch().settings().hle_enabled = true;
    vu.Execute(cs.start_pc);
    gif.arbiter.drain();
}

// The hook path over a configured lowering (arbiter + sink, as the composition
// root builds it) against the raw-only lowering on one corpus case: the same
// program, the same kicks, and the drained register-op stream must be the raw
// route's (the lowering's self-oracle at corpus scale); every kick a library
// program produces -- one complete PACKED tag -- lowers direct.
struct LoweringParitySummary {
    size_t cases = 0, kicks = 0, lowered = 0, lowered_state = 0, lowered_packed = 0, unsupported = 0, parity_failures = 0;
};

void LoweringParityOnCase(const char* path, LoweringParitySummary& sum) {
    static Vu1Case c;
    if (!vu1_case::ReadCase(path, c)) {
        std::printf("SKIP: case absent (%s)\n", path);
        return;
    }
    const Vu1ProgramLibrary& lib = Vu1Library::Get();
    Vu1ImageAnalyzer an(lib);
    const Vu1ImageInfo info = an.Analyze(c.micro_mem, c.data_mem);
    const Vu1LibraryEntry* e = Vu1Registry(lib).Lookup(info, c.start_pc);
    if (!e) return;  // unregistered images are the tier's, not the lowering's
    TestVu1HleMechanism direct_gif, raw_gif;
    KickLowering direct_lowering(direct_gif.arbiter, direct_gif.sink), raw_lowering(raw_gif.arbiter);
    Vu1HleRuntime direct_rt(lib, direct_lowering), raw_rt(lib, raw_lowering);
    VuUnit direct_vu(1, &direct_gif.arbiter), raw_vu(1, &raw_gif.arbiter);
    RunHooked(direct_gif, direct_rt, direct_vu, c);
    RunHooked(raw_gif, raw_rt, raw_vu, c);
    if (direct_rt.census().runs != 1 || raw_rt.census().runs != 1) return;  // a decline is the program's verdict
    const KickLowering::Census d = direct_lowering.census(), r = raw_lowering.census();
    CHECK(d.submitted == r.submitted && r.unconfigured == r.submitted && r.lowered == 0);
    CHECK(d.queued + d.unsupported == d.submitted && d.lowered == d.queued && d.ordering_fallback == 0);
    CHECK(direct_gif.sink.kicks() == raw_gif.sink.kicks());
    const bool parity = direct_gif.sink.ops().Count() == raw_gif.sink.ops().Count() &&
                        direct_gif.sink.ops().Hash() == raw_gif.sink.ops().Hash();
    if (!parity) {
        std::printf("PARITY FAIL %s: direct ops=%zu/%016llx raw ops=%zu/%016llx\n", path,
                    (size_t)direct_gif.sink.ops().Count(), (unsigned long long)direct_gif.sink.ops().Hash(),
                    (size_t)raw_gif.sink.ops().Count(), (unsigned long long)raw_gif.sink.ops().Hash());
        sum.parity_failures++;
    }
    CHECK(parity);
    if (d.unsupported != 0) {
        std::printf("UNSUPPORTED %s (%s): %llu of %llu kicks\n", path, e->name, (unsigned long long)d.unsupported,
                    (unsigned long long)d.submitted);
        CHECK(d.unsupported == 0);
    }
    sum.cases++;
    sum.kicks += d.submitted;
    sum.lowered += d.lowered;
    sum.lowered_state += d.lowered_state;
    sum.lowered_packed += d.lowered_packed;
    sum.unsupported += d.unsupported;
}

// The A+D register block as the title's material pass hands it through: one
// case checked by shape (12 writes then the strip), then the parity over every
// case given (a directory expands to its *.bin).
void MaterialPassLowering(const char* path) {
    static Vu1Case c;
    if (!vu1_case::ReadCase(path, c)) {
        std::printf("SKIP: material case absent (%s)\n", path);
        return;
    }
    const Vu1ProgramLibrary& lib = Vu1Library::Get();
    TestVu1HleMechanism gif;
    KickLowering lowering(gif.arbiter, gif.sink);
    Vu1HleRuntime rt(lib, lowering);
    VuUnit vu(1, &gif.arbiter);
    RunHooked(gif, rt, vu, c);
    const KickLowering::Census d = lowering.census();
    CHECK(rt.census().runs == 1);
    CHECK(d.submitted == 2 && d.queued == 2 && d.lowered == 2 && d.queued_state == 1 && d.lowered_state == 1);
    CHECK(d.unsupported == 0 && d.raw_fallback == 0 && d.ordering_fallback == 0);
    CHECK(lowering.unsupported_by_program().empty());
    CHECK(gif.sink.kicks().size() == 2);
    if (gif.sink.kicks().size() == 2) {
        const auto& block = gif.sink.kicks()[0];
        CHECK(block.size() == 16 + 12 * 16 && block[1] == 0x80 && (block[7] >> 4) == 1 && (block[8] & 0xF) == 0xE);
    }
    std::printf("material pass: kicks=%zu ops=%zu hash=%016llx lowered_state=%llu unsupported=%llu\n",
                gif.sink.kicks().size(), (size_t)gif.sink.ops().Count(), (unsigned long long)gif.sink.ops().Hash(),
                (unsigned long long)d.lowered_state, (unsigned long long)d.unsupported);
}

void LoweringParity(int argc, char** argv) {
    LoweringParitySummary sum;
    for (int i = 2; i < argc; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec)) {
            std::vector<std::string> files;
            for (const auto& entry : std::filesystem::directory_iterator(argv[i]))
                if (entry.path().extension() == ".bin") files.push_back(entry.path().string());
            std::sort(files.begin(), files.end());
            for (const std::string& f : files) LoweringParityOnCase(f.c_str(), sum);
        } else {
            LoweringParityOnCase(argv[i], sum);
        }
    }
    std::printf("LOWERING-PARITY cases=%zu kicks=%zu lowered=%zu lowered_state=%zu lowered_packed=%zu unsupported=%zu "
                "parity_failures=%zu\n",
                sum.cases, sum.kicks, sum.lowered, sum.lowered_state, sum.lowered_packed, sum.unsupported, sum.parity_failures);
}

void LoweringRules() {
    TestVu1HleMechanism rig;
    KickLowering low(rig.arbiter);
    std::vector<uint8_t> tiny(8, 0);  // shorter than a GIFtag: the arbiter drops it, the slot still counts
    low.Submit(std::move(tiny));
    std::vector<uint8_t> tag(16, 0);
    tag[1] = 0x80;  // EOP, NLOOP 0
    low.Submit(std::move(tag));
    CHECK(low.submitted() == 2);
    rig.arbiter.drain();
    CHECK(rig.sink.kicks().size() == 1);
}

}  // namespace

int main(int argc, char** argv) {
    RegistryRules();
    AnalyzerRules();
    LoweringRules();
    if (argc >= 2) RuntimeOnCase(argv[1]);
    if (argc >= 3) MaterialPassLowering(argv[2]);
    if (argc >= 3) LoweringParity(argc, argv);
    if (g_fail == 0) std::printf("Vu1HleMechanism PASS\n");
    return g_fail ? 1 : 0;
}
