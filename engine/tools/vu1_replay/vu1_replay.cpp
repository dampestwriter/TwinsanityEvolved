// vu1_replay -- the VU1 corpus oracle. Replays one recorded VU1 program run
// (a Vu1Case file) on a fresh unit through the interpreter tier or the cached
// tier, and proves the two tiers identical (post-run data memory + the whole
// register block) case by case; with --hle / --catalog it proves the title's
// VU1 HLE library against the interpreter (kicks lowered to canonical GS
// writes and diffed, data memory, the register file when claimed, the typed
// direct-op projection) and tabulates every corpus image. Zero primary types:
// free functions + main.
//
// Rules of the harness: every tier leg runs on a FRESH VuUnit (cold program
// table, empty rings) with the HLE consult off, so what is compared is the
// tier semantics alone; kicks go through a real GifArbiter -> GifParser into
// the capture sink. The HLE leg calls the library program DIRECTLY on the
// case's memories (the runtime is proven by its own unit test). One image
// analyzer per process (its family-prefix latch is the catalog's `prefix`
// column). No environment variable is read; --cycle-cap is the one knob.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/gif_regs.h"
#include "engine/vu/hle/vu1_image_analyzer.h"
#include "engine/vu/hle/vu1_registry.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu1_case.h"
#include "engine/vu/vu_unit.h"
#include PS2R_TITLE_VU1_LIBRARY_H   // the configured title's vu1_library/vu1_library.h (CMake)
#include "tools/vu1_replay/gs_direct_ops_check.h"
#include "tools/vu1_replay/gs_packet_diff.h"
#include "tools/vu1_replay/xgk_capture_sink.h"

