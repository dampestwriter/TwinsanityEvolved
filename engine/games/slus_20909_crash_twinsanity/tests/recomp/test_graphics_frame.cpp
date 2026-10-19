#include "games/slus_20909_crash_twinsanity/composition/graphics_frame.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"

namespace {
using Service = title::FrameService;
using Intent = title::FrameIntent;
using Source = title::FrameEnvironment;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)
constexpr std::array<uint32_t,5> entries{Intent::kAddress,Service::kSend,Service::kSendInterrupt,
                                        title::FrameClear::kAddress,title::DisplayLayout::kAddress};
class TestGraphicsFrame {
public:
    MainMemory ram;
    Scratchpad scratch;
    MemorySections memory{ram,scratch,nullptr,0};
    StateRestorer restorer{memory};
    std::map<uint32_t,Service::Body> dispatch;
    Source::Snapshot source;
    std::vector<std::shared_ptr<const Intent::Output>> work;
    std::vector<std::shared_ptr<const title::FrameClear::Output>> clears;
    std::vector<title::DisplayLayout::Output> layouts;
    unsigned originals = 0, accepted = 0, pre_restore = 0;
    bool legacy = false;   // the root's GraphicsImport "prepared" predicate
    bool can_submit = true, accept = true;
    std::unique_ptr<ps2r::app::GraphicsFrame> app;
    TestGraphicsFrame() {
        Put(title::MaterialState::kDisplayWidth,512);
        Put(title::MaterialState::kDisplayHeight,448);
        Put(title::MaterialState::kDisplayPixelBytes,2);
        Put(title::MaterialState::kDisplayPixelBytes+4,4);
    }
    void Put(uint32_t at,uint32_t value) { std::memcpy(ram.Pointer(at,4),&value,4); }
    Service::Hooks Hooks(bool enabled = true) {
        return {enabled,[this](uint32_t at,void* out,size_t size) {
            const auto* ptr = ram.Pointer(at,size);
            if (!ptr) return false;
            std::memcpy(out,ptr,size); return true;
        },[this](auto& frame) {
            frame = {0x70000,0x150000,512,448,7,31}; return can_submit;
        },[this](auto intent,const auto&) {
            if (!accept) return false;
            work.push_back(std::move(intent)); ++accepted; return true;
        },[this](auto& frame) {
            frame = {0x70000,0x150000,512,448,7,31}; return can_submit;
        },[this](auto intent,const auto& frame) {
            CHECK(frame.epoch == 31 && frame.source_frame == 7);
            if (!accept) return false;
            clears.push_back(std::move(intent)); return true;
        },[this](const auto& layout) {
            if (!accept) return false;
            layouts.push_back(layout); return true;
        }};
    }
    Service::Capture Capture() {
        return [this](uint32_t) { return Service::Body{[this](auto&,auto) { ++originals; }}; };
    }
    Service::Installer Install() {
        return [this](auto at,auto body) { dispatch[at] = std::move(body); return true; };
    }
    void Call(uint32_t at,uint32_t entry = 0,uint32_t object = Intent::kSourceObject) {
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = object;
        dispatch.at(at)(context,entry);
    }
    void Root(bool with_import = false) {
        app = std::make_unique<ps2r::app::GraphicsFrame>(Hooks());
        CHECK(app->Install(Capture(),Install()));
        if (with_import) app->SetLegacyImport([this] { return legacy; });
        ps2r::savestate::SectionSpec frame;
        frame.tag = title::FrameState::kTag; frame.version = 1; frame.owner = "source fixture";
        frame.required = !with_import;   // the root's source owner is optional under the import
        frame.serialize = [this](auto& bytes) { CHECK(title::FrameState::Encode(source,bytes)); };
        frame.validate = [](const auto& blob,auto* why) {
            Source::Snapshot decoded; return title::FrameState::Decode(blob.payload,decoded,why);
        };
        frame.restore = [this](const auto& blob,auto* why) {
            return title::FrameState::Decode(blob.payload,source,why);
        };
        CHECK(restorer.Registry().Register(frame));
        CHECK(app->RegisterState(restorer));
        restorer.RequireSectionOwners();
        restorer.SetPreRestore([this](auto* why) {
            if (!app->CanCheckpoint(why)) return false;
            ++pre_restore; return true;
        });
    }
};

template<class F> bool Throws(F&& operation) {
    try { operation(); } catch (const std::runtime_error&) { return true; }
    return false;
}

