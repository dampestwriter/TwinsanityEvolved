#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

namespace {
using Service = title::GraphicsService;
using Bridge = title::GraphicsBridge;
using Material = title::MaterialState;
using Draw = ps2r::gs::DrawIr;
int checks = 0, failures = 0;
void Check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

struct TestGraphicsService final : ps2r::gs::DrawIrSink {
    static constexpr uint32_t kShape = 0x1000, kPositions = 0x2000, kColours = 0x3000;
    static constexpr uint32_t kShader = 0x4000, kTarget = 0x6000, kMatrix = 0x7000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400000);
    std::map<uint32_t, Service::Body> bindings;
    std::vector<std::function<void()>> slots;
    std::vector<Draw> output, stage, pending;
    std::vector<uint32_t> fallback_entries, fallback_bodies, link_entries;
    std::function<void()> original_link;
    uint64_t stages = 0, flushes = 0, fences = 0;
    uint32_t sequence = 100;
    bool environment_known = true;
    bool owned_available = true, owned_throws = false;
    Service* installed_service = nullptr;
    CpuContext context{};

    TestGraphicsService() {
        Put(kShape, Material::kFlatMaterial);
        Put(kShape + 0x10, 4, 1);
        Put(kShape + 0x14, kPositions); Put(kShape + 0x18, kColours);
        const float positions[]{1, 1, 1, 4, 3, 1, 3, 4};
        std::memcpy(bytes.data() + kPositions, positions, sizeof(positions));
        Colours(0x80402010);
        Put(Material::kFlatMaterial, kShader);
        Put(Material::kFlatMaterial + 0x10, 1);
        Put(Material::kFlatMaterial + 0x18, 2, 8);
        Put(Material::kFlatMaterial + 0x20, 24);
        // Meaning: mix blend, GEQUAL, Gouraud, no depth writes.
        Put(kShader, 0x0800000004400001ull, 8);
        Put(kShader + 0x64, 14);
        Put(kShader + 0x6c, Material::kFlatShaderVtable);
        Put(Material::kRenderTarget, kTarget);
        Put(kTarget + 0xc, 512); Put(kTarget + 0x10, 448);
        Put(kTarget + 0x18, 512); Put(kTarget + 0x1c, 448);
        Put(Material::kDisplayWidth, 512); Put(Material::kDisplayHeight, 448);
        Put(Material::kDisplayPixelBytes, 2); Put(Material::kDepthBufferPage, 168);
        const float matrix[]{1,0,0,0, 0,1,0,0, 0,0,1,0, 5,7,0,1};
        std::memcpy(bytes.data() + kMatrix, matrix, sizeof(matrix));
        Frame(0x10000, 0x11000);
        for (uint32_t at : Service::kStripAddresses)
            bindings[at] = [this, at](CpuContext& c, uint32_t entry) {
                fallback_bodies.push_back(at);
                fallback_entries.push_back(entry);
                c.cpuRegs.GPR.r[2].UL[0] = 0xface;
            };
        bindings[Service::kLinkAddress] = [this](CpuContext&, uint32_t entry) {
            link_entries.push_back(entry);
            if (original_link) original_link();
        };
    }
    void Put(uint32_t address, uint64_t value, size_t size = 4) {
        for (size_t i = 0; i < size; ++i) bytes.at(address + i) = uint8_t(value >> (8 * i));
    }
    void Colours(uint32_t color) {
        for (size_t i = 0; i < 4; ++i) Put(kColours + uint32_t(i) * 4, color);
    }
    void Frame(uint32_t root, uint32_t anchor) {
        Put(Bridge::kFrameBucketsAddress + 8, root);
        Put(Bridge::kFrameBucketsAddress + 24 * 0x38 + 0x10, anchor);
        // Append predecessor, with the already-produced material cache intact.
        Put(Bridge::kFrameBucketsAddress + 24 * 0x38 + 8 + 0x24, 0xfedcba98);
        Put(anchor, 0x20000000);
        Put(anchor + 4, 0);
        Put(anchor + 8, 0);
        Put(anchor + 12, 0);
    }
    Service::Hooks Hooks() {
        Service::Hooks hooks;
        hooks.strip_selection = Service::StripSelection::All;
        hooks.read = [this](uint32_t at, uint8_t* out, size_t size) {
            if (at > bytes.size() || size > bytes.size() - at) return false;
            std::memcpy(out, bytes.data() + at, size);
            return true;
        };
        hooks.environment = [this](Material::Environment& out) {
            if (!environment_known) return false;
            out.colour_clamp = true;
            out.dither_matrix = 0x6071243571603524ull;
            return true;
        };
        hooks.flush_staged = [this] {
            ++stages;
            slots.push_back([this, owned = std::move(stage)] {
                pending.insert(pending.end(), owned.begin(), owned.end());
            });
            stage.clear();
        };
        hooks.delivery.enqueue = [this](std::function<void()> callback) {
            slots.push_back(std::move(callback));
            return true;
        };
        hooks.delivery.flush_legacy = [this] {
            ++flushes;
            for (const auto& draw : pending) OnDraw(draw);
            pending.clear();
            return true;
        };
        hooks.delivery.sink = this;
        hooks.delivery.drain = [this] { ++fences; return true; };
        hooks.record_strip = [this](auto intent) {
            if (owned_throws) throw std::runtime_error("owned capture failed");
            if (!owned_available) return false;
            return installed_service && installed_service->RecordOwned(intent.bucket,
                [this,intent=std::move(intent)] { OnDraw(intent.draw); return true; });
        };
        return hooks;
    }
    bool Install(Service& service) {
        const bool installed = service.Install(
            [this](uint32_t at) { return bindings.at(at); },
            [this](uint32_t at, Service::Body body) { bindings[at] = std::move(body); return true; });
        if (installed) installed_service = &service;
        return installed;
    }
    void Strip(uint32_t entry = Service::kStripAddress) {
        context.cpuRegs.GPR.r[4].UL[0] = kShape;
        bindings.at(Service::kStripAddress)(context, entry);
    }
    void Link(uint32_t entry = Service::kLinkAddress) {
        context.cpuRegs.GPR.r[4].UL[0] = Bridge::kFrameBucketsAddress;
        bindings.at(Service::kLinkAddress)(context, entry);
    }
    void Legacy(uint8_t color) {
        Draw draw;
        draw.verts.resize(3);
        draw.vertex_count = 3;
        draw.verts[0].r = color;
        stage.push_back(std::move(draw));
    }
    void Deliver() {
        for (auto& slot : slots) slot();
        slots.clear();
    }
    void OnDraw(const Draw& original) override {
        // Model the composition root's common publisher. The title producer
        // does not own the seq/frame/epoch counters.
        auto draw = original;
        draw.seq = sequence++;
        draw.src_frame = 77;
        draw.vram_epoch = 42;
        output.push_back(std::move(draw));
    }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};