namespace {

constexpr int kSkipExit = 77;  // ctest SKIP_RETURN_CODE

// One GIF chain per leg: capture sink <- parser <- arbiter (+ the submit tap).
struct Rig {
    XgkCaptureSink sink;
    GifRegs regs;
    GifParser parser;
    GifArbiter arbiter;
    Rig() : parser(sink, regs), arbiter(parser) { sink.Attach(arbiter); }
};

struct RunResult {
    VUregs regs;
    std::vector<uint8_t> data;
    uint64_t cycle = 0;
    uint64_t bundles = 0;
    size_t kicks = 0;
    size_t kick_bytes = 0;
    size_t opcount = 0;
    uint64_t ophash = 0;
};

const char* ModeName(VuTierMode m) { return m == VuTierMode::Cached ? "cached" : "interp"; }

void Configure(VuUnit& vu, const Vu1Case& c, VuTierMode mode, uint64_t cycle_cap) {
    vu.LoadCase(c);
    vu.dispatch().settings().vu1_mode = mode;
    vu.dispatch().settings().hle_enabled = false;
    vu.dispatch().set_cycle_cap(cycle_cap);
}

RunResult Capture(VuUnit& vu, Rig& rig) {
    RunResult r;
    r.regs = vu.regs();
    r.data.assign(vu.state().data, vu.state().data + Vu1Case::kDataSize);
    r.cycle = vu.cycle();
    r.bundles = vu.dispatch().bundles();
    r.kicks = rig.sink.kicks().size();
    r.kick_bytes = rig.sink.kick_bytes();
    r.opcount = rig.sink.ops().Count();
    r.ophash = rig.sink.ops().Hash();
    return r;
}

// Fresh unit + fresh GIF chain per run: the acceptance semantics of a cold
// program table and empty pipelines, identical for both tiers.
RunResult RunCaseCapture(const Vu1Case& c, VuTierMode mode, uint64_t cycle_cap,
                         std::vector<std::vector<uint8_t>>* out_kicks = nullptr) {
    Rig rig;
    VuUnit vu(1, &rig.arbiter);
    Configure(vu, c, mode, cycle_cap);
    vu.Execute(c.start_pc);
    rig.arbiter.drain();
    if (out_kicks) *out_kicks = rig.sink.kicks();
    return Capture(vu, rig);
}

// The single-run summary line plus the optional raw dumps: the 16 KB post-run
// data memory to `postdata_out` and the 608-byte VUregs to `<postdata_out>.regs`.
void RunCase(const Vu1Case& c, VuTierMode mode, uint64_t cycle_cap, const char* postdata_out) {
    const RunResult r = RunCaseCapture(c, mode, cycle_cap);
    uint32_t q_bits;
    std::memcpy(&q_bits, &r.regs.Q, 4);
    std::printf("replayed pc=0x%04x mode=%s -> TPC=0x%04x Q=%08x vi01=%04x vf01=%08x,%08x,%08x,%08x\n", c.start_pc,
                ModeName(mode), r.regs.TPC, q_bits, r.regs.VI[1], r.regs.VF[1].I[0], r.regs.VF[1].I[1],
                r.regs.VF[1].I[2], r.regs.VF[1].I[3]);
    if (!postdata_out) return;
    if (std::FILE* f = std::fopen(postdata_out, "wb")) {
        std::fwrite(r.data.data(), 1, r.data.size(), f);
        std::fclose(f);
        std::printf("post-run data memory dumped to %s\n", postdata_out);
    } else {
        std::fprintf(stderr, "cannot open %s for post-data dump\n", postdata_out);
    }
    const std::string regs_path = std::string(postdata_out) + ".regs";
    if (std::FILE* rf = std::fopen(regs_path.c_str(), "wb")) {
        std::fwrite(&r.regs, sizeof(VUregs), 1, rf);
        std::fclose(rf);
        std::printf("final VUregs (%zuB, incl. TPC) dumped to %s\n", sizeof(VUregs), regs_path.c_str());
    } else {
        std::fprintf(stderr, "cannot open %s for regs dump\n", regs_path.c_str());
    }
}

// --compare: interp leg, then cached leg, each on a fresh unit; report every
// differing data byte (first 8 shown) and register, then the verdict line.
int Compare(const Vu1Case& c, uint64_t cycle_cap, int expect_aot, bool compare_cycle) {
    const RunResult ri = RunCaseCapture(c, VuTierMode::Interp, cycle_cap);
    const RunResult rc = RunCaseCapture(c, VuTierMode::Cached, cycle_cap);
    const uint64_t aot_runs = 0;  // no native tier in this tree

    size_t data_diff_bytes = 0;
    size_t shown = 0;
    for (size_t i = 0; i < ri.data.size(); i++) {
        if (ri.data[i] == rc.data[i]) continue;
        data_diff_bytes++;
        if (shown < 8) {
            shown++;
            std::printf("  data[0x%04zx]: interp=%02x cached=%02x\n", i, ri.data[i], rc.data[i]);
        }
    }

    const bool regs_differ = std::memcmp(&ri.regs, &rc.regs, sizeof(VUregs)) != 0;
    if (regs_differ) {
        for (int r = 0; r < 32; r++) {
            if (std::memcmp(&ri.regs.VF[r], &rc.regs.VF[r], 16) != 0)
                std::printf("  VF%02d: interp=%08x,%08x,%08x,%08x cached=%08x,%08x,%08x,%08x\n", r, ri.regs.VF[r].I[0],
                            ri.regs.VF[r].I[1], ri.regs.VF[r].I[2], ri.regs.VF[r].I[3], rc.regs.VF[r].I[0],
                            rc.regs.VF[r].I[1], rc.regs.VF[r].I[2], rc.regs.VF[r].I[3]);
        }
        for (int r = 0; r < 16; r++) {
            if (ri.regs.VI[r] != rc.regs.VI[r])
                std::printf("  VI%02d: interp=%04x cached=%04x\n", r, ri.regs.VI[r], rc.regs.VI[r]);
        }
        uint32_t qi, qc, pi_, pc_;
        std::memcpy(&qi, &ri.regs.Q, 4);
        std::memcpy(&qc, &rc.regs.Q, 4);
        std::memcpy(&pi_, &ri.regs.P, 4);
        std::memcpy(&pc_, &rc.regs.P, 4);
        if (std::memcmp(&ri.regs.ACC, &rc.regs.ACC, 16) != 0) std::printf("  ACC differs\n");
        if (qi != qc) std::printf("  Q: interp=%08x cached=%08x\n", qi, qc);
        if (pi_ != pc_) std::printf("  P: interp=%08x cached=%08x\n", pi_, pc_);
        if (ri.regs.I != rc.regs.I) std::printf("  I: interp=%08x cached=%08x\n", ri.regs.I, rc.regs.I);
        if (ri.regs.R != rc.regs.R) std::printf("  R: interp=%08x cached=%08x\n", ri.regs.R, rc.regs.R);
        if (ri.regs.status_flag != rc.regs.status_flag)
            std::printf("  status_flag: interp=%04x cached=%04x\n", ri.regs.status_flag, rc.regs.status_flag);
        if (ri.regs.mac_flag != rc.regs.mac_flag)
            std::printf("  mac_flag: interp=%04x cached=%04x\n", ri.regs.mac_flag, rc.regs.mac_flag);
        if (ri.regs.clip_flag != rc.regs.clip_flag)
            std::printf("  clip_flag: interp=%06x cached=%06x\n", ri.regs.clip_flag, rc.regs.clip_flag);
        if (ri.regs.TPC != rc.regs.TPC) std::printf("  TPC: interp=%04x cached=%04x\n", ri.regs.TPC, rc.regs.TPC);
    }

    bool aot_expect_ok = true;
    if (expect_aot > 0 && aot_runs == 0) aot_expect_ok = false;
    if (expect_aot < 0 && aot_runs != 0) aot_expect_ok = false;

    // --compare-cycle: the final VU clock must also agree (the kicked op
    // stream is checked as the GS-visible output volume).
    bool cycle_ok = true;
    if (compare_cycle) {
        if (ri.cycle != rc.cycle) {
            cycle_ok = false;
            std::printf("  vu_cycle: interp=%llu cached=%llu\n", (unsigned long long)ri.cycle,
                        (unsigned long long)rc.cycle);
        }
        if (ri.ophash != rc.ophash || ri.opcount != rc.opcount) {
            cycle_ok = false;
            std::printf("  gsops: interp=%zu/%016llx cached=%zu/%016llx\n", ri.opcount,
                        (unsigned long long)ri.ophash, rc.opcount, (unsigned long long)rc.ophash);
        }
    }

    const bool pass = data_diff_bytes == 0 && !regs_differ && aot_expect_ok && cycle_ok;
    std::printf("COMPARE %s: pc=0x%04x data_diff_bytes=%zu/%u regs_%s aot_dispatches=%llu%s (interp vs cached)\n",
                pass ? "PASS" : "FAIL", c.start_pc, data_diff_bytes, Vu1Case::kDataSize,
                regs_differ ? "DIFFER" : "identical", (unsigned long long)aot_runs,
                aot_expect_ok ? "" : (expect_aot > 0 ? " EXPECTED-AOT-BUT-DID-NOT-DISPATCH" : " EXPECTED-FALLBACK-BUT-DISPATCHED"));
    return pass ? 0 : 1;
}

// --bench: ONE unit per mode reused across N runs (the cached decode
// amortises); the case is re-installed per run and only Execute is timed.
void BenchMode(const Vu1Case& c, VuTierMode mode, uint64_t cycle_cap, long n) {
    Rig rig;
    VuUnit vu(1, &rig.arbiter);
    uint64_t total_ns = 0;
    for (long i = 0; i < n; i++) {
        Configure(vu, c, mode, cycle_cap);
        rig.sink.Clear();
        const auto t0 = std::chrono::steady_clock::now();
        vu.Execute(c.start_pc);
        const auto t1 = std::chrono::steady_clock::now();
        total_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        rig.arbiter.drain();
    }
    const uint64_t bundles = vu.dispatch().bundles();
    if (bundles > 0) {
        std::printf("BENCH mode=%s runs=%ld total=%.3f ms avg=%.3f us/run bundles/run=%llu ns/bundle=%.1f aot=0\n",
                    ModeName(mode), n, total_ns / 1e6, (n > 0) ? total_ns / 1e3 / n : 0.0,
                    (unsigned long long)(bundles / (uint64_t)n), (double)total_ns / (double)bundles);
        std::printf("CENSUS entries=%zu\n", vu.dispatch().cached().table_size());
    } else {
        std::printf("BENCH mode=%s runs=%ld total=%.3f ms avg=%.3f us/run\n", ModeName(mode), n, total_ns / 1e6,
                    (n > 0) ? total_ns / 1e3 / n : 0.0);
    }
}

// The library + analyzer the HLE grammars use (one of each per process).
Vu1Registry& Registry() {
    static Vu1Registry r(Vu1Library::Get());
    return r;
}
Vu1ImageAnalyzer& Analyzer() {
    static Vu1ImageAnalyzer a(Vu1Library::Get());
    return a;
}

struct HleOpts {
    GsPacketDiff::Tolerance tol;
    bool use_ref = false;            // --hle-ref: register the interpreter as HLE
    bool skip_unregistered = false;  // --skip-unregistered: exit 77 when no HLE
    uint32_t max_lines = 20;
};

Vu1HleInput MakeInput(const Vu1Case& c, const Vu1ImageInfo& info) {
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
    return in;
}

void PrintImageHeader(const Vu1Case& c, const Vu1ImageInfo& info, std::FILE* f) {
    std::fprintf(f,
                 "image=%016llx (%s) variant=%016llx prefix=%s extent=0x%x entry=0x%04x "
                 "top=%u tops=%u itop=%u itops=%u dispatch=",
                 (unsigned long long)info.image_hash, Vu1ImageAnalyzer::ShortHash(info.image_hash).c_str(),
                 (unsigned long long)info.variant_hash, info.prefix_is_family ? "family" : "other", info.extent,
                 c.start_pc, c.vif1_top, c.vif1_tops, c.vif1_itop, c.vif1_itops);
    if (info.dispatch_chain.empty()) std::fprintf(f, "(n/a)");
    for (size_t i = 0; i < info.dispatch_chain.size(); i++)
        std::fprintf(f, "%s0x%x", i ? "->" : "", info.dispatch_chain[i]);
    std::fprintf(f, "%s\n", info.dispatch_valid ? "" : " (unterminated)");
}

// --dump-kicks: interp run; the image header, the kick census and the decoded
// GIF stream (every tag, every canonical write).
int DumpKicks(const Vu1Case& c, uint64_t cycle_cap, const char* out_path) {
    std::vector<std::vector<uint8_t>> kicks;
    RunCaseCapture(c, VuTierMode::Interp, cycle_cap, &kicks);
    const Vu1ImageInfo info = Analyzer().Analyze(c.micro_mem, c.data_mem);
    std::FILE* f = out_path ? std::fopen(out_path, "w") : stdout;
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", out_path);
        return 2;
    }
    PrintImageHeader(c, info, f);
    GsPacketDiff::Stream s = GsPacketDiff::Decode(kicks);
    size_t bytes = 0;
    for (const auto& k : kicks) bytes += k.size();
    std::fprintf(f, "kicks=%zu bytes=%zu writes=%zu tags=%zu errors=%zu\n", kicks.size(), bytes, s.writes.size(),
                 s.tags.size(), s.errors.size());
    GsPacketDiff::Print(s, f);
    if (out_path) {
        std::fclose(f);
        std::printf("kick dump written to %s\n", out_path);
    }
    return s.errors.empty() ? 0 : 1;
}

