#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Frame = title::FrameEnvironment;
using Environment = title::MaterialState::Environment;
using Status = Frame::Status;
int checks = 0, failures = 0;

void Check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

std::array<uint64_t, 9> Fields(const Environment& e) {
    return {e.colour_clamp, e.per_pixel_alpha, e.scan_mask, e.fog_r, e.fog_g,
            e.fog_b, e.dither, e.dither_matrix, 0};
}

void Unavailable(const Frame& frame, Status expected) {
    Environment out{false, true, 3, 17, 19, 23, 1, 0x123456789abcdef0ull};
    const auto before = Fields(out);
    CHECK(frame.CurrentStatus() == expected);
    CHECK(!frame.Read(out));
    CHECK(Fields(out) == before);
}

Environment Ready(const Frame& frame) {
    Environment out{false, true, 3, 17, 19, 23, 1, 0x123456789abcdef0ull};
    CHECK(frame.CurrentStatus() == Status::Ready);
    CHECK(frame.Read(out));
    CHECK(out.colour_clamp);
    CHECK(!out.per_pixel_alpha && out.scan_mask == 0);
    CHECK(out.fog_r == 0 && out.fog_g == 0 && out.fog_b == 0);
    CHECK(out.dither == 0 && out.dither_matrix == 0x6071243571603524ull);
    return out;
}

struct TestFrameEnvironment {
    static constexpr std::array<uint32_t, Frame::kBindingCount> addresses{
        Frame::kInitDisplayAddress, Frame::kWriteFrameHeadAddress, Frame::kSetUpFrameAddress,
        Frame::kFinishSkyAddress, Frame::kBeginShadowsAddress, Frame::kEndShadowsAddress,
        Frame::kLinkAddress, Frame::kResetGraphAddress};
    std::map<uint32_t, Frame::Body> handlers;
    std::map<uint32_t, Frame::Body> actions;
    std::vector<std::pair<uint32_t, uint32_t>> calls;
    size_t captures = 0, installs = 0;
    uint32_t reject = 0, missing = 0;
    CpuContext context{};

    TestFrameEnvironment() {
        for (uint32_t address : addresses) {
            handlers[address] = [this, address](CpuContext& c, uint32_t entry) {
                calls.emplace_back(address, entry);
                c.cpuRegs.GPR.r[2].UD[0] = 0xfeed000000000000ull | address;
                if (actions[address]) actions[address](c, entry);
            };
        }
    }
    bool Install(Frame& frame) {
        return frame.Install(
            [this](uint32_t address) {
                ++captures;
                return address == missing ? Frame::Body{} : handlers.at(address);
            },
            [this](uint32_t address, Frame::Body body) {
                ++installs;
                if (address == reject) return false;
                handlers[address] = std::move(body);
                return true;
            });
    }
    void Call(uint32_t address) { handlers.at(address)(context, address); }
    void Interior(uint32_t address) { handlers.at(address)(context, address + 4); }
    std::map<uint32_t, uint32_t> words;
    size_t reads = 0;
    uint32_t missing_word = 0;
    bool prohibit_reads = false;
    static constexpr uint32_t target = 0x12000;
    title::MaterialState::Read Reader() {
        return [this](uint32_t address, void* out, size_t size) {
            ++reads;
            CHECK(!prohibit_reads);
            if (prohibit_reads || size % 4) return false;
            for (size_t offset = 0; offset < size; offset += 4) {
                const uint32_t at = address + uint32_t(offset);
                const auto found = words.find(at);
                if (at == missing_word || found == words.end()) return false;
                std::memcpy(static_cast<uint8_t*>(out) + offset, &found->second, 4);
            }
            return true;
        };
    }
    void Display(uint32_t width = 512, uint32_t height = 448,
                 uint32_t color_page = 0x38, uint32_t depth_page = 0xa8) {
        words = {{title::MaterialState::kDisplayWidth,width},
                 {title::MaterialState::kDisplayHeight,height},
                 {title::MaterialState::kDisplayPixelBytes,2},
                 {Frame::kDrawPixelBytes,4},
                 {Frame::kFrameBufferPage,color_page},
                 {title::MaterialState::kDepthBufferPage,depth_page},
                 {target+0x18,width},{target+0x1c,height}};
        context.cpuRegs.GPR.r[4].UL[0] = target;
    }
    void Link(uint32_t address = Frame::kFrameBuckets) {
        context.cpuRegs.GPR.r[4].UL[0] = address;
        Call(Frame::kLinkAddress);
        context.cpuRegs.GPR.r[4].UL[0] = target;
    }
};

