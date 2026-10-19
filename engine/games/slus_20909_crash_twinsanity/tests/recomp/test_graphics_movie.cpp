// Root Movie composition owner: installation guards, control-arm counters and
// presentation-boundary failure visibility. No generated body, GPU, decoder or
// game clock runs here; this pins what the parent's game legs rely on before
// they execute (an Install refusal or a miscounted control arm costs a slot).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/graphics_movie.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ipu/hle/guest_memory.h"

namespace {
using Decoder = title::MovieDecoder;
using Root = ps2r::app::GraphicsMovie;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

struct Fixture : ps2r::ipu::GuestMemory {
    std::vector<uint8_t> ram = std::vector<uint8_t>(1u << 20);
    std::map<uint32_t, Decoder::Body> bindings;
    std::map<uint32_t, unsigned> originals;  // actual captured-body executions
    std::map<uint32_t, unsigned> interior;   // interior-entry resumes forwarded to the original
    uint32_t throw_at = 0;
    uint8_t* Translate(uint32_t addr, size_t len) override {
        return uint64_t(addr) + len <= ram.size() ? ram.data() + addr : nullptr;
    }
    uint32_t Read32(uint32_t addr) override {
        uint32_t v = 0; if (auto* p = Translate(addr, 4)) std::memcpy(&v, p, 4); return v;
    }
    void Write32(uint32_t addr, uint32_t value) override {
        if (auto* p = Translate(addr, 4)) std::memcpy(p, &value, 4);
    }
    Decoder::Capture Capture() {
        return [this](uint32_t at) -> Decoder::Body {
            if (bindings.count(at)) return bindings.at(at);
            return [this, at](CpuContext&, uint32_t entry) {
                // The control arm forwards the complete entry; the presentation
                // transport also forwards an INTERIOR entry (a restored session
                // resuming inside Present after its SyncV) to the original.
                if (entry && entry != at) ++interior[at]; else ++originals[at];
                if (throw_at == at) throw std::runtime_error("fixture original failed");
            };
        };
    }
    Decoder::Installer Install() {
        return [this](uint32_t at, Decoder::Body body) { bindings[at] = std::move(body); return true; };
    }
    void Call(uint32_t at, CpuContext& c) {
        Check(bindings.count(at) > 0, "address is installed");
        bindings.at(at)(c, at);
    }
    std::unique_ptr<Decoder> MakeDecoder(bool lifecycle) {
        Decoder::Hooks hooks;
        hooks.create = hooks.reset = [](CpuContext&) {};
        hooks.picture_rgb32 = hooks.picture_rgb16 = [](CpuContext&) { Check(false, "no guest picture plane"); };
        hooks.initialize = [] {};
        hooks.destroy = [](uint32_t) {};
        hooks.call = [this](CpuContext& c, uint32_t at) { Call(at, c); };
        hooks.has_pending = [](uint32_t) { return false; };
        auto decoder = std::make_unique<Decoder>(*this, hooks);
        Check(decoder->Install(Capture(), Install(), lifecycle) == (lifecycle ? 7 : 3), "decoder services");
        return decoder;
    }
};

std::string Field(const std::string& meta, const std::string& key) {
    const auto at = meta.find("\n" + key + "=");
    Check(at != std::string::npos, "census key is published");
    const auto end = meta.find('\n', at + 1);
    return meta.substr(at + key.size() + 2, end - at - key.size() - 2);
}
bool Has(const std::string& census, const char* pair) { return census.find(pair) != std::string::npos; }

std::string Render(const Root& root, const std::string& scratch, const char* name) {
    ps2r::verify::RunMeta::Identity id;
    id.build_id = "test";
    ps2r::verify::RunMeta meta(scratch + "/" + name + ".meta", id);
    root.Record(meta);
    return meta.Render();
}

void ControlArm(const std::string& scratch) {
    Fixture fx;
    auto decoder = fx.MakeDecoder(false);
    Root root(fx, *decoder, Root::Hooks{}, {});
    std::string reason;
    Check(!root.InstallPlayer(fx.Capture(), fx.Install(), &reason) && reason == "Movie player owners install after the Movie pictures",
          "player owners refuse, with the reason, before the pictures are installed");
    Check(root.Install(fx.Capture(), fx.Install()), "deselected root composition installs");
    Check(!root.Install(fx.Capture(), fx.Install(), &reason) && reason == "Graphics Movie is already installed",
          "second installation refuses with its reason");
    for (uint32_t at : {title::MovieStart::kAddress, title::MovieWait::kAddress, title::MovieStop::kAddress})
        Check(!fx.bindings.count(at), "the player owners are not installed with the pictures");
    Check(root.InstallPlayer(fx.Capture(), fx.Install()), "deselected player owners install after the pictures");
    Check(!root.InstallPlayer(fx.Capture(), fx.Install(), &reason) && reason == "Movie player owners are already installed",
          "second player installation refuses with its reason");
    for (uint32_t at : {Root::kInterruptPresent, Root::kPresent})
        Check(!fx.bindings.count(at), "deselected pictures leave the presentation transport uninstalled");
    CpuContext c{};
    for (uint32_t at : {title::MovieStep::kAddress, title::MovieStart::kAddress, title::MovieWait::kAddress,
                        title::MovieWait::kTail, title::MovieStop::kAddress, title::MovieStop::kCallback,
                        0x2b0e50u, 0x2af968u, 0x2aeb30u})
        fx.Call(at, c);
    for (uint32_t at : {title::MovieStep::kAddress, title::MovieStart::kAddress, title::MovieWait::kAddress,
                        title::MovieStop::kAddress, title::MovieStop::kCallback})
        Check(fx.originals[at] == 1, "control arm executes the actual captured body once");
    Check(root.CanCheckpoint(nullptr), "idle control composition is quiescent");
    const auto meta = Render(root, scratch, "control");
    Check(Has(Field(meta, "movie_step"), "selected=0,original_calls=1"), "movie_step counts actual delegation");
    Check(Has(Field(meta, "movie_start"), "selected=0,direct_presentation=0,original_calls=1"),
          "movie_start counts actual delegation without direct presentation");
    Check(Has(Field(meta, "movie_wait"), "selected=0,original_calls=1,tail_calls=1"),
          "movie_wait counts the wrapper and its retained tail");
    Check(Has(Field(meta, "movie_stop"), "selected=0,original_calls=1,callback_calls=1,stops=0"),
          "movie_stop counts the actual Stop body and the actual VBlank callback");
    Check(Has(Field(meta, "movie_graphics"), "decode_original=1,queue_original=1,draw_original=1"),
          "movie_graphics counts the three actual retained picture producers");
    Check(Field(meta, "graphics_movie_transport") == "interrupt_model_calls=0,present_model_calls=0,interior_resumes=0,failed=0,release_refused=0",
          "no transport is counted when pictures are deselected");
}

void SelectionGuards() {
    Fixture fx;
    auto decoder = fx.MakeDecoder(true);
    std::string reason;
    {
        Root::Hooks hooks;
        hooks.stop = true; // without start/wait/step: the validator refuses this too
        Root root(fx, *decoder, hooks, {});
        Check(!root.Install(fx.Capture(), fx.Install(), &reason) && reason.find("paired Start, WaitFrame and Step") != std::string::npos,
              "Stop without paired Start/Wait/Step refuses with its reason");
    }
    {
        Root::Hooks hooks;
        hooks.selected = true; // pictures without the root picture/submission/select/clear services
        Root root(fx, *decoder, hooks, {});
        Check(!root.Install(fx.Capture(), fx.Install(), &reason) && reason.find("picture/submission/select/clear") != std::string::npos,
              "selected pictures require complete root services, with the reason");
    }
    {
        Root::Hooks hooks;
        hooks.step = true; // owned Step needs selected pictures first
        Root root(fx, *decoder, hooks, {});
        Check(!root.Install(fx.Capture(), fx.Install(), &reason) && reason.find("owned Movie pictures") != std::string::npos,
              "owned Step without owned pictures refuses with its reason");
    }
    {
        // The gate-3 root defect: a selected Start over an incomplete service
        // mask (here lifecycle + picture services only) is a visible refusal
        // naming the missing services, never a silent exit.
        Root::Hooks hooks;
        hooks.selected = true; hooks.step = true; hooks.start = true;
        hooks.call = [&fx](CpuContext& c, uint32_t at) { fx.Call(at, c); };
        hooks.picture = [](CpuContext&, ps2r::ipu::DecodedPicture&) {};
        hooks.submitted = [](Root::Frame&) { return true; };
        hooks.select = [](const Root::Frame&) { return true; };
        hooks.clear = [] { return true; };
        Root root(fx, *decoder, hooks, {[](auto) { return true; }, [](const auto&, auto) { return true; }});
        Check(!root.Install(fx.Capture(), fx.Install(), &reason) && reason.find("Movie Step requires the Movie lifecycle and audio services") != std::string::npos,
              "selected Step over lifecycle-only services refuses with the missing services");
        hooks.step = false;
        Root pictures(fx, *decoder, hooks, {[](auto) { return true; }, [](const auto&, auto) { return true; }});
        Check(pictures.Install(fx.Capture(), fx.Install(), &reason), "selected pictures install over lifecycle services");
        Check(!pictures.InstallPlayer(fx.Capture(), fx.Install(), &reason), "selected Start over an incomplete mask refuses");
        Check(reason.find("selected Movie Start requires the complete Movie service mask: have 0x41, missing ") == 0 &&
              reason.find("player buffers (MovieStorage") != std::string::npos && reason.find("stream services") != std::string::npos &&
              reason.find("picture services") == std::string::npos,
              "the refusal names the mask and exactly the missing services");
        Check(Root::MissingServices(95u) == "player buffers (MovieStorage: hle.music_buffers with hle.stream_files, installed before the player owners)" &&
              Root::MissingServices(127u).empty(),
              "the gate-3 mask 95 names only the player buffers; 127 is complete");
        for (uint32_t at : {title::MovieStart::kAddress, title::MovieWait::kAddress, title::MovieStop::kAddress})
            Check(!fx.bindings.count(at), "a refused player owner installs nothing");
    }
}

void PresentationBoundary(const std::string& scratch) {
    Fixture fx;
    auto decoder = fx.MakeDecoder(true);
    unsigned submitted = 0, selected = 0, cleared = 0;
    Root::Hooks hooks;
    hooks.selected = true;
    hooks.call = [&fx](CpuContext& c, uint32_t at) { fx.Call(at, c); };
    hooks.picture = [](CpuContext&, ps2r::ipu::DecodedPicture&) { Check(false, "no picture is served"); };
    hooks.submitted = [&](Root::Frame&) { ++submitted; return true; };
    hooks.select = [&](const Root::Frame&) { ++selected; return true; };
    hooks.clear = [&] { ++cleared; return true; };
    Root root(fx, *decoder, hooks, {[](auto) { return true; }, [](const auto&, auto) { return true; }});
    Check(root.Install(fx.Capture(), fx.Install()), "selected pictures install over the complete services");
    Check(root.InstallPlayer(fx.Capture(), fx.Install()), "deselected player owners install beside selected pictures");
    for (uint32_t at : {Root::kInterruptPresent, Root::kPresent})
        Check(fx.bindings.count(at) == 1, "selected pictures own both presentation boundaries");
    CpuContext c{};
    fx.Call(Root::kPresent, c);
    fx.Call(Root::kInterruptPresent, c); // no active movie: release, never select
    Check(fx.originals[Root::kPresent] == 1 && fx.originals[Root::kInterruptPresent] == 1,
          "retained transport still executes once per boundary");
    Check(submitted == 0 && selected == 0 && cleared == 2, "inactive movie releases the frame on both boundaries");
    Check(root.CanCheckpoint(nullptr), "released presentation is quiescent");
    // An interior entry is the restored session resuming inside Present after
    // its SyncV (the savestate boundary lives there): the original finishes
    // that frame's presentation, no owned select/release is re-derived, no
    // fault; counted as an interior resume. The transport keeps the original's
    // continuation proof for exactly this (DispatchTable::TraceBody in the root).
    bool failed = false;
    try { fx.bindings.at(Root::kPresent)(c, 0x1a0154); } catch (const std::runtime_error&) { failed = true; }
    Check(!failed && fx.originals[Root::kPresent] == 1 && fx.interior[Root::kPresent] == 1 && cleared == 2,
          "an interior entry resumes the original transport without owned work or a fault");
    Check(root.CanCheckpoint(nullptr), "the resumed boundary stays quiescent");
    const auto meta = Render(root, scratch, "presentation");
    Check(Field(meta, "graphics_movie_transport") == "interrupt_model_calls=1,present_model_calls=1,interior_resumes=1,failed=0,release_refused=0",
          "transport census counts actual retained calls and the interior resume");
    Check(Has(Field(meta, "movie_graphics"), "decode_original=0,queue_original=0,draw_original=0"),
          "owned pictures execute no retained picture producer");

    Fixture other;
    auto second = other.MakeDecoder(true);
    other.throw_at = Root::kInterruptPresent;
    Root root2(other, *second, hooks, {[](auto) { return true; }, [](const auto&, auto) { return true; }});
    Check(root2.Install(other.Capture(), other.Install()) && root2.InstallPlayer(other.Capture(), other.Install()),
          "second root installs");
    failed = false;
    try { other.Call(Root::kInterruptPresent, c); } catch (const std::runtime_error&) { failed = true; }
    Check(failed && !root2.CanCheckpoint(nullptr), "a failing retained transport marks the owner failed, no fallback");

    // The all-on frame-63 shape: the presentation RELEASE (no active movie)
    // is owned work at the ordered slot; the owned output refusing it (stopped,
    // or faulted by another group) releases nothing and is counted -- it is
    // not the transport's fault and never throws out of the title's Present.
    Fixture third;
    auto decoder3 = third.MakeDecoder(true);
    unsigned refused = 0;
    auto refusing = hooks;
    refusing.clear = [&] { ++refused; return false; };
    Root root3(third, *decoder3, refusing, {[](auto) { return true; }, [](const auto&, auto) { return true; }});
    Check(root3.Install(third.Capture(), third.Install()) && root3.InstallPlayer(third.Capture(), third.Install()),
          "third root installs");
    bool threw = false;
    try { third.Call(Root::kPresent, c); third.Call(Root::kInterruptPresent, c); } catch (...) { threw = true; }
    Check(!threw && refused == 2 && third.originals[Root::kPresent] == 1 && third.originals[Root::kInterruptPresent] == 1,
          "a refused presentation release is counted, never a fault of the frame");
    Check(root3.CanCheckpoint(nullptr), "a refused release leaves the transport quiescent");
    const auto meta3 = Render(root3, scratch, "release-refused");
    Check(Field(meta3, "graphics_movie_transport") == "interrupt_model_calls=1,present_model_calls=1,interior_resumes=0,failed=0,release_refused=2",
          "transport census counts the refused releases");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string scratch = argv[1];
    std::filesystem::create_directories(scratch);
    ControlArm(scratch);
    SelectionGuards();
    PresentationBoundary(scratch);
    std::printf("GraphicsMovie: %u checks; control-arm counters, selection guards with reasons, player-owner install order and presentation boundary.\n", checks);
    return 0;
}