void TestEntryCutoverAndFallback() {
    TestGraphicsService fixture;
    auto hooks = fixture.Hooks();
    hooks.strip_selection = Service::StripSelection::Plain;
    Service service(hooks);
    CHECK(fixture.Install(service));
    CHECK(service.CanCheckpoint());
    const auto before = fixture.bytes;
    fixture.context.cpuRegs.GPR.r[29].UD[0] = 0xabcdef;
    fixture.Strip();
    CHECK(fixture.bytes == before);  // no packet/setup/cache writes
    CHECK(fixture.context.cpuRegs.GPR.r[29].UD[0] == 0xabcdef);
    CHECK(fixture.fallback_entries.empty() && fixture.slots.empty() && fixture.output.empty());
    CHECK(service.Counts().replaced_producers == 1);
    CHECK(!service.CanCheckpoint() && service.Counts().unsubmitted == 1);
    CHECK(!service.RestoreFrame(78) && service.NextFrame() == 0);

    fixture.Put(TestGraphicsService::kShape, 0x9999);  // unsupported material
    fixture.Strip();
    CHECK(fixture.fallback_entries.size() == 1);
    CHECK(fixture.context.cpuRegs.GPR.r[2].UL[0] == 0xface);
    fixture.Put(TestGraphicsService::kShape, Material::kFlatMaterial);
    fixture.Put(TestGraphicsService::kShape + 0x10, 2, 1);
    fixture.Strip();
    fixture.Put(TestGraphicsService::kShape + 0x10, 4, 1);
    fixture.environment_known = false;
    fixture.Strip();
    fixture.environment_known = true;
    fixture.Strip(Service::kStripAddress + 4);  // restored interior continuation
    CHECK(fixture.fallback_entries == std::vector<uint32_t>({
        Service::kStripAddress, Service::kStripAddress,
        Service::kStripAddress, Service::kStripAddress + 4}));
    fixture.Link(Service::kLinkAddress + 4);
    CHECK(service.Counts().linked_submissions == 0);  // no sealing at an interior PC
    CHECK(service.Counts().interior_fallback == 2);
    fixture.Link();
    CHECK(service.Counts().linked_submissions == 1);
    CHECK(service.Counts().pending_submissions == 1 && service.Counts().unsubmitted == 0);
    fixture.Legacy(9);
    service.OnTag(0x10000, false, true);
    CHECK(fixture.stages == 0);  // no flush at unrelated legacy tags
    service.OnTag(0x11000, false, false);
    CHECK(fixture.stages == 1 && fixture.output.empty());
    service.OnTag(0x11000, false, false);
    CHECK(fixture.stages == 1);  // once-only within the walk
    service.EndWalk();
    CHECK(!service.CanCheckpoint());
    fixture.Deliver();
    CHECK(service.CanCheckpoint());
    CHECK(fixture.output.size() == 2 && fixture.output[0].verts[0].r == 9);
    CHECK(fixture.output[1].vertex_count == 6);
    CHECK(fixture.output[1].verts[0].x == 1 && fixture.output[1].verts[0].y == 1);
    CHECK(fixture.output[1].verts[0].r == 0x10);
    CHECK(fixture.output[1].raster.frame.fbp == 512 * 448 * 2);
    CHECK(fixture.output[1].seq == 101 && fixture.output[1].src_frame == 77 &&
          fixture.output[1].vram_epoch == 42);
    CHECK(service.Counts().output.replaced_bucket_jobs[24] == 1);
    CHECK(service.Counts().fallback == 3 && service.Counts().output.failed_groups == 0);
}