void TestColdBootAndCompleteBoundaries() {
    TestFrameEnvironment f;
    Frame frame;
    CHECK(f.Install(frame));
    CHECK(f.captures == Frame::kBindingCount && f.installs == Frame::kBindingCount);
    Unavailable(frame, Status::NoFrameBoundary);
    f.actions[Frame::kInitDisplayAddress] = [&](CpuContext&, uint32_t) {
        f.Call(Frame::kResetGraphAddress);
        f.Call(Frame::kWriteFrameHeadAddress);
        Unavailable(frame, Status::IncompleteOperation);
    };
    f.Call(Frame::kInitDisplayAddress);
    CHECK(frame.Observed().complete_display_initializations == 1);
    CHECK(frame.Observed().dither_matrix);
    Unavailable(frame, Status::NoFrameBoundary);  // startup alone is not a frame

    f.context.cpuRegs.GPR.r[4].UD[0] = 0x9000;
    f.context.cpuRegs.GPR.r[5].UD[0] = 0;  // neither clear flag gates the environment writes
    f.context.cpuRegs.GPR.r[6].UD[0] = 0;
    f.context.cpuRegs.GPR.r[16].UD[0] = 0x1111222233334444ull;
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext& c, uint32_t entry) {
        CHECK(entry == Frame::kSetUpFrameAddress);
        CHECK(c.cpuRegs.GPR.r[4].UD[0] == 0x9000);
        CHECK(c.cpuRegs.GPR.r[5].UD[0] == 0 && c.cpuRegs.GPR.r[6].UD[0] == 0);
        Unavailable(frame, Status::IncompleteOperation);
        c.cpuRegs.GPR.r[2].UD[0] = 0x123456789abcdeffull;
    };
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);
    CHECK(frame.Observed().completed_frame_boundaries == 1);
    CHECK(f.context.cpuRegs.GPR.r[2].UD[0] == 0x123456789abcdeffull);
    CHECK(f.context.cpuRegs.GPR.r[16].UD[0] == 0x1111222233334444ull);
    CHECK(f.Install(frame));  // no duplicate wrappers
    CHECK(f.captures == Frame::kBindingCount && f.installs == Frame::kBindingCount);
}

void TestSkyShadowsAndEpochs() {
    TestFrameEnvironment f;
    Frame frame;
    CHECK(f.Install(frame));
    f.Call(Frame::kFinishSkyAddress);  // scene has a sky and skips SetUpFrame
    const auto owned = Ready(frame);
    CHECK(!frame.Observed().dither_matrix);  // inactive native matrix needs no startup replay
    f.Call(Frame::kBeginShadowsAddress);
    CHECK(frame.Observed().shadow_open);
    Unavailable(frame, Status::IncompleteOperation);
    f.actions[Frame::kEndShadowsAddress] = [&](CpuContext&, uint32_t) {
        Unavailable(frame, Status::IncompleteOperation);
    };
    f.Call(Frame::kEndShadowsAddress);
    CHECK(!frame.Observed().shadow_open);
    Ready(frame);
    f.Call(Frame::kLinkAddress);
    Unavailable(frame, Status::NoFrameBoundary);
    CHECK(owned.colour_clamp && owned.dither == 0 && owned.scan_mask == 0);
    f.Call(Frame::kEndShadowsAddress);
    Unavailable(frame, Status::NoFrameBoundary);  // shadow end cannot establish a frame
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);
    CHECK(frame.Observed().completed_frame_boundaries == 2);
}

void TestRestoreAndInteriors() {
    TestFrameEnvironment f;
    Frame frame;
    CHECK(f.Install(frame));
    f.Call(Frame::kWriteFrameHeadAddress);
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);
    const auto before = frame.Observed();
    frame.ResetAfterQuiescence();
    CHECK(!frame.Observed().dither_matrix);
    CHECK(frame.Observed().completed_frame_boundaries == before.completed_frame_boundaries);
    Unavailable(frame, Status::NoFrameBoundary);

    // Restored partial callers retain their original continuation and stack.
    // Even a complete nested operation cannot certify the unfinished caller.
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&, uint32_t entry) {
        if (entry != Frame::kSetUpFrameAddress) f.Call(Frame::kFinishSkyAddress);
    };
    f.Interior(Frame::kSetUpFrameAddress);
    CHECK(f.calls[f.calls.size() - 2].second == Frame::kSetUpFrameAddress + 4);
    CHECK(frame.Observed().interior_entries == 1);
    Unavailable(frame, Status::NoFrameBoundary);
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);  // fresh source boundary resumes after restore without InitDisplay
    for (uint32_t address : {Frame::kLinkAddress, Frame::kWriteFrameHeadAddress,
                             Frame::kResetGraphAddress, Frame::kFinishSkyAddress}) {
        f.Interior(address);
        Unavailable(frame, Status::NoFrameBoundary);
        f.Call(Frame::kSetUpFrameAddress);
        Ready(frame);
    }
    f.Call(Frame::kResetGraphAddress);
    Unavailable(frame, Status::NoFrameBoundary);
    f.handlers.at(Frame::kSetUpFrameAddress)(f.context, 0);  // dispatcher entry alias
    Ready(frame);
}