// The harness reference program: the interpreter itself behind the HLE
// contract (registered as the wildcard under --hle-ref; it wins only where no
// library entry matches). Proves the capture -> decode -> diff loop end to end.
bool ReferenceInterpreter(const Vu1HleInput& in, Vu1HleOutput& out) {
    static Vu1Case c;  // 33 KB -- keep off the stack
    std::memset(&c, 0, sizeof(c));
    c.magic = Vu1Case::kMagic;
    c.version = Vu1Case::kVersion;
    c.start_pc = in.start_pc;
    c.regs = *in.regs;
    std::memcpy(c.micro_mem, in.micro, Vu1Case::kMicroSize);
    std::memcpy(c.data_mem, in.data, Vu1Case::kDataSize);
    c.vif1_top = in.top;
    c.vif1_tops = in.tops;
    c.vif1_itop = in.itop;
    c.vif1_itops = in.itops;
    const RunResult r = RunCaseCapture(c, VuTierMode::Interp, VuTierDispatch::kDefaultCycleCap, &out.kicks);
    out.regs = r.regs;
    out.has_regs = true;
    for (uint32_t qw = 0; qw < Vu1Case::kDataSize / 16; ++qw) {
        if (std::memcmp(r.data.data() + qw * 16, in.data + qw * 16, 16) != 0) {
            uint32_t end = qw + 1;
            while (end < Vu1Case::kDataSize / 16 && std::memcmp(r.data.data() + end * 16, in.data + end * 16, 16) != 0)
                ++end;
            Vu1HleOutput::DataWrite w;
            w.addr = qw * 16;
            w.bytes.assign(r.data.data() + qw * 16, r.data.data() + end * 16);
            out.data_writes.push_back(std::move(w));
            qw = end;
        }
    }
    return true;
}