void InstallationAndRouting() {
    for (unsigned fail = 0; fail < entries.size(); ++fail) {
        TestGraphicsFrame f;
        Service service(f.Hooks());
        unsigned calls = 0;
        CHECK(!service.Install(f.Capture(),[&](auto at,auto body) {
            if (calls++ == fail) return false;
            f.dispatch[at] = std::move(body); return true;
        }));
        for (unsigned i = 0; i < fail; ++i) f.Call(entries[i]);
        CHECK(f.originals == fail && service.Counts().setup.calls == 0);
        CHECK(service.Install(f.Capture(),f.Install()));
        CHECK(!service.Install(f.Capture(),f.Install()));
    }
    TestGraphicsFrame f;
    Service service(f.Hooks());
    CHECK(service.Install(f.Capture(),f.Install()));
    uint32_t w = 1,h = 1;
    CHECK(service.CapturePlan(w,h) && !w && !h);
    f.Call(Intent::kAddress);
    for (uint32_t address : {Service::kSend,Service::kSendInterrupt})
        for (uint32_t alias : {0u,0x80000000u,0xa0000000u})
            f.Call(address,address,Intent::kSourceObject|alias);
    CHECK(!f.originals && f.accepted == 6 && service.Counts().send.replaced == 3 &&
          service.Counts().interrupt.replaced == 3);
    for (uint32_t address : {Service::kSend,Service::kSendInterrupt})
        f.Call(address,address,0x231000);
    CHECK(f.originals == 2 && service.Counts().foreign_sends == 2 &&
          service.Counts().send.model_calls == 0);
    const auto first = f.work.front();
    CHECK(service.RestorePlan(640,400));
    CHECK(service.CapturePlan(w,h) && w == 640 && h == 400);
    CHECK(first->width == 512 && first->height == 448); // queued plan stays immutable
    CHECK(!service.RestorePlan(UINT32_MAX,UINT32_MAX));
    CHECK(service.CapturePlan(w,h) && w == 640 && h == 400);
    CHECK(service.Counts().setup.replaced == 1 && service.Counts().send.replaced == 3);
}

// A selected send whose plan/frame is unavailable, whose owned recording the
// root refuses, or that enters at an interior label REFUSES: the original send
// runs (counted), nothing is faulted and the owner stays live for the next head.
void SelectedFailuresNeverReplay() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        TestGraphicsFrame f;
        auto hooks = f.Hooks();
        std::vector<std::pair<title::GraphicsRefusals::Reason,uint32_t>> refusals;
        hooks.refuse = [&](auto reason,uint32_t boundary,uint32_t detail) {
            CHECK(boundary == Service::kSend); refusals.emplace_back(reason,detail);
        };
        Service service(std::move(hooks));
        CHECK(service.Install(f.Capture(),f.Install()));
        if (failure != 0) f.Call(Intent::kAddress);
        if (failure == 1) f.can_submit = false;
        if (failure == 2) f.accept = false;
        if (failure == 3) CHECK(service.RestorePlan(640,400));
        f.Call(Service::kSend,failure == 4 ? Service::kSend+4 : Service::kSend);
        CHECK(f.originals == 1 && !f.accepted && service.Counts().failures == 0 &&
              service.Counts().send.replaced == 0 && service.Counts().send.model_calls == 1);
        CHECK(service.Counts().refused == 1 && refusals.size() == 1 &&
              refusals[0].first == title::GraphicsRefusals::Reason::Ordering &&
              refusals[0].second == (failure == 4 ? Service::kInterior :
                                     failure == 2 ? Service::kSendRefused : Service::kSendFrame));
        CHECK(service.Counts().interior_calls == (failure == 4 ? 1u : 0u) &&
              service.Counts().send.calls == (failure == 4 ? 0u : 1u));
        // The owner is still selected: the next complete head and send are owned.
        f.can_submit = f.accept = true;
        f.Call(Intent::kAddress);
        f.Call(Service::kSend);
        CHECK(f.originals == 1 && f.accepted == 1 && service.Counts().send.replaced == 1);
        uint32_t w,h;
        CHECK(service.CapturePlan(w,h) && w == 512 && h == 448);
    }
    TestGraphicsFrame f;
    Service model(f.Hooks(false));
    CHECK(model.Install(f.Capture(),f.Install()));
    for (auto at : entries) f.Call(at);
    CHECK(f.originals == 5 && !f.accepted && model.Counts().setup.model_calls == 1 &&
          model.Counts().send.model_calls == 1 && model.Counts().interrupt.model_calls == 1 &&
          model.Counts().clear.model_calls == 1 && model.Counts().layout.model_calls == 1);
}