void TestFailedBodiesDoNotEstablishReadiness() {
    TestFrameEnvironment f;
    Frame frame;
    CHECK(f.Install(frame));
    f.Call(Frame::kSetUpFrameAddress);
    const auto completed = frame.Observed().completed_frame_boundaries;
    f.actions[Frame::kFinishSkyAddress] = [&](CpuContext&, uint32_t) {
        Unavailable(frame, Status::IncompleteOperation);
        throw std::runtime_error("source body failed");
    };
    bool failed = false;
    try { f.Call(Frame::kFinishSkyAddress); }
    catch (const std::runtime_error&) { failed = true; }
    CHECK(failed);
    CHECK(frame.Observed().completed_frame_boundaries == completed);
    Unavailable(frame, Status::NoFrameBoundary);
    frame.ResetAfterQuiescence();  // exception unwound the operation depth
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&, uint32_t) {
        bool refused = false;
        try { frame.ResetAfterQuiescence(); }
        catch (const std::logic_error&) { refused = true; }
        CHECK(refused);
        Unavailable(frame, Status::IncompleteOperation);
    };
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);
}

void TestInstallPreflightAndRollback() {
    Frame unused;
    CHECK(!unused.Install({}, {}));
    TestFrameEnvironment missing;
    missing.missing = Frame::kResetGraphAddress;
    CHECK(!missing.Install(unused));
    CHECK(missing.installs == 0);
    for (uint32_t rejected : TestFrameEnvironment::addresses) {
        TestFrameEnvironment f;
        Frame frame;
        f.reject = rejected;
        CHECK(!f.Install(frame));
        for (uint32_t address : TestFrameEnvironment::addresses) f.Call(address);
        CHECK(frame.Observed().completed_frame_boundaries == 0);
        CHECK(frame.Observed().complete_display_initializations == 0);
        Unavailable(frame, Status::NoFrameBoundary);
        f.reject = 0;
        CHECK(f.Install(frame));
        f.Call(Frame::kSetUpFrameAddress);
        Ready(frame);
    }
}

void TestCompositionWithSubmissionWrapper() {
    for (bool observer_first : {false, true}) {
        TestFrameEnvironment f;
        Frame frame;
        size_t submits = 0;
        auto install_submission = [&] {
            const auto previous = f.handlers.at(Frame::kLinkAddress);
            f.handlers[Frame::kLinkAddress] = [&, previous](CpuContext& c, uint32_t entry) {
                ++submits;
                previous(c, entry);
            };
        };
        if (!observer_first) install_submission();
        CHECK(f.Install(frame));
        if (observer_first) install_submission();
        f.Call(Frame::kSetUpFrameAddress);
        const auto pending_draw_environment = Ready(frame);
        f.actions[Frame::kLinkAddress] = [&](CpuContext&, uint32_t) {
            Unavailable(frame, Status::IncompleteOperation);
        };
        f.Call(Frame::kLinkAddress);
        CHECK(submits == 1);
        CHECK(f.calls.back() == std::make_pair(Frame::kLinkAddress, Frame::kLinkAddress));
        CHECK(pending_draw_environment.colour_clamp && !pending_draw_environment.per_pixel_alpha);
        Unavailable(frame, Status::NoFrameBoundary);
    }
}

