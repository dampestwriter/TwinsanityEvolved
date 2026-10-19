// tools/gs_replay/gs_replay.cpp -- the corpus replay oracle over the NEW GS HLE
// layers and GL backend: replays a `.gsdl` draw-IR capture headlessly and emits
// NUMERIC comparison artifacts (per-frame framebuffer hashes, the chained
// REPLAY_HASH, the .gsrep downsample artifact, the census), or compares them.
//
//   gs_replay replay    <in.gsdl> [--out f.gsrep] [--ds N] [--frames a-b] [--quiet]
//                       [--skip SPEC] [--dump-ppm DIR] [--dump-frames a-b]   (exit 77 = no GL)
//   gs_replay census    <in.gsdl> [--frames a-b] [--configs]                  (CPU-only)
//   gs_replay diff      <a.gsrep> <b.gsrep> [--tol N] [--pct P] [--max N]     (exit 3 = DIFF)
//   gs_replay classdiff <a.gsdl> <b.gsdl> [--frames a-b] [--seq]              (CPU-only; exit 3 = DIFF)
//
// Exit codes: 0 ok; 1 a VERTS-OFF recording or an unwritable --out; 2 usage or a
// load failure; 3 a DIFF verdict; 77 no GL context (SKIP). Every knob is a flag:
// this tool reads no environment.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include PS2R_TITLE_RENDERER_PROFILE_H   // the configured title's renderer_profile/renderer_profile.h (CMake)
#include "tests/lib/gsdl_reader.h"
#include "tools/gs_replay/census.h"
#include "tools/gs_replay/frame_range.h"
#include "tools/gs_replay/replay_diff.h"
#include "tools/gs_replay/replay_driver.h"
#include "tools/gs_replay/skip_filter.h"

using namespace ps2r;
using ps2r::tools::FrameRange;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: gs_replay replay <in.gsdl> [--out f.gsrep] [--ds N]\n"
                 "                 [--frames a-b] [--quiet] [--skip SPEC]\n"
                 "                 [--dump-ppm DIR] [--dump-frames a-b]  (exit 77 = no GL)\n"
                 "       gs_replay census <in.gsdl> [--frames a-b] [--configs]\n"
                 "                                                 (CPU-only)\n"
                 "       gs_replay diff <a.gsrep> <b.gsrep> [--tol N] [--pct P]\n"
                 "                 [--max N]                       (exit 3 = DIFF)\n"
                 "       gs_replay classdiff <a.gsdl> <b.gsdl> [--frames a-b]\n"
                 "                 [--seq]        (CPU-only numeric; exit 3 = DIFF)\n");
    return 2;
}

bool ParseRange(const char* s, FrameRange* fr) {
    unsigned a = 0, b = UINT32_MAX;
    if (std::sscanf(s, "%u-%u", &a, &b) < 1) return false;
    fr->lo = a;
    fr->hi = b;
    return true;
}

int RunCensus(const gs::GsdlReader::Recording& rec, const FrameRange& fr, bool want_cfg) {
    const title::RendererProfile profile;
    tools::Census c(profile.ClassRules(), rec, want_cfg);
    for (const gs::GsdlReader::Frame& f : rec.frames) {
        if (!fr.Contains(f.vsync_frame)) continue;
        c.AddFrame();
        for (const gs::GsdlReader::Draw& d : f.draws) c.AddDraw(d.d);
        for (const gs::GsdlReader::Job& j : f.jobs) c.AddJob(j.j);
    }
    c.Print("[GSCENSUS]");
    return 0;
}

int MainDiff(int argc, char** argv) {
    if (argc < 4) return Usage();
    int tol = 0;
    double pct = 100.0;
    size_t max_lines = 10000;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--tol") && i + 1 < argc) tol = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pct") && i + 1 < argc) pct = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) max_lines = std::strtoul(argv[++i], nullptr, 0);
        else return Usage();
    }
    return tools::ReplayDiff::RunDiff(argv[2], argv[3], tol, pct, max_lines);
}

int MainClassDiff(int argc, char** argv) {
    if (argc < 4) return Usage();
    FrameRange fr;
    bool want_seq = false;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            (void)ParseRange(argv[++i], &fr);
        } else if (!std::strcmp(argv[i], "--seq")) {
            want_seq = true;
        } else {
            return Usage();
        }
    }
    const title::RendererProfile profile;
    return tools::ReplayDiff::RunClassDiff(profile.ClassRules(), argv[2], argv[3], fr, want_seq);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return Usage();
    const std::string cmd = argv[1];
    if (cmd == "diff") return MainDiff(argc, argv);
    if (cmd == "classdiff") return MainClassDiff(argc, argv);
    if (cmd != "census" && cmd != "replay") return Usage();

    tools::ReplayDriver::Options opt;
    opt.in_path = argv[2];
    tools::SkipFilter skip;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            opt.out_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--ds") && i + 1 < argc) {
            long v = std::strtol(argv[++i], nullptr, 0);
            if (v < 4) v = 4;
            if (v > 256) v = 256;
            opt.ds_n = static_cast<uint32_t>(v);
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            (void)ParseRange(argv[++i], &opt.range);
        } else if (!std::strcmp(argv[i], "--dump-frames") && i + 1 < argc) {
            (void)ParseRange(argv[++i], &opt.dump_range);
        } else if (!std::strcmp(argv[i], "--dump-ppm") && i + 1 < argc) {
            opt.dump_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--quiet")) {
            opt.quiet = true;
        } else if (!std::strcmp(argv[i], "--configs")) {
            opt.want_cfg = true;
        } else if (!std::strcmp(argv[i], "--skip") && i + 1 < argc) {
            if (!skip.Parse(argv[++i])) return Usage();
        } else {
            return Usage();
        }
    }

    gs::GsdlReader::Recording rec;
    if (!gs::GsdlReader::Load(argv[2], rec)) return 2;
    if (rec.truncated) std::fprintf(stderr, "[GSREPLAY] note: %s is truncated (tail frame kept)\n", argv[2]);
    if (cmd == "census") return RunCensus(rec, opt.range, opt.want_cfg);
    tools::ReplayDriver driver(opt, skip);
    return driver.Run(rec);
}