void TestOldWalkInsideNewLinkAndTailIsolation() {
    TestGraphicsService fixture;
    Service service(fixture.Hooks());
    CHECK(fixture.Install(service));
    fixture.Strip();
    fixture.original_link = [&] { fixture.Frame(0x20000, 0x21000); };
    fixture.Link();  // A is parked; B's frame writer starts
    fixture.Colours(0x80112233);
    fixture.Strip();
    fixture.original_link = [&] {
        // New Link's kick drains A before parking B.
        fixture.Legacy(1);
        service.OnTag(0x10000, false, true);
        service.OnTag(0x11000, false, false);
        fixture.Frame(0x10000, 0x11000);
    };
    fixture.Link();  // both submissions exist while A's observer runs
    CHECK(service.Counts().linked_submissions == 2);
    CHECK(service.Counts().output.queued_groups == 1);
    CHECK(service.Counts().pending_submissions == 2);

    service.OnTag(0x12000, false, true);  // tail replay, no frame root
    service.OnTag(0x21000, false, false); // coincident B anchor MUST NOT select B
    CHECK(service.Counts().output.queued_groups == 1 && fixture.stages == 1);
    fixture.Legacy(2);
    service.OnTag(0x20000, false, true);
    service.OnTag(0x21000, false, false);
    service.EndWalk();
    CHECK(service.Counts().output.queued_groups == 2);
    fixture.Colours(0);  // deferred intent must retain each call's original bytes
    fixture.Deliver();
    CHECK(fixture.output.size() == 4);
    CHECK(fixture.output[0].verts[0].r == 1 && fixture.output[1].verts[0].r == 0x10);
    CHECK(fixture.output[2].verts[0].r == 2 && fixture.output[3].verts[0].r == 0x33);
    CHECK(service.CanCheckpoint() && service.Counts().output.completed_work == 2 &&
          service.Counts().output.completed == 0 && service.Counts().owned_strip_producers == 2);
    CHECK(service.Counts().ordering_errors == 0);
}