// TR renderer.cpp:436-440: Present (the frame-chain Send) precedes StartNextFrame's
// SubmitBuckets (Link) in every PresentFrame. The title's FIRST Send therefore
// has a plan but no linked scene: it completes as the owned display of no scene
// (replaced, no model call, no recording, no refusal). Once any Link has run,
// a Send without a submitted scene refuses to the original exactly as before.
void FirstSendBeforeAnyLink() {
    TestGraphicsFrame f;
    auto hooks = f.Hooks();
    bool linked = false;
    hooks.linked = [&] { return linked; };
    unsigned refusals = 0;
    hooks.refuse = [&](auto,uint32_t boundary,uint32_t detail) {
        CHECK(boundary == Service::kSend || boundary == Service::kSendInterrupt);
        CHECK(detail == Service::kSendFrame); ++refusals;
    };
    Service service(std::move(hooks));
    CHECK(service.Install(f.Capture(),f.Install()));
    f.can_submit = false;
    // No plan yet (no WriteFrameHead): still a refusal, linked or not.
    f.Call(Service::kSend);
    CHECK(f.originals == 1 && refusals == 1 && service.Counts().send.model_calls == 1 &&
          service.Counts().empty_sends == 0);
    f.Call(Intent::kAddress);
    CHECK(service.Counts().setup.replaced == 1);
    f.Call(Service::kSend);
    f.Call(Service::kSendInterrupt);
    CHECK(f.originals == 1 && !f.accepted && f.work.empty() && refusals == 1);
    CHECK(service.Counts().send.replaced == 1 && service.Counts().send.model_calls == 1 &&
          service.Counts().interrupt.replaced == 1 && service.Counts().interrupt.model_calls == 0 &&
          service.Counts().empty_sends == 2 && service.Counts().refused == 1 &&
          service.Counts().failures == 0);
    // After the first Link a Send with no submitted scene is a refusal again.
    linked = true;
    f.Call(Service::kSend);
    CHECK(f.originals == 2 && refusals == 2 && service.Counts().send.model_calls == 2 &&
          service.Counts().empty_sends == 2);
    // A submitted scene is recorded as before; the plan survived the empty sends.
    f.can_submit = true;
    f.Call(Service::kSend);
    CHECK(f.accepted == 1 && f.work.size() == 1 && f.work[0]->width == 512 &&
          service.Counts().send.replaced == 2 && service.Counts().empty_sends == 2);
    // Without the hook the pre-existing contract holds: refuse.
    TestGraphicsFrame g;
    Service plain(g.Hooks());
    CHECK(plain.Install(g.Capture(),g.Install()));
    g.Call(Intent::kAddress);
    g.can_submit = false;
    g.Call(Service::kSend);
    CHECK(g.originals == 1 && plain.Counts().send.model_calls == 1 && plain.Counts().empty_sends == 0);
}

void RequiredPlanState() {
    TestGraphicsFrame f;
    f.Root();
    const auto empty = f.restorer.SaveImage({});
    // A completed source head cannot carry the explicit empty-plan image.
    f.source.dither_known = true;
    const auto omitted = f.restorer.SaveImage({});
    f.Call(Intent::kAddress);
    const auto initialized = f.restorer.SaveImage({});
    f.Call(Service::kSend);
    f.Put(0x1000,91);
    std::string error;
    CHECK(!f.restorer.RestoreImage(omitted.data(),omitted.size(),&error));
    CHECK(!f.pre_restore && f.ram.Data()[0x1000] == 91);
    CHECK(f.restorer.RestoreImage(initialized.data(),initialized.size(),&error));
    CHECK(f.pre_restore == 1 && f.ram.Data()[0x1000] == 0 && !f.originals);
    f.Call(Service::kSendInterrupt);
    CHECK(f.accepted == 2 && f.work.back()->width == 512 && !f.originals);
    // Restoring another source owner must fail before RAM changes as well.
    f.Put(title::MaterialState::kDisplayWidth,640);
    const auto mixed = f.restorer.SaveImage({});
    CHECK(!f.restorer.RestoreImage(mixed.data(),mixed.size(),&error) && f.pre_restore == 1);
    CHECK(f.restorer.RestoreImage(empty.data(),empty.size(),&error) && f.pre_restore == 2);
    f.Call(Service::kSend); // explicit empty plan: the send refuses to the original
    CHECK(f.originals == 1 && f.accepted == 2);

    TestGraphicsFrame fresh;
    fresh.Root();
    CHECK(fresh.restorer.RestoreImage(initialized.data(),initialized.size(),&error));
    fresh.Call(Service::kSend);
    CHECK(!fresh.originals && fresh.accepted == 1); // no setup body during restore
    MemorySections memory(fresh.ram,fresh.scratch,nullptr,0);
    StateRestorer missing(memory);
    const auto absent = missing.SaveImage({});
    CHECK(!fresh.restorer.RestoreImage(absent.data(),absent.size(),&error) && fresh.pre_restore == 1);
}