// --hle: interp leg vs the registered native program -- kicks (canonical
// writes under the tolerance), the typed direct-op projection per kick, the
// data-memory claim, the register file when claimed. Exit 0 PASS, 1 FAIL /
// FALLBACK / unregistered, 77 unregistered under --skip-unregistered.
int HleCompare(const Vu1Case& c, uint64_t cycle_cap, const HleOpts& o) {
    const Vu1ImageInfo info = Analyzer().Analyze(c.micro_mem, c.data_mem);
    if (o.use_ref) Registry().Append({0, 0, "ref_interpreter", kVu1AnyEntry, ReferenceInterpreter, nullptr, 0});
    const Vu1LibraryEntry* e = Registry().Lookup(info, c.start_pc);
    PrintImageHeader(c, info, stdout);
    if (!e) {
        std::printf("HLE %s: no HLE registered for image %s (entry 0x%04x)\n", o.skip_unregistered ? "SKIP" : "FAIL",
                    Vu1ImageAnalyzer::ShortHash(info.image_hash).c_str(), c.start_pc);
        return o.skip_unregistered ? kSkipExit : 1;
    }

    std::vector<std::vector<uint8_t>> kicks_interp;
    const RunResult ri = RunCaseCapture(c, VuTierMode::Interp, cycle_cap, &kicks_interp);

    Vu1HleOutput out;
    const Vu1HleInput in = MakeInput(c, info);
    if (!e->fn(in, out)) {
        std::printf("HLE FALLBACK: %s declined image %s (entry 0x%04x)\n", e->name,
                    Vu1ImageAnalyzer::ShortHash(info.image_hash).c_str(), c.start_pc);
        return 1;
    }

    GsPacketDiff::Stream a = GsPacketDiff::Decode(kicks_interp);
    GsPacketDiff::Stream b = GsPacketDiff::Decode(out.kicks);
    GsPacketDiff::DiffResult d = GsPacketDiff::Diff(a, b, o.tol, o.max_lines);
    for (const auto& l : d.lines) std::printf("  %s\n", l.c_str());

    // direct=: every HLE kick converted to typed ops must project back to
    // EXACTLY the byte decoder's writes (bit equality regardless of --tol). A
    // converter decline (IMAGE tag / truncation) is reported as fallback.
    bool direct_ok = true;
    bool direct_fallback = false;
    for (size_t ki = 0; ki < out.kicks.size(); ki++) {
        const auto& k = out.kicks[ki];
        std::vector<GsDirectOpsCheck::Op> ops;
        if (!GsDirectOpsCheck::PacketToOps(k.data(), k.size(), ops)) {
            direct_fallback = true;
            std::printf("  DIRECT fallback: kick %zu (%zu bytes) not convertible\n", ki, k.size());
            continue;
        }
        std::vector<std::pair<uint8_t, uint64_t>> tw;
        GsDirectOpsCheck::OpsToWrites(ops, tw);
        std::vector<std::vector<uint8_t>> one;
        one.push_back(k);
        GsPacketDiff::Stream ks = GsPacketDiff::Decode(one);
        if (!GsDirectOpsCheck::Matches(tw, ks)) {
            if (direct_ok)
                std::printf("  DIRECT mismatch: kick %zu ops=%zu typed_writes=%zu byte_writes=%zu\n", ki, ops.size(),
                            tw.size(), ks.writes.size());
            direct_ok = false;
        }
    }

    // data=: entry data + the claimed data_writes must equal the interpreter's
    // post-run data memory byte for byte.
    bool data_ok = true;
    {
        std::vector<uint8_t> claim(c.data_mem, c.data_mem + Vu1Case::kDataSize);
        for (const auto& w : out.data_writes) {
            const uint32_t addr = w.addr & 0x3FFF;
            const size_t n = w.bytes.size() <= claim.size() - addr ? w.bytes.size() : claim.size() - addr;
            std::memcpy(claim.data() + addr, w.bytes.data(), n);
        }
        static int s_qw_detail = 0;  // first 8 mismatching QWs per process
        for (uint32_t i = 0; i < Vu1Case::kDataSize; i += 16) {
            if (std::memcmp(claim.data() + i, ri.data.data() + i, 16) != 0) {
                if (data_ok) std::printf("  DATA mismatch first at QW %u (0x%x)\n", i / 16, i);
                if (s_qw_detail < 8) {
                    s_qw_detail++;
                    uint32_t aw[4], bw[4];
                    std::memcpy(aw, claim.data() + i, 16);
                    std::memcpy(bw, ri.data.data() + i, 16);
                    std::printf("    QW %u hle    %08x %08x %08x %08x\n    QW %u interp %08x %08x %08x %08x\n", i / 16,
                                aw[0], aw[1], aw[2], aw[3], i / 16, bw[0], bw[1], bw[2], bw[3]);
                }
                data_ok = false;
            }
        }
    }

    // regs= (only when the program claims the register file): raw-bit compare.
    bool regs_ok = true;
    if (out.has_regs) {
        char why[128] = {0};
        auto cmp32 = [&](const char* nm, uint32_t x, uint32_t y) {
            if (x != y && regs_ok) {
                std::snprintf(why, sizeof(why), "%s interp=%08x hle=%08x", nm, x, y);
                regs_ok = false;
            }
        };
        for (int r = 0; r < 32; r++)
            for (int k = 0; k < 4; k++) {
                char nm[16];
                std::snprintf(nm, sizeof(nm), "vf%02d.%c", r, "xyzw"[k]);
                cmp32(nm, ri.regs.VF[r].I[k], out.regs.VF[r].I[k]);
            }
        for (int r = 0; r < 16; r++) {
            char nm[16];
            std::snprintf(nm, sizeof(nm), "vi%02d", r);
            cmp32(nm, ri.regs.VI[r], out.regs.VI[r]);
        }
        for (int k = 0; k < 4; k++) cmp32("acc", ri.regs.ACC.I[k], out.regs.ACC.I[k]);
        uint32_t qa, qb, pa, pb;
        std::memcpy(&qa, &ri.regs.Q, 4);
        std::memcpy(&qb, &out.regs.Q, 4);
        std::memcpy(&pa, &ri.regs.P, 4);
        std::memcpy(&pb, &out.regs.P, 4);
        cmp32("Q", qa, qb);
        cmp32("P", pa, pb);
        cmp32("I", ri.regs.I, out.regs.I);
        cmp32("R", ri.regs.R, out.regs.R);
        cmp32("status", ri.regs.status_flag, out.regs.status_flag);
        cmp32("mac", ri.regs.mac_flag, out.regs.mac_flag);
        cmp32("clip", ri.regs.clip_flag, out.regs.clip_flag);
        cmp32("TPC", ri.regs.TPC, out.regs.TPC);
        if (!regs_ok) std::printf("  REGS mismatch: %s\n", why);
    }

    const bool pass = d.pass && data_ok && regs_ok && direct_ok;
    std::printf("HLE %s: %s vs interp -- %s data=%s regs=%s direct=%s (tol xy=%u z=%u rgba=%u st=%g)\n",
                pass ? "PASS" : "FAIL", e->name, d.summary.c_str(), data_ok ? "ok" : "MISMATCH",
                out.has_regs ? (regs_ok ? "ok" : "MISMATCH") : "n/a",
                out.kicks.empty() ? "n/a" : (!direct_ok ? "MISMATCH" : (direct_fallback ? "fallback" : "ok")),
                o.tol.xy, o.tol.z, o.tol.rgba, o.tol.st_rel);
    return pass ? 0 : 1;
}