// A walk that never visits an accepted predecessor is the title's own doing: its
// renderer kicked another chain (ResetBuckets after WaitIdle at a movie start/end,
// TR movie.cpp:156/211) and dropped the frame's appended buckets. The group is
// DISCARDED with them -- never delivered, never replayed, attributed (ordering,
// the Link producer, kDiscarded) -- and the service stays live and checkpointable.
// (Before: "Graphics walk did not deliver every accepted predecessor", a fault.)
void TestMissingAnchorAndResetGuards() {
    using Reason = title::GraphicsRefusals::Reason;
    TestGraphicsService fixture;
    auto hooks = fixture.Hooks();
    struct Refusal { Reason reason; uint32_t producer, detail; };
    std::vector<Refusal> refusals;
    hooks.refuse = [&](Reason reason, uint32_t producer, uint32_t detail) { refusals.push_back({reason, producer, detail}); };
    Service service(hooks);
    CHECK(fixture.Install(service));
    fixture.Strip();
    fixture.Link();
    service.OnTag(0x10000, false, true);
    bool failed = false;
    try { service.EndWalk(); } catch (const std::runtime_error&) { failed = true; }
    CHECK(!failed && service.Counts().ordering_errors == 0);
    CHECK(service.Counts().discarded_walks == 1 && service.Counts().output.discarded_groups == 1);
    CHECK(refusals.size() == 1 && refusals[0].reason == Reason::Ordering &&
          refusals[0].producer == Service::kLinkAddress && refusals[0].detail == Service::kDiscarded);
    CHECK(service.CanCheckpoint() && service.Counts().pending_submissions == 0);
    CHECK(fixture.fallback_entries.empty());  // accepted producer cannot be silently replayed
    fixture.Deliver();
    CHECK(fixture.output.empty());            // the discarded strip is never drawn
    service.ResetAfterQuiescence();
    CHECK(service.CanCheckpoint());

    fixture.Strip();
    fixture.Link();
    service.OnTag(0x10000, false, true);
    service.OnTag(0x11000, false, false);
    service.EndWalk();
    auto stale = fixture.slots;  // detached by the root's reset/quiescence procedure
    fixture.slots.clear();
    CHECK(!service.CanCheckpoint());
    service.ResetAfterQuiescence();
    for (auto& callback : stale) callback();
    CHECK(fixture.output.empty());
    CHECK(service.CanCheckpoint());
    CHECK(service.Counts().pending_submissions == 0 && service.Counts().unsubmitted == 0);
}

void TestInstallationAndInvalidRanges() {
    TestGraphicsService fixture;
    auto hooks = fixture.Hooks();
    hooks.record_strip = {};
    Service missing_owned(hooks);
    CHECK(!fixture.Install(missing_owned));
    hooks = fixture.Hooks();
    hooks.flush_staged = {};
    Service missing(hooks);
    CHECK(!fixture.Install(missing));
    fixture.Strip();
    CHECK(fixture.fallback_entries.size() == 1);
    auto complete = fixture.Hooks();
    complete.strip_selection = Service::StripSelection::Plain;
    Service service(complete);
    int installs = 0;
    CHECK(!service.Install(
        [&](uint32_t at) { return fixture.bindings.at(at); },
        [&](uint32_t at, Service::Body body) {
            ++installs;
            if (at == Service::kLinkAddress) return false;
            fixture.bindings[at] = std::move(body);
            return true;
        }));
    CHECK(installs == 9);  // four Strip installs, declined Link, four rollbacks
    fixture.Strip();
    CHECK(fixture.fallback_entries.size() == 2);
    CHECK(fixture.Install(service));
    fixture.Put(TestGraphicsService::kShape + 0x14, 0x40002000);
    fixture.Strip();
    fixture.Put(TestGraphicsService::kShape + 0x14, 0x01fffff8);
    fixture.Strip();
    fixture.Put(TestGraphicsService::kShape + 0x14, TestGraphicsService::kPositions);
    fixture.Put(TestGraphicsService::kShape + 0x18, 0xfffffff0);
    fixture.Strip();
    CHECK(service.Counts().replaced_producers == 0 && service.Counts().fallback == 3);
    CHECK(service.CanCheckpoint());
    CHECK(fixture.slots.empty());
}