using SourceFixture = TestFrameEnvironment;
std::array<uint64_t, 6> SceneFields(const Frame::SceneFrame& frame) {
    return {frame.color_buffer,frame.depth_buffer,frame.width,frame.height,frame.source_frame,frame.epoch};
}
void NoScene(const Frame& frame, uint32_t bucket = 27) {
    Frame::SceneFrame out{1,2,3,4,5,6};
    const auto before = SceneFields(out);
    CHECK(!frame.CaptureScene(bucket,out));
    CHECK(SceneFields(out) == before);
}
void TestSourceSceneSnapshot() {
    SourceFixture f;
    f.Display();
    Frame frame(f.Reader());
    CHECK(f.Install(frame));
    NoScene(frame);
    f.Call(Frame::kInitDisplayAddress);
    f.Call(Frame::kWriteFrameHeadAddress);
    NoScene(frame); // allocation/display initialization is not completed scene setup
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&, uint32_t) { NoScene(frame); };
    f.Call(Frame::kSetUpFrameAddress);
    Frame::SceneFrame first;
    CHECK(frame.CaptureScene(27,first));
    CHECK(SceneFields(first) == (std::array<uint64_t,6>{0x70000,0x150000,512,448,0,1}));
    const auto captured_reads = f.reads;
    f.prohibit_reads = true;
    f.words.clear(); // no source pointer survives capture
    // TR renderer.h:431 family buckets share the frame's scene tuple.
    for (uint32_t bucket : {2u,3u,4u,6u,19u,20u,21u,22u,23u,24u,26u,27u}) {
        Frame::SceneFrame copy;
        CHECK(Frame::SceneBucket(bucket));
        CHECK(frame.CaptureScene(bucket,copy));
        CHECK(SceneFields(copy) == SceneFields(first));
    }
    for (uint32_t bucket : {0u,1u,5u,25u,28u,0xffffffffu}) {
        CHECK(!Frame::SceneBucket(bucket));
        NoScene(frame,bucket);
    }
    CHECK(f.reads == captured_reads);
    CHECK(!frame.Linked()); // no primary Link has completed yet (the title's first Send sees this)
    f.Link();
    NoScene(frame);
    CHECK(frame.Linked());
    Frame::SceneFrame submitted;
    CHECK(frame.CaptureSubmittedScene(submitted));
    CHECK(SceneFields(submitted) == SceneFields(first));
    CHECK(f.reads == captured_reads); // submission/present capture never rereads RAM
    CHECK(first.epoch == 1 && first.width == 512); // already queued owned value survives Link
    f.prohibit_reads = false;
    f.Display(640,512,0x50,0xf0);
    f.Call(Frame::kFinishSkyAddress);
    CHECK(frame.CaptureSubmittedScene(submitted));
    CHECK(SceneFields(submitted) == SceneFields(first)); // next setup cannot overwrite submitted output
    Frame::SceneFrame second;
    CHECK(frame.CaptureScene(24,second));
    CHECK(SceneFields(second) == (std::array<uint64_t,6>{0xa0000,0x1e0000,640,512,1,2}));
    // Shadow bracket (TR chunkscenery.cpp:1617): only bucket 21 is admitted
    // while it is open, including from inside either bracket body; a nested
    // non-bracket operation still refuses it.
    Frame::SceneFrame shadows;
    // CaptureEffects (buckets 4/21/23): the same tuple under the same admission
    // plus the frame-wide head environment; UI/world buckets refuse it.
    {
        Frame::SceneFrame effects;
        title::MaterialState::Environment environment;
        for (uint32_t bucket : {4u, 21u, 23u}) {
            effects = {};
            CHECK(frame.CaptureEffects(bucket,effects,environment) && SceneFields(effects) == SceneFields(second));
            CHECK(environment.colour_clamp && !environment.dither && environment.dither_matrix == 0x6071243571603524ull);
            CHECK(!environment.per_pixel_alpha && !environment.scan_mask && !environment.fog_r && !environment.fog_g && !environment.fog_b);
        }
        for (uint32_t bucket : {0u, 1u, 2u, 5u, 20u, 22u, 24u, 26u, 27u, 28u})
            CHECK(!frame.CaptureEffects(bucket,effects,environment) && !Frame::EffectBucket(bucket));
        CHECK(Frame::EffectBucket(4) && Frame::EffectBucket(21) && Frame::EffectBucket(23));
    }
    // CaptureMesh (the owned mesh bucket: opaque 2/3; blended 6-19): the
    // same tuple plus the head/sky environment with the fog bytes zero; sky, the
    // effect buckets, decals, particles, UI, fonts, movies refuse it.
    {
        Frame::SceneFrame mesh;
        title::MaterialState::Environment environment;
        for (uint32_t bucket : {2u, 3u, 6u, 9u, 19u}) {
            mesh = {};
            environment = {};
            CHECK(frame.CaptureMesh(bucket,mesh,environment) && SceneFields(mesh) == SceneFields(second));
            CHECK(environment.colour_clamp && !environment.dither && environment.dither_matrix == 0x6071243571603524ull);
            CHECK(!environment.per_pixel_alpha && !environment.scan_mask && !environment.fog_r && !environment.fog_g && !environment.fog_b);
            CHECK(Frame::MeshBucket(bucket));
        }
        for (uint32_t bucket : {0u, 1u, 4u, 5u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u})
            CHECK(!frame.CaptureMesh(bucket,mesh,environment) && !Frame::MeshBucket(bucket));
    }
    f.actions[Frame::kBeginShadowsAddress] = [&](CpuContext&, uint32_t) {
        CHECK(frame.CaptureScene(21,shadows) && SceneFields(shadows) == SceneFields(second));
        Frame::SceneFrame effects;
        title::MaterialState::Environment environment;
        CHECK(frame.CaptureEffects(21,effects,environment) && SceneFields(effects) == SceneFields(second));
        CHECK(!frame.CaptureEffects(23,effects,environment) && !frame.CaptureEffects(4,effects,environment));
        CHECK(!frame.CaptureMesh(2,effects,environment) && !frame.CaptureMesh(3,effects,environment)); // the bracket is open
        NoScene(frame,23);
        NoScene(frame,2);
    };
    f.Call(Frame::kBeginShadowsAddress);
    f.actions[Frame::kBeginShadowsAddress] = {};
    NoScene(frame);
    NoScene(frame,4);
    NoScene(frame,23);
    shadows = {};
    CHECK(frame.CaptureScene(21,shadows) && SceneFields(shadows) == SceneFields(second));
    f.actions[Frame::kWriteFrameHeadAddress] = [&](CpuContext&, uint32_t) { NoScene(frame,21); };
    f.Call(Frame::kWriteFrameHeadAddress);
    f.actions[Frame::kWriteFrameHeadAddress] = {};
    NoScene(frame,21); // the head invalidated the frame's scene tuple for every bucket
    f.actions[Frame::kEndShadowsAddress] = [&](CpuContext&, uint32_t) { NoScene(frame,21); };
    f.Call(Frame::kEndShadowsAddress);
    f.actions[Frame::kEndShadowsAddress] = {};
    f.Call(Frame::kFinishSkyAddress);
    CHECK(frame.CaptureScene(24,second) && second.epoch == 3);
    f.Call(Frame::kBeginShadowsAddress);
    NoScene(frame);
    NoScene(frame,2);
    CHECK(frame.CaptureScene(21,shadows) && SceneFields(shadows) == SceneFields(second));
    f.actions[Frame::kEndShadowsAddress] = [&](CpuContext&, uint32_t) {
        CHECK(frame.CaptureScene(21,shadows) && SceneFields(shadows) == SceneFields(second));
        NoScene(frame,24);
    };
    f.Call(Frame::kEndShadowsAddress);
    f.actions[Frame::kEndShadowsAddress] = {};
    CHECK(frame.CaptureScene(26,second) && second.epoch == 3);
    CHECK(frame.CaptureScene(21,shadows) && SceneFields(shadows) == SceneFields(second));
    f.Link(0x13200); // foreign small bucket is not a source frame
    CHECK(frame.Linked()); // the earlier primary Link still counts
    f.Call(Frame::kSetUpFrameAddress);
    CHECK(frame.CaptureScene(27,second) && second.source_frame == 1);
    f.Link(Frame::kFrameBuckets | 0x80000000);
    CHECK(frame.CaptureSubmittedScene(submitted));
    CHECK(submitted.epoch == second.epoch && submitted.source_frame == second.source_frame);
    f.context.cpuRegs.GPR.r[4].UL[0] = SourceFixture::target | 0xa0000000;
    f.Call(Frame::kSetUpFrameAddress);
    CHECK(frame.CaptureScene(27,second) && second.source_frame == 2);
    frame.ResetAfterQuiescence();
    CHECK(!frame.CaptureSubmittedScene(submitted));
    CHECK(!frame.Linked()); // the Link ordinal is reset with the source frame
}
void TestSourceSceneResetAndInteriors() {
    SourceFixture f;
    f.Display();
    Frame frame(f.Reader());
    CHECK(f.Install(frame));
    f.Call(Frame::kSetUpFrameAddress);
    Frame::SceneFrame before;
    CHECK(frame.CaptureScene(27,before));
    f.Link();
    frame.ResetAfterQuiescence();
    NoScene(frame);
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&, uint32_t at) {
        if (at != Frame::kSetUpFrameAddress) f.Call(Frame::kFinishSkyAddress);
    };
    f.Interior(Frame::kSetUpFrameAddress);
    NoScene(frame); // a nested full FinishSky cannot certify a restored partial caller
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&, uint32_t) {
        f.Interior(Frame::kWriteFrameHeadAddress);
        f.Call(Frame::kFinishSkyAddress);
    };
    f.Call(Frame::kSetUpFrameAddress);
    NoScene(frame); // an interior nested operation poisons the outer source snapshot
    f.actions[Frame::kSetUpFrameAddress] = {};
    f.Call(Frame::kSetUpFrameAddress);
    Frame::SceneFrame after;
    CHECK(frame.CaptureScene(27,after));
    CHECK(after.source_frame == 0 && after.epoch > before.epoch);
    f.actions[Frame::kFinishSkyAddress] = [](CpuContext&, uint32_t) {
        throw std::runtime_error("incomplete source setup");
    };
    try { f.Call(Frame::kFinishSkyAddress); CHECK(false); }
    catch (const std::runtime_error&) {}
    NoScene(frame);
    f.Call(Frame::kSetUpFrameAddress);
    f.Call(Frame::kWriteFrameHeadAddress);
    NoScene(frame); // refreshed display-copy layout requires a new completed scene setup
    f.Call(Frame::kSetUpFrameAddress);
    f.Call(Frame::kResetGraphAddress);
    NoScene(frame);
}
void TestSourceSceneValidation() {
    // Literal retail-size cases: source globals and public target ABI agree.
    for (const auto& row : std::array<std::array<uint32_t,4>,4>{{
            {512,448,0x38,0xa8},{512,512,0x40,0xc0},
            {640,448,0x46,0xd2},{640,512,0x50,0xf0}}}) {
        SourceFixture f;
        f.Display(row[0],row[1],row[2],row[3]);
        Frame frame(f.Reader());
        CHECK(f.Install(frame));
        f.Call(Frame::kSetUpFrameAddress);
        Frame::SceneFrame value;
        CHECK(frame.CaptureScene(27,value));
        CHECK(value.color_buffer == row[2]*8192 && value.depth_buffer == row[3]*8192);
    }
    for (unsigned variant = 0; variant < 14; ++variant) {
        SourceFixture f;
        f.Display();
        Frame frame(f.Reader());
        CHECK(f.Install(frame));
        switch (variant) {
        case 0: f.words[title::MaterialState::kDisplayWidth] = 0; break;
        case 1: f.words[title::MaterialState::kDisplayWidth] = 513; break;
        case 2: f.words[title::MaterialState::kDisplayHeight] = 0; break;
        case 3: f.words[title::MaterialState::kDisplayPixelBytes] = 4; break;
        case 4: f.words[Frame::kDrawPixelBytes] = 2; break;
        case 5: ++f.words[Frame::kFrameBufferPage]; break;
        case 6: ++f.words[title::MaterialState::kDepthBufferPage]; break;
        case 7: f.words[SourceFixture::target+0x18] = 256; break;
        case 8: f.words[SourceFixture::target+0x1c] = 224; break;
        case 9: f.context.cpuRegs.GPR.r[4].UL[0] = 0xfffffff0; break;
        case 10: f.context.cpuRegs.GPR.r[4].UL[0] = 0x10000000; break;
        case 11: f.Display(2048,2048,1024,3072); break;
        case 12: f.missing_word = Frame::kFrameBufferPage; break;
        case 13: f.missing_word = title::MaterialState::kDepthBufferPage; break;
        }
        f.Call(Frame::kSetUpFrameAddress);
        NoScene(frame);
        Ready(frame); // the accepted plain Strip environment contract is unchanged
    }
    // Missing constructor reader does not grant invented defaults.
    TestFrameEnvironment f;
    Frame frame;
    CHECK(f.Install(frame));
    f.Call(Frame::kSetUpFrameAddress);
    Ready(frame);
    NoScene(frame);
}
void TestSkySourceBeforeSetup() {
    SourceFixture f;
    f.Display();
    f.words[title::MaterialState::kRenderTarget] = SourceFixture::target;
    Frame frame(f.Reader());
    CHECK(f.Install(frame));
    Frame::SceneFrame sky{1,2,3,4,5,6};
    Environment environment;
    const auto absent = [&] {
        const auto previous = SceneFields(sky);
        const auto state = Fields(environment);
        CHECK(!frame.CaptureSky(sky, environment));
        CHECK(SceneFields(sky) == previous && Fields(environment) == state);
    };
    absent(); // startup/restored device defaults cannot certify inherited intent
    f.actions[Frame::kWriteFrameHeadAddress] = [&](CpuContext&, uint32_t) { absent(); };
    f.Call(Frame::kWriteFrameHeadAddress);
    CHECK(frame.CaptureSky(sky, environment));
    CHECK(SceneFields(sky) == (std::array<uint64_t,6>{0x70000,0x150000,512,448,0,1}));
    CHECK(environment.colour_clamp && !environment.dither && !environment.per_pixel_alpha);
    CHECK(environment.scan_mask == 0 && environment.dither_matrix == 0x6071243571603524ull);
    NoScene(frame); // prospective Sky setup is not a completed UI/movie frame
    f.actions[Frame::kFinishSkyAddress] = [&](CpuContext&, uint32_t) { absent(); };
    f.Call(Frame::kFinishSkyAddress);
    Frame::SceneFrame complete;
    CHECK(frame.CaptureScene(27, complete));
    CHECK(SceneFields(sky) == SceneFields(complete));
    f.Link();
    CHECK(frame.CaptureSky(sky, environment));
    CHECK(sky.epoch == complete.epoch + 1 && sky.source_frame == 1);
    f.Call(Frame::kBeginShadowsAddress);
    absent();
    f.Call(Frame::kEndShadowsAddress);
    CHECK(frame.CaptureSky(sky, environment));
    f.words[SourceFixture::target+0x18] = 256;
    absent();
    f.words[SourceFixture::target+0x18] = 512;
    f.words[title::MaterialState::kRenderTarget] = 0xe0000000 | SourceFixture::target;
    absent();
    f.words[title::MaterialState::kRenderTarget] = SourceFixture::target;
    frame.ResetAfterQuiescence();
    absent();
    f.actions[Frame::kWriteFrameHeadAddress] = [&](CpuContext&, uint32_t entry) {
        if (entry != Frame::kWriteFrameHeadAddress) f.Call(Frame::kFinishSkyAddress);
    };
    f.Interior(Frame::kWriteFrameHeadAddress);
    absent(); // nested completed setup cannot certify an unfinished head
    f.Call(Frame::kWriteFrameHeadAddress);
    CHECK(frame.CaptureSky(sky, environment));
    f.actions[Frame::kWriteFrameHeadAddress] = [](CpuContext&, uint32_t) {
        throw std::runtime_error("unfinished frame head");
    };
    try { f.Call(Frame::kWriteFrameHeadAddress); CHECK(false); }
    catch (const std::runtime_error&) {}
    absent();
}
void TestPersistentSourceBoundaries() {
    using State = title::FrameState;
    SourceFixture f;
    f.Display();
    Frame frame(f.Reader());
    CHECK(f.Install(frame));
    Frame::Snapshot cold;
    CHECK(frame.CaptureState(cold) && !cold.scene_valid && !cold.sky_known);
    f.Call(Frame::kSetUpFrameAddress);
    f.Link();
    f.Call(Frame::kSetUpFrameAddress);
    Frame::Snapshot captured;
    CHECK(frame.CaptureState(captured));
    CHECK(captured.scene_valid && captured.submitted_valid && captured.next_frame == 1);
    CHECK(captured.scene.epoch == 2 && captured.submitted.epoch == 1);
    std::vector<uint8_t> bytes, again;
    CHECK(State::Encode(captured, bytes) && bytes.size() == 80);
    Frame::Snapshot decoded;
    CHECK(State::Decode(bytes, decoded));
    CHECK(State::Encode(decoded, again) && bytes == again);
    f.Call(Frame::kSetUpFrameAddress); // timeline abandoned by restore
    const auto evidence = frame.Observed();
    const auto reads = f.reads;
    frame.ResetAfterQuiescence();
    f.prohibit_reads = true;
    CHECK(frame.RestoreState(decoded));
    Frame::SceneFrame scene, submitted;
    CHECK(frame.CaptureScene(27, scene) && frame.CaptureSubmittedScene(submitted));
    CHECK(SceneFields(scene) == SceneFields(captured.scene));
    CHECK(SceneFields(submitted) == SceneFields(captured.submitted));
    CHECK(f.reads == reads); // no later source/header/GS reads at publication
    CHECK(frame.Observed().completed_frame_boundaries == evidence.completed_frame_boundaries);
    Ready(frame);
    f.prohibit_reads = false;
    f.Link();
    f.Call(Frame::kSetUpFrameAddress);
    CHECK(frame.CaptureScene(24, scene) && scene.source_frame == 2 && scene.epoch > 3);
    // No nested/interior operation may certify itself from saved source facts.
    f.actions[Frame::kFinishSkyAddress] = [&](CpuContext&, uint32_t) {
        Frame::Snapshot untouched = decoded;
        CHECK(!frame.CaptureState(untouched));
        CHECK(!frame.RestoreState(decoded));
        CHECK(untouched.next_frame == decoded.next_frame);
    };
    f.Call(Frame::kFinishSkyAddress);
    f.Call(Frame::kBeginShadowsAddress);
    CHECK(!frame.CaptureState(decoded) && !frame.RestoreState(captured));
    f.Call(Frame::kEndShadowsAddress);
    f.Interior(Frame::kSetUpFrameAddress);
    CHECK(frame.CaptureState(decoded) && !decoded.scene_valid && !decoded.submitted_valid);
    CHECK(frame.RestoreState(cold));
    NoScene(frame);
    Unavailable(frame, Status::NoFrameBoundary);
    // Validation is atomic and checks target arithmetic, flags and ordinals.
    for (unsigned change = 0; change < 7; ++change) {
        auto invalid = captured;
        switch (change) {
        case 0: ++invalid.scene.color_buffer; break;
        case 1: ++invalid.next_frame; break;
        case 2: invalid.source_epoch = 0; break;
        case 3: invalid.frame = false; break;
        case 4: invalid.scene_valid = false; break;
        case 5: invalid.scene.epoch = invalid.submitted.epoch; break;
        case 6: invalid.scene.height = 0xffffffffu; break;
        }
        CHECK(!State::Validate(invalid) && !frame.RestoreState(invalid));
        NoScene(frame);
    }
    for (size_t length : {size_t(0), size_t(79), size_t(81)}) {
        auto invalid = bytes; invalid.resize(length);
        CHECK(!State::Decode(invalid, decoded));
    }
    bytes[8] |= 0x80;
    CHECK(!State::Decode(bytes, decoded));
}
void TestSetupProducerSnapshot() {
    TestFrameEnvironment f;
    f.Display();
    Frame frame(f.Reader());
    CHECK(f.Install(frame));
    Frame::SceneFrame output;
    CHECK(!frame.CaptureSetup(output));
    bool nested = false;
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&,uint32_t) {
        CHECK(frame.CaptureSetup(output) == !nested);
        if (nested) return;
        CHECK(output.color_buffer == 0x70000 && output.depth_buffer == 0x150000 &&
              output.width == 512 && output.height == 448 && output.epoch == 1);
        const auto before = SceneFields(output);
        CHECK(!frame.CaptureScene(24,output));
        nested = true;
        f.Call(Frame::kSetUpFrameAddress);
        CHECK(!frame.CaptureSetup(output) && SceneFields(output) == before);
        f.Call(Frame::kResetGraphAddress);
        CHECK(!frame.CaptureSetup(output)); // invalidated inside the incomplete producer
    };
    f.Call(Frame::kSetUpFrameAddress);
    CHECK(!frame.CaptureSetup(output) && !frame.CaptureScene(24,output));
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&,uint32_t) {
        CHECK(frame.CaptureSetup(output));
        throw std::runtime_error("source setup failure");
    };
    try { f.Call(Frame::kSetUpFrameAddress); } catch (const std::runtime_error&) {}
    CHECK(!frame.CaptureSetup(output) && !frame.CaptureScene(24,output));
    f.actions[Frame::kSetUpFrameAddress] = [&](CpuContext&,uint32_t) {
        CHECK(!frame.CaptureSetup(output));
    };
    f.Interior(Frame::kSetUpFrameAddress);
    CHECK(!frame.CaptureSetup(output));
}
}  // namespace

int main() {
    TestColdBootAndCompleteBoundaries();
    TestSkyShadowsAndEpochs();
    TestRestoreAndInteriors();
    TestFailedBodiesDoNotEstablishReadiness();
    TestInstallPreflightAndRollback();
    TestCompositionWithSubmissionWrapper();
    TestSourceSceneSnapshot();
    TestSourceSceneResetAndInteriors();
    TestSourceSceneValidation();
    TestSkySourceBeforeSetup();
    TestPersistentSourceBoundaries();
    TestSetupProducerSnapshot();
    std::printf("FrameEnvironment: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