bool EndsWithBin(const std::string& s) { return s.size() > 4 && s.compare(s.size() - 4, 4, ".bin") == 0; }

// Expand directories to their *.bin files; pass files through; sort the paths.
std::vector<std::string> ExpandCasePaths(int argc, char** argv, int first) {
    std::vector<std::string> out;
    for (int i = first; i < argc; i++) {
        const std::string p = argv[i];
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (DIR* d = opendir(p.c_str())) {
                while (dirent* de = readdir(d))
                    if (EndsWithBin(de->d_name)) out.push_back(p + "/" + de->d_name);
                closedir(d);
            }
            continue;
        }
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct ImageRow {
    Vu1ImageInfo info;
    uint32_t start_pc = 0;
    uint32_t cases = 0;
    uint64_t kicks = 0, verts = 0, bundles = 0, bytes = 0;
    std::vector<std::string> prims;  // distinct PRIM types seen (as text)
    std::string example;
};

// --catalog: one row per (image, entry) -- hashes, dispatch chain, kicks /
// vertices (interp leg), bundles (cached leg), corpus-weighted bundle share
// over family images, the HLE coverage. Rows sorted by bundles descending.
int Catalog(const std::vector<std::string>& paths, uint64_t cycle_cap) {
    std::map<std::pair<uint64_t, uint32_t>, ImageRow> rows;
    static Vu1Case c;  // 33 KB -- keep off the stack
    uint32_t loaded = 0;
    for (const std::string& p : paths) {
        if (!vu1_case::ReadCase(p.c_str(), c)) {
            std::fprintf(stderr, "catalog: skipping unreadable case %s\n", p.c_str());
            continue;
        }
        loaded++;
        const Vu1ImageInfo info = Analyzer().Analyze(c.micro_mem, c.data_mem);
        ImageRow& r = rows[{info.image_hash, c.start_pc}];
        if (r.cases == 0) {
            r.info = info;
            r.start_pc = c.start_pc;
            r.example = p;
        }
        r.cases++;
        std::vector<std::vector<uint8_t>> kicks;
        RunCaseCapture(c, VuTierMode::Interp, cycle_cap, &kicks);
        const RunResult rc = RunCaseCapture(c, VuTierMode::Cached, cycle_cap);
        r.bundles += rc.bundles;
        r.kicks += kicks.size();
        for (const auto& k : kicks) r.bytes += k.size();
        GsPacketDiff::Stream s = GsPacketDiff::Decode(kicks);
        for (const GsPacketDiff::Write& w : s.writes) {
            if (GsPacketDiff::IsKickReg(w.addr)) r.verts++;
            if (w.addr == GsPacketDiff::REG_PRIM) {
                static const char* kNames[8] = {"point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"};
                std::string n = kNames[w.value & 7];
                if ((w.value >> 4) & 1) n += "+tex";
                if ((w.value >> 6) & 1) n += "+abe";
                if ((w.value >> 5) & 1) n += "+fog";
                if (std::find(r.prims.begin(), r.prims.end(), n) == r.prims.end()) r.prims.push_back(n);
            }
        }
    }

    std::vector<const ImageRow*> sorted;
    for (const auto& kv : rows) sorted.push_back(&kv.second);
    std::sort(sorted.begin(), sorted.end(), [](const ImageRow* a, const ImageRow* b) { return a->bundles > b->bundles; });

    // Weights count family images only: synthetic backstop cases (foreign
    // prefix, no kicks) spin to the cycle limit and would swamp the column.
    uint64_t total_bundles = 0, covered_bundles = 0;
    uint32_t covered_images = 0;
    for (const ImageRow* r : sorted)
        if (r->info.prefix_is_family) total_bundles += r->bundles;

    std::printf("%-8s %-16s %-16s %-6s %-5s %-6s %-7s %-8s %-9s %-6s %-22s %-8s %s\n", "image", "image_hash",
                "variant_hash", "entry", "cases", "kicks", "verts", "bundles", "bundle%", "extent", "dispatch", "prefix",
                "hle / prims");
    for (const ImageRow* r : sorted) {
        const Vu1LibraryEntry* e = Registry().Lookup(r->info, r->start_pc);
        if (e) {
            covered_images++;
            if (r->info.prefix_is_family) covered_bundles += r->bundles;
        }
        std::string chain;
        for (size_t i = 0; i < r->info.dispatch_chain.size(); i++) {
            char b[16];
            std::snprintf(b, sizeof(b), "%s%x", i ? ">" : "", r->info.dispatch_chain[i]);
            chain += b;
        }
        if (chain.empty()) chain = "-";
        if (chain.size() > 22) chain = chain.substr(0, 19) + "...";
        std::string prims;
        for (size_t i = 0; i < r->prims.size(); i++) prims += (i ? "," : "") + r->prims[i];
        std::printf("%-8s %016llx %016llx 0x%04x %5u %6llu %7llu %8llu %8.2f%% 0x%04x %-22s %-8s %s%s%s\n",
                    Vu1ImageAnalyzer::ShortHash(r->info.image_hash).c_str(), (unsigned long long)r->info.image_hash,
                    (unsigned long long)r->info.variant_hash, r->start_pc, r->cases, (unsigned long long)r->kicks,
                    (unsigned long long)r->verts, (unsigned long long)r->bundles,
                    (total_bundles && r->info.prefix_is_family) ? 100.0 * r->bundles / total_bundles : 0.0,
                    r->info.extent, chain.c_str(), r->info.prefix_is_family ? "family" : "other", e ? e->name : "-",
                    e ? " " : " [", e ? "" : (prims + "]").c_str());
    }
    std::printf("\nCATALOG cases=%u images=%zu hle_covered=%u/%zu bundle_coverage=%.2f%% "
                "(corpus-weighted)\n",
                loaded, sorted.size(), covered_images, sorted.size(),
                total_bundles ? 100.0 * covered_bundles / total_bundles : 0.0);
    return loaded ? 0 : 2;
}

// --registry: the library's self-report (the module gate pins the counts):
// the table totals, then one row per program name in first-registration
// order with its entry count, distinct variant hashes and entry pcs.
int RegistryReport() {
    const Vu1Registry& r = Registry();
    std::printf("REGISTRY entries=%zu distinct_variant_hashes=%zu\n", r.Entries().size(), r.DistinctVariantHashes());
    std::vector<std::string> order;
    std::map<std::string, std::vector<const Vu1LibraryEntry*>> by_name;
    for (const Vu1LibraryEntry& e : r.Entries()) {
        if (by_name.find(e.name) == by_name.end()) order.push_back(e.name);
        by_name[e.name].push_back(&e);
    }
    for (const std::string& name : order) {
        const std::vector<const Vu1LibraryEntry*>& es = by_name[name];
        std::vector<uint64_t> hashes;
        std::vector<uint32_t> pcs;
        for (const Vu1LibraryEntry* e : es) {
            hashes.push_back(e->image_hash ? e->image_hash : e->variant_hash);
            pcs.push_back(e->start_pc);
        }
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        std::sort(pcs.begin(), pcs.end());
        pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
        std::string pc_list;
        for (uint32_t pc : pcs) {
            char buf[16];
            if (pc == kVu1AnyEntry)
                std::snprintf(buf, sizeof buf, "any");
            else
                std::snprintf(buf, sizeof buf, "0x%x", pc);
            pc_list += (pc_list.empty() ? "" : ",") + std::string(buf);
        }
        std::printf("  %-22s entries=%-3zu distinct_hashes=%-3zu entry_pcs=%s\n", name.c_str(), es.size(), hashes.size(),
                    pc_list.c_str());
    }
    std::printf("REGISTRY programs=%zu\n", order.size());
    return 0;
}

// --gsops: one seed line `<case basename> <op count> <op hash>` from the
// interp leg (the GS-side observable the Phase-6 renderer is seeded from).
int GsOps(const Vu1Case& c, uint64_t cycle_cap, const char* case_path) {
    const RunResult r = RunCaseCapture(c, VuTierMode::Interp, cycle_cap);
    const char* base = std::strrchr(case_path, '/');
    base = base ? base + 1 : case_path;
    std::printf("%s %zu %016llx\n", base, r.opcount, (unsigned long long)r.ophash);
    return 0;
}

struct Bundle {
    uint32_t lower, upper;
};

constexpr uint32_t U_NOP = 0x000002ff;
constexpr uint32_t L_NOP = 0x8000033c;  // MOVE.- vf0,vf0

void SetVf(VUregs& r, int reg, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    r.VF[reg].I[0] = x;
    r.VF[reg].I[1] = y;
    r.VF[reg].I[2] = z;
    r.VF[reg].I[3] = w;
}

// A minimal deterministic case: the transform-chain register set feeding a
// short FMAC program terminated by NOP[E] + the E-bit delay bundle. Shared by
// --selftest and `--compare --fabricate`.
void FabricateCase(Vu1Case& c) {
    std::memset(&c, 0, sizeof(c));
    c.magic = Vu1Case::kMagic;
    c.version = Vu1Case::kVersion;
    c.start_pc = 0;
    c.regs.Reset();  // VF0=(0,0,0,1), Q=1.0
    SetVf(c.regs, 1, 0x45d4dbd0, 0x4664410d, 0xc3eb1b81, 0x3ed75d7c);
    SetVf(c.regs, 2, 0x44601194, 0xc5dd1d30, 0xc2521707, 0x3d407333);
    SetVf(c.regs, 3, 0xc700be6a, 0xc6e87006, 0x447d2fb3, 0xbf67f06a);
    SetVf(c.regs, 4, 0x488a4b80, 0x4873ad40, 0x49cbcea7, 0x40e81f40);
    SetVf(c.regs, 11, 0x3e96b5d2, 0x3f8c92b2, 0xbf335710, 0);
    c.vif1_top = 2;
    c.vif1_tops = 0x202;
    c.vif1_itop = 1;
    c.vif1_itops = 3;

    const Bundle snippet[] = {
        {L_NOP, 0x01e021bf},          // MULAw  ACC,vf4,vf0w
        {L_NOP, 0x01eb10bd},          // MADDAy ACC,vf2,vf11y
        {L_NOP, 0x01eb18be},          // MADDAz ACC,vf3,vf11z
        {L_NOP, 0x01eb0a48},          // MADDx  vf9,vf1,vf11x
        {0x10010005, U_NOP},          // IADDIU vi1,vi0,5
        {L_NOP, U_NOP | 0x40000000},  // NOP[E]
        {L_NOP, U_NOP},               // E-bit delay bundle
    };
    for (size_t i = 0; i < sizeof(snippet) / sizeof(snippet[0]); i++) {
        std::memcpy(&c.micro_mem[i * 8 + 0], &snippet[i].lower, 4);
        std::memcpy(&c.micro_mem[i * 8 + 4], &snippet[i].upper, 4);
    }
}

bool SameRun(const RunResult& a, const RunResult& b) {
    return std::memcmp(&a.regs, &b.regs, sizeof(VUregs)) == 0 && a.data == b.data && a.cycle == b.cycle &&
           a.ophash == b.ophash && a.opcount == b.opcount;
}

// --selftest: fabricate a case, round-trip it through WriteCase/ReadCase,
// replay it twice on the interpreter (replay determinism), then prove the
// cached tier identical on it. Exit 0 / 1.
int SelfTest(uint64_t cycle_cap) {
    const std::string case_path = "/tmp/vu1_selftest_case_" + std::to_string((long)getpid()) + ".bin";
    static Vu1Case c, back;
    FabricateCase(c);
    if (!vu1_case::WriteCase(case_path.c_str(), c)) {
        std::printf("SELFTEST FAIL: cannot write %s\n", case_path.c_str());
        return 1;
    }
    const bool read_ok = vu1_case::ReadCase(case_path.c_str(), back);
    std::remove(case_path.c_str());
    if (!read_ok || std::memcmp(&c, &back, sizeof(Vu1Case)) != 0) {
        std::printf("SELFTEST FAIL: case round-trip mismatch (%s)\n", case_path.c_str());
        return 1;
    }
    const RunResult a = RunCaseCapture(back, VuTierMode::Interp, cycle_cap);
    const RunResult b = RunCaseCapture(back, VuTierMode::Interp, cycle_cap);
    RunCase(back, VuTierMode::Interp, cycle_cap, nullptr);
    RunCase(back, VuTierMode::Interp, cycle_cap, nullptr);
    if (!SameRun(a, b)) {
        std::printf("SELFTEST FAIL: two interp replays differ\n");
        return 1;
    }
    // 7 bundles: E-bit at 0x28, its delay bundle at 0x30 -> TPC 0x38; VF9 is the
    // transform-chain ground truth (round-toward-zero + fused multiply-add).
    static const uint32_t want_vf9[4] = {0x48970746u, 0x48825220u, 0x49cbb259u, 0x410105e0u};
    if (a.regs.TPC != 0x38 || a.regs.VI[1] != 5 || std::memcmp(a.regs.VF[9].I, want_vf9, 16) != 0) {
        std::printf("SELFTEST FAIL: fabricated program ended at TPC=0x%04x vi01=%04x vf09=%08x,%08x,%08x,%08x "
                    "(expected 0x0038, 0005, 48970746,48825220,49cbb259,410105e0)\n",
                    a.regs.TPC, a.regs.VI[1], a.regs.VF[9].I[0], a.regs.VF[9].I[1], a.regs.VF[9].I[2],
                    a.regs.VF[9].I[3]);
        return 1;
    }
    if (Compare(back, cycle_cap, 0, true) != 0) {
        std::printf("SELFTEST FAIL: interp vs cached differ on the fabricated case\n");
        return 1;
    }
    std::printf("SELFTEST PASS: 2 interp replays identical, interp==cached, bundles=%llu\n",
                (unsigned long long)a.bundles);
    return 0;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: vu1_replay <case.bin> [trace_out] [postdata_out]\n"
                 "       vu1_replay --selftest\n"
                 "       vu1_replay --mode interp|cached <case.bin> [postdata_out]\n"
                 "       vu1_replay --compare [--expect-aot|--expect-noaot] "
                 "<case.bin | --fabricate>\n"
                 "       vu1_replay --bench N <case.bin>\n"
                 "       vu1_replay --dump-kicks <case.bin> [out.txt]\n"
                 "       vu1_replay --hle [--hle-ref] [--skip-unregistered] [--exact]\n"
                 "                  [--tol-xy N] [--tol-z N] [--tol-rgba N] [--tol-st F]\n"
                 "                  [--strict-packets] [--max-lines N] <case.bin>\n"
                 "       vu1_replay --catalog <dir | case.bin ...>\n"
                 "       vu1_replay --registry            (the library self-report: entries, distinct variant hashes)\n"
                 "       vu1_replay [--cycle-cap N] ...   (bundles before a runaway program is cut; default 1000000)\n"
                 "       vu1_replay --gsops <case.bin>    (one seed line: case, register-op count, stream hash)\n");
    return 2;
}