void TestVariantDispatchAndModelToggle() {
    for (auto selection : {Service::StripSelection::Model, Service::StripSelection::Plain,
                           Service::StripSelection::All}) {
        TestGraphicsService fixture;
        auto hooks = fixture.Hooks();
        hooks.strip_selection = selection;
        Service service(hooks);
        CHECK(fixture.Install(service));
        const auto before = fixture.bytes;
        size_t replaced = 0;
        for (size_t i = 0; i < Service::kStripAddresses.size(); ++i) {
            const bool enabled = selection == Service::StripSelection::All ||
                (i == 0 && selection == Service::StripSelection::Plain);
            replaced += enabled ? 2 : 0;
            const uint32_t address = Service::kStripAddresses[i];
            fixture.context.cpuRegs.GPR.r[4].UL[0] = TestGraphicsService::kShape;
            fixture.context.cpuRegs.GPR.r[5].UL[0] = i >= 2 ? TestGraphicsService::kMatrix : 0x40606060;
            fixture.context.cpuRegs.GPR.r[6].UL[0] = 0x40606060;
            fixture.bindings.at(address)(fixture.context, 0);  // each captured owner, not plain Strip
            fixture.bindings.at(address)(fixture.context, address);
            CHECK(service.Counts().variants[i].calls == 2);
            CHECK(service.Counts().variants[i].replaced == (enabled ? 2 : 0));
            CHECK(service.Counts().variants[i].model_calls == (enabled ? 0 : 2));
        }
        CHECK(fixture.bytes == before);
        CHECK(service.Counts().model_producers == 8 - replaced);
        if (replaced != 0) {
            fixture.Link();
            service.OnTag(0x10000, false, true);
            service.OnTag(0x11000, false, false);
            service.EndWalk();
            fixture.Deliver();
            CHECK(fixture.output.size() == replaced);
            for (size_t i = 0; i < replaced / 2; ++i) {
                CHECK(fixture.output[i * 2].verts[0].x == (i >= 2 ? 6 : 1));
                CHECK(fixture.output[i * 2].verts[0].y == (i >= 2 ? 8 : 1));
                CHECK(fixture.output[i * 2].verts[0].a == (i % 2 ? 0x40 : 0x80));
            }
        } else {
            CHECK(fixture.fallback_bodies.size() == 8 && fixture.slots.empty());
            for (size_t i = 0; i < 4; ++i)
                CHECK(fixture.fallback_bodies[i * 2] == Service::kStripAddresses[i]);
        }
        CHECK(fixture.fallback_bodies.size() == 8 - replaced);
        if (selection == Service::StripSelection::Plain)
            for (size_t i = 1; i < 4; ++i)
                CHECK(fixture.fallback_bodies[(i - 1) * 2] == Service::kStripAddresses[i]);
        if (selection == Service::StripSelection::All) continue; // strict interiors tested below
        for (uint32_t address : Service::kStripAddresses) {
            fixture.bindings.at(address)(fixture.context, address + 4);
            CHECK(fixture.fallback_bodies.back() == address);
            CHECK(fixture.fallback_entries.back() == address + 4);
        }
        CHECK(service.Counts().interior_fallback == 4);
        CHECK(service.Counts().model_producers == 12 - replaced);
    }
    CHECK(Service::Hooks{}.strip_selection == Service::StripSelection::Plain);
}