void SourceClearRouting() {
    constexpr uint32_t target = 0x12000;
    for (unsigned mode = 0; mode < 5; ++mode) {
        TestGraphicsFrame f;
        f.Put(target+0xc,512); f.Put(target+0x10,448);
        f.Put(target+0x18,512); f.Put(target+0x1c,448);
        f.Put(target+0x60,1792); f.Put(target+0x64,1824);
        f.Put(target+0x14,0x007f3511);
        Service service(f.Hooks());
        CHECK(service.Install(f.Capture(),f.Install()));
        if (mode == 1) f.can_submit = false;
        if (mode == 2) f.accept = false;
        if (mode == 3) f.Put(target+0x18,640);
        if (!mode) {
            f.Call(title::FrameClear::kAddress,0,target|0xa0000000);
            CHECK(f.clears.size() == 1 && f.clears[0]->draw.verts[0].r == 17 &&
                  f.clears[0]->draw.raster.frame.fbmsk == UINT32_MAX &&
                  f.clears[0]->draw.raster.zbuf.zmsk);
            CHECK(service.Counts().clear.replaced == 1);
        } else {
            // No source target (1), refused recording (2), mismatched target
            // (3) and an interior entry (4) refuse to the original clear.
            f.Call(title::FrameClear::kAddress,title::FrameClear::kAddress+(mode == 4 ? 4 : 0),target);
            CHECK(f.clears.empty() && service.Counts().failures == 0 &&
                  !service.Counts().clear.replaced && service.Counts().refused == 1);
            CHECK(f.originals == 1 && service.Counts().clear.model_calls == 1);
        }
        if (!mode) CHECK(!f.originals && !service.Counts().clear.model_calls);
    }
}

void SourceDisplayRouting() {
    for (unsigned failure = 0; failure < 7; ++failure) {
        TestGraphicsFrame f;
        const std::array<uint32_t,22> settings{0,8,2,0,0,0,0,128,1,1,1,0,0,0,0,0,
                                             4,0,2560,447,0,0};
        for (unsigned i = 0; i < settings.size(); ++i)
            f.Put(title::DisplayLayout::kSettings+4*i,settings[i]);
        auto hooks = f.Hooks();
        if (failure == 6) hooks.layout = {};
        Service service(std::move(hooks));
        CHECK(service.Install(f.Capture(),f.Install()));
        if (failure != 1) f.Call(Intent::kAddress);
        if (failure == 2) f.accept = false;
        if (failure == 3) CHECK(service.RestorePlan(640,400));
        if (failure == 4) f.Put(title::DisplayLayout::kSettings+0x50,1); // unsupported shift
        if (!failure) {
            f.Call(title::DisplayLayout::kAddress);
            CHECK(f.layouts.size() == 1 && f.layouts[0].width == 512 && f.layouts[0].height == 448);
            f.Put(title::MaterialState::kDisplayWidth,640);
            CHECK(f.layouts[0].width == 512); // queued layout never rereads source settings
            CHECK(service.Counts().layout.replaced == 1);
        } else {
            // No plan (1), refused recording (2), plan mismatch (3), unsupported
            // settings (4), interior entry (5), no layout hook (6): the original
            // SetupPCRTC runs (counted) and the owner stays live.
            f.Call(title::DisplayLayout::kAddress,title::DisplayLayout::kAddress+(failure == 5 ? 4 : 0));
            CHECK(f.layouts.empty() && service.Counts().failures == 0 &&
                  !service.Counts().layout.replaced && service.Counts().refused == 1);
            CHECK(f.originals == 1 && service.Counts().layout.model_calls == 1);
            f.Call(Intent::kAddress);
            CHECK(service.Counts().setup.replaced == (failure == 1 ? 1u : 2u) && f.originals == 1);
        }
        if (!failure) CHECK(!f.originals && !service.Counts().layout.model_calls);
    }
}
}

// The explicit legacy import: a frozen image (no FPLN, no GFRM) is admitted
// only while the root's import holds a prepared image; the plan is none until
// the next source head. Unprepared, the plan section stays required.
void LegacyImportAbsentPlan() {
    TestGraphicsFrame f;
    f.Root(true);
    MemorySections memory(f.ram,f.scratch,nullptr,0);
    StateRestorer frozen(memory);
    const auto legacy = frozen.SaveImage({});
    f.Call(Intent::kAddress);
    std::string error;
    CHECK(!f.restorer.RestoreImage(legacy.data(),legacy.size(),&error) && f.pre_restore == 0);
    f.legacy = true;
    const bool restored = f.restorer.RestoreImage(legacy.data(),legacy.size(),&error);
    if (!restored) std::fprintf(stderr,"legacy plan restore: %s\n",error.c_str());
    CHECK(restored && f.pre_restore == 1);
    f.Call(Service::kSend);   // no plan after the import: the send refuses to the original
    CHECK(f.originals == 1);
}
int main() {
    InstallationAndRouting();
    SelectedFailuresNeverReplay();
    FirstSendBeforeAnyLink();
    RequiredPlanState();
    LegacyImportAbsentPlan();
    SourceClearRouting();
    SourceDisplayRouting();
    std::printf("GraphicsFrame: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