bool LoadOrComplain(const char* path, Vu1Case& c) {
    if (vu1_case::ReadCase(path, c)) return true;
    std::fprintf(stderr, "vu1_replay: cannot load case %s (bad path, magic, or version)\n", path);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    // --cycle-cap N may appear anywhere; it is lifted out before dispatch.
    uint64_t cycle_cap = VuTierDispatch::kDefaultCycleCap;
    std::vector<char*> args;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && std::strcmp(argv[i], "--cycle-cap") == 0) {
            if (i + 1 >= argc) return Usage();
            cycle_cap = std::strtoull(argv[++i], nullptr, 0);
            if (cycle_cap == 0) {
                std::fprintf(stderr, "vu1_replay: --cycle-cap N must be > 0\n");
                return 2;
            }
            continue;
        }
        args.push_back(argv[i]);
    }
    const int n = (int)args.size();
    argv = args.data();

    if (n >= 2 && std::strcmp(argv[1], "--selftest") == 0) return SelfTest(cycle_cap);

    if (n >= 2 && std::strcmp(argv[1], "--dump-kicks") == 0) {
        if (n < 3) return Usage();
        static Vu1Case c;
        if (!vu1_case::ReadCase(argv[2], c)) {
            std::fprintf(stderr, "vu1_replay: cannot load case %s\n", argv[2]);
            return 2;
        }
        return DumpKicks(c, cycle_cap, n >= 4 ? argv[3] : nullptr);
    }
    if (n >= 2 && std::strcmp(argv[1], "--gsops") == 0) {
        if (n < 3) return Usage();
        static Vu1Case c;
        if (!vu1_case::ReadCase(argv[2], c)) {
            std::fprintf(stderr, "vu1_replay: cannot load case %s\n", argv[2]);
            return 2;
        }
        return GsOps(c, cycle_cap, argv[2]);
    }
    if (n >= 2 && std::strcmp(argv[1], "--hle") == 0) {
        HleOpts o;
        const char* case_path = nullptr;
        for (int i = 2; i < n; i++) {
            auto next_u = [&](uint32_t& dst) {
                if (i + 1 < n) dst = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
            };
            if (std::strcmp(argv[i], "--hle-ref") == 0) o.use_ref = true;
            else if (std::strcmp(argv[i], "--skip-unregistered") == 0) o.skip_unregistered = true;
            else if (std::strcmp(argv[i], "--strict-packets") == 0) o.tol.strict_packets = true;
            else if (std::strcmp(argv[i], "--exact") == 0) {
                o.tol.xy = o.tol.z = o.tol.rgba = o.tol.fog = o.tol.uv = 0;
                o.tol.st_rel = o.tol.st_abs = o.tol.q_rel = 0.f;
                o.tol.ignore_nop_state = false;
            } else if (std::strcmp(argv[i], "--tol-xy") == 0) next_u(o.tol.xy);
            else if (std::strcmp(argv[i], "--tol-z") == 0) next_u(o.tol.z);
            else if (std::strcmp(argv[i], "--tol-rgba") == 0) next_u(o.tol.rgba);
            else if (std::strcmp(argv[i], "--max-lines") == 0) next_u(o.max_lines);
            else if (std::strcmp(argv[i], "--tol-st") == 0) {
                if (i + 1 < n) o.tol.st_rel = o.tol.q_rel = std::strtof(argv[++i], nullptr);
            } else case_path = argv[i];
        }
        if (!case_path) return Usage();
        static Vu1Case c;
        if (!vu1_case::ReadCase(case_path, c)) {
            std::fprintf(stderr, "vu1_replay: cannot load case %s\n", case_path);
            return 2;
        }
        return HleCompare(c, cycle_cap, o);
    }
    if (n >= 2 && std::strcmp(argv[1], "--catalog") == 0) {
        if (n < 3) return Usage();
        return Catalog(ExpandCasePaths(n, argv, 2), cycle_cap);
    }
    if (n >= 2 && std::strcmp(argv[1], "--registry") == 0) return RegistryReport();

    if (n >= 2 && std::strcmp(argv[1], "--mode") == 0) {
        if (n < 4) return Usage();
        VuTierMode mode;
        if (std::strcmp(argv[2], "interp") == 0) {
            mode = VuTierMode::Interp;
        } else if (std::strcmp(argv[2], "cached") == 0) {
            mode = VuTierMode::Cached;
        } else {
            std::fprintf(stderr, "vu1_replay: unknown --mode '%s'\n", argv[2]);
            return Usage();
        }
        static Vu1Case c;
        if (!LoadOrComplain(argv[3], c)) return 2;
        RunCase(c, mode, cycle_cap, (n >= 5) ? argv[4] : nullptr);
        std::fflush(nullptr);
        return 0;
    }

    if (n >= 2 && std::strcmp(argv[1], "--compare") == 0) {
        if (n < 3) return Usage();
        int expect_aot = 0;
        bool fabricate = false;
        bool compare_cycle = false;
        const char* case_path = nullptr;
        for (int i = 2; i < n; i++) {
            if (std::strcmp(argv[i], "--expect-aot") == 0) expect_aot = 1;
            else if (std::strcmp(argv[i], "--compare-cycle") == 0) compare_cycle = true;
            else if (std::strcmp(argv[i], "--expect-noaot") == 0) expect_aot = -1;
            else if (std::strcmp(argv[i], "--fabricate") == 0) fabricate = true;
            else case_path = argv[i];
        }
        if (expect_aot > 0) {
            std::fprintf(stderr,
                         "vu1_replay: SKIP --expect-aot: no native (AOT) tier exists in this tree; the cached-tier "
                         "compares remain the live bit-exactness contract.\n");
            return kSkipExit;
        }
        static Vu1Case c;
        if (fabricate) {
            FabricateCase(c);
        } else if (case_path == nullptr) {
            return Usage();
        } else if (!vu1_case::ReadCase(case_path, c)) {
            // An absent file is a corpus-dependent SKIP; a present but corrupt one is a hard error.
            struct stat st;
            if (stat(case_path, &st) != 0) {
                std::fprintf(stderr, "vu1_replay: case file absent, skipping: %s\n", case_path);
                return kSkipExit;
            }
            std::fprintf(stderr, "vu1_replay: cannot load case %s (bad path, magic, or version)\n", case_path);
            return 2;
        }
        return Compare(c, cycle_cap, expect_aot, compare_cycle);
    }

    if (n >= 2 && (std::strcmp(argv[1], "--bench") == 0 || std::strcmp(argv[1], "--bench-cached") == 0)) {
        if (n < 4) return Usage();
        const bool cached_only = std::strcmp(argv[1], "--bench-cached") == 0;
        const long runs = std::strtol(argv[2], nullptr, 0);
        if (runs <= 0) {
            std::fprintf(stderr, "vu1_replay: %s N must be > 0\n", argv[1]);
            return 2;
        }
        static Vu1Case c;
        if (!LoadOrComplain(argv[3], c)) return 2;
        if (!cached_only) BenchMode(c, VuTierMode::Interp, cycle_cap, runs);
        BenchMode(c, VuTierMode::Cached, cycle_cap, runs);
        return 0;
    }

    // Legacy form: an interpreter replay; `trace_out` is accepted for grammar
    // compatibility (no trace file is produced by this tree).
    if (n < 2) return Usage();
    const char* postdata_path = (n >= 4) ? argv[3] : nullptr;
    static Vu1Case c;
    if (!LoadOrComplain(argv[1], c)) return 2;
    RunCase(c, VuTierMode::Interp, cycle_cap, postdata_path);
    std::fflush(nullptr);
    return 0;
}