// A selected (All) Strip whose source intent, environment, interior entry or
// ordered target is unavailable REFUSES: the original producer runs for that
// strip (counted as its model call, attributed with the detail code) and the
// service stays live. Only an owned capture that THREW after acceptance faults.
void TestAllRefusesUnsupportedInputs() {
    using Reason = title::GraphicsRefusals::Reason;
    for (size_t variant = 0; variant < Service::kStripAddresses.size(); ++variant)
        for (unsigned failure = 0; failure < 8; ++failure) {
            TestGraphicsService fixture;
            auto hooks = fixture.Hooks();
            struct Refusal { Reason reason; uint32_t producer, detail; };
            std::vector<Refusal> refusals;
            unsigned faults = 0;
            hooks.refuse = [&](Reason reason, uint32_t producer, uint32_t detail) { refusals.push_back({reason, producer, detail}); };
            hooks.fault = [&](Reason, uint32_t, uint32_t) { ++faults; };
            Service service(hooks);
            CHECK(fixture.Install(service));
            const uint32_t address = Service::kStripAddresses[variant];
            fixture.context.cpuRegs.GPR.r[4].UL[0] = TestGraphicsService::kShape;
            fixture.context.cpuRegs.GPR.r[5].UL[0] = variant >= 2 ? TestGraphicsService::kMatrix : 0x40606060;
            fixture.context.cpuRegs.GPR.r[6].UL[0] = 0x40606060;
            if (failure == 0) fixture.environment_known = false;
            if (failure == 1) fixture.Put(TestGraphicsService::kShape,0x9999);
            if (failure == 2) fixture.Put(TestGraphicsService::kShape+0x14,0x40002000);
            if (failure == 3) fixture.Put(TestGraphicsService::kPositions,0x7f800000);
            const uint32_t before_anchor = uint32_t(fixture.bytes[Bridge::kFrameBucketsAddress+24*0x38+0x10]) |
                uint32_t(fixture.bytes[Bridge::kFrameBucketsAddress+24*0x38+0x11]) << 8 |
                uint32_t(fixture.bytes[Bridge::kFrameBucketsAddress+24*0x38+0x12]) << 16 |
                uint32_t(fixture.bytes[Bridge::kFrameBucketsAddress+24*0x38+0x13]) << 24;
            if (failure == 4) fixture.Put(Bridge::kFrameBucketsAddress+24*0x38+0x10,0);
            if (failure == 6) fixture.owned_available = false;
            if (failure == 7) fixture.owned_throws = true;
            const auto before = fixture.bytes;
            const uint32_t entry = address + (failure == 5 ? 4 : 0);
            bool threw = false;
            try { fixture.bindings.at(address)(fixture.context, entry); }
            catch (const std::runtime_error&) { threw = true; }
            CHECK(fixture.bytes == before && fixture.slots.empty() && service.Counts().replaced_producers == 0);
            if (failure == 7) {
                // The owned capture threw after the service handed it the intent: a fault.
                CHECK(threw && faults == 1 && refusals.empty() && fixture.fallback_entries.empty());
                CHECK(service.Counts().ordering_errors == 1 && !service.CanCheckpoint());
                continue;
            }
            CHECK(!threw && faults == 0 && service.Counts().ordering_errors == 0 && service.CanCheckpoint());
            CHECK(fixture.fallback_entries.size() == 1 && fixture.fallback_entries.back() == entry &&
                  fixture.fallback_bodies.back() == address);
            CHECK(service.Counts().model_producers == 1 && service.Counts().variants[variant].model_calls == 1);
            CHECK(service.Counts().fallback == (failure == 5 ? 0u : 1u) &&
                  service.Counts().interior_fallback == (failure == 5 ? 1u : 0u));
            CHECK(refusals.size() == 1 && refusals[0].producer == address);
            const uint32_t expected_detail =
                failure == 0 ? Service::kEnvironment :
                failure == 1 ? (Service::kCapture | uint32_t(title::StripIntent::Result::UnsupportedMaterial)) :
                failure == 2 ? (Service::kCapture | uint32_t(title::StripIntent::Result::InvalidMemory)) :
                failure == 3 ? (Service::kCapture | uint32_t(title::StripIntent::Result::InvalidGeometry)) :
                failure == 5 ? Service::kInterior : Service::kRecordRefused;
            CHECK(refusals[0].detail == expected_detail);
            CHECK(refusals[0].reason == (failure == 1 || failure == 2 || failure == 3 ? Reason::UnsupportedState
                                                                                         : Reason::Ordering));
            // The service is still selected: the next complete strip is owned.
            if (failure == 0) fixture.environment_known = true;
            if (failure == 1) fixture.Put(TestGraphicsService::kShape, Material::kFlatMaterial);
            if (failure == 2) fixture.Put(TestGraphicsService::kShape+0x14, TestGraphicsService::kPositions);
            if (failure == 3) { const float x = 1; uint32_t bits; std::memcpy(&bits, &x, sizeof bits); fixture.Put(TestGraphicsService::kPositions, bits); }
            if (failure == 4) fixture.Put(Bridge::kFrameBucketsAddress+24*0x38+0x10, before_anchor);
            if (failure == 6) fixture.owned_available = true;
            fixture.bindings.at(address)(fixture.context, address);
            CHECK(service.Counts().replaced_producers == 1 && refusals.size() == 1 &&
                  service.Counts().model_producers == 1);
        }
}

void TestEveryInstallationRollback() {
    for (size_t failure = 0; failure != 5; ++failure) {
        TestGraphicsService fixture;
        Service service(fixture.Hooks());
        std::vector<uint32_t> installs;
        bool failed_once = false;
        CHECK(!service.Install(
            [&](uint32_t at) { return fixture.bindings.at(at); },
            [&](uint32_t at, Service::Body body) {
                installs.push_back(at);
                if (!failed_once && installs.size() == failure + 1) {
                    failed_once = true;
                    return false;
                }
                fixture.bindings[at] = std::move(body);
                return true;
            }));
        CHECK(installs.size() == 2 * failure + 1);
        for (size_t i = 0; i < failure; ++i)
            CHECK(installs[failure + 1 + i] == Service::kStripAddresses[failure - 1 - i]);
        for (uint32_t address : Service::kStripAddresses) {
            fixture.bindings.at(address)(fixture.context, address);
            CHECK(fixture.fallback_bodies.back() == address);
        }
        CHECK(service.Counts().strip_calls == 0 && service.CanCheckpoint());
        CHECK(fixture.Install(service));  // retry captures restored originals
    }
}

void TestOwnedWorkUsesSourceClosure() {
    TestGraphicsService fixture;
    auto hooks = fixture.Hooks();
    hooks.strip_selection = Service::StripSelection::Model;
    Service service(hooks);
    size_t calls = 0;
    CHECK(!service.RecordOwned(24,[&] { ++calls; return true; }));
    CHECK(fixture.Install(service));
    const auto before = fixture.bytes;
    fixture.Legacy(1);
    CHECK(service.RecordOwned(24,[&] {
        CHECK(fixture.output.size() == 1 && fixture.output[0].verts[0].r == 1);
        ++calls;
        return true;
    }));
    CHECK(fixture.bytes == before && calls == 0);
    CHECK(!service.CanCheckpoint() && service.Counts().unsubmitted == 1);
    fixture.Link();
    CHECK(fixture.link_entries.size() == 1); // existing source Link still executes
    service.OnTag(0x10000,false,true);
    service.OnTag(0x11000,false,false);
    service.EndWalk();
    CHECK(calls == 0 && !service.CanCheckpoint());
    fixture.Deliver();
    CHECK(calls == 1 && service.CanCheckpoint());
    CHECK(service.Counts().output.completed_work == 1);
    CHECK(service.Counts().output.completed == 0 && service.Counts().strip_calls == 0);
    CHECK(fixture.stages == 1 && fixture.flushes == 1 && fixture.fences == 1);
    service.EndWalk();
    CHECK(service.Counts().pending_submissions == 0);
    const auto completed = service.Counts().output.completed_work;
    CHECK(service.RestoreFrame(78) && service.NextFrame() == 78);
    CHECK(service.Counts().output.completed_work == completed);
    fixture.Frame(0x12000, 0x13000);
    CHECK(service.RecordOwned(24, [&] { ++calls; return true; }));
    fixture.Link();
    CHECK(service.NextFrame() == 79);
    service.OnTag(0x12000, false, true);
    service.OnTag(0x13000, false, false);
    service.EndWalk();
    fixture.Deliver();
    CHECK(calls == 2 && service.CanCheckpoint());
    CHECK(service.Counts().output.completed_work == completed + 1);
}

// The capture-time settle (INTERFACES section 13). The title's frame boundary
// is Present -> SyncV -> StartNextFrame -> Link (TR renderer.cpp:436-440): at
// SyncV the frame's owned groups are RECORDED behind their predecessors and
// not yet Linked. Before the settle that boundary refused by name ("Graphics
// owned pending=12"): the strict predicate stays red there (pinned), the
// settle admits it and reports the unlinked digest; a sealed group still in
// flight (the previous Link's walk undelivered) is NOT settled; a fault never is.
void TestCaptureSettleAdmitsTheOpenRecording() {
    TestGraphicsService fixture;
    auto hooks = fixture.Hooks();
    hooks.strip_selection = Service::StripSelection::Model;
    Service service(hooks);
    CHECK(fixture.Install(service));
    Bridge::Recording unlinked;
    CHECK(service.CanCheckpoint() && service.CanCheckpointUnlinked(&unlinked) && unlinked.items == 0 &&
          unlinked.groups == 0);
    size_t calls = 0;
    // Twelve owned groups recorded in one frame (two predecessors: bucket 24's
    // anchor and a second bucket's): the title's pre-Present shape.
    for (int i = 0; i < 7; ++i) CHECK(service.RecordOwned(24, [&] { ++calls; return true; }));
    fixture.Put(Bridge::kFrameBucketsAddress + 26 * 0x38 + 0x10, 0x11100);
    fixture.Put(0x11100, 0x20000000); fixture.Put(0x11104, 0); fixture.Put(0x11108, 0); fixture.Put(0x1110c, 0);
    for (int i = 0; i < 5; ++i) CHECK(service.RecordOwned(26, [&] { ++calls; return true; }));
    CHECK(!service.CanCheckpoint() && service.Counts().unsubmitted == 12);   // red before: the boundary refused
    CHECK(service.CanCheckpointUnlinked(&unlinked));                          // the settle admits it ...
    CHECK(unlinked.items == 12 && unlinked.groups == 2 && unlinked.bucket_items[24] == 7 &&
          unlinked.bucket_items[26] == 5 && unlinked.bucket_items[0] == 0);  // ... and names what the image omits
    CHECK(!service.RestoreFrame(5));                                          // a restore still needs it closed
    // Link seals the recording; until the walk delivers, the sealed groups are
    // IN FLIGHT: neither predicate admits (the previous Link's work must land).
    fixture.Link();
    CHECK(!service.CanCheckpoint() && !service.CanCheckpointUnlinked(&unlinked));
    service.OnTag(0x10000, false, true);
    service.OnTag(0x11000, false, false);
    service.OnTag(0x11100, false, false);
    service.EndWalk();
    CHECK(!service.CanCheckpointUnlinked(&unlinked));                         // queued, not delivered
    fixture.Deliver();
    CHECK(calls == 12 && service.CanCheckpoint() && service.CanCheckpointUnlinked(&unlinked) && unlinked.items == 0);
    // The next frame's recording reopens: settled again with its own digest,
    // while the strict predicate is red at every such boundary.
    fixture.Frame(0x12000, 0x13000);
    CHECK(service.RecordOwned(24, [&] { ++calls; return true; }));
    CHECK(!service.CanCheckpoint() && service.CanCheckpointUnlinked(&unlinked) && unlinked.items == 1 &&
          unlinked.groups == 1 && unlinked.bucket_items[24] == 1);
}
}  // namespace

int main() {
    TestEntryCutoverAndFallback();
    TestCaptureSettleAdmitsTheOpenRecording();
    TestOldWalkInsideNewLinkAndTailIsolation();
    TestMissingAnchorAndResetGuards();
    TestInstallationAndInvalidRanges();
    TestVariantDispatchAndModelToggle();
    TestAllRefusesUnsupportedInputs();
    TestEveryInstallationRollback();
    TestOwnedWorkUsesSourceClosure();
    std::printf("GraphicsService: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
