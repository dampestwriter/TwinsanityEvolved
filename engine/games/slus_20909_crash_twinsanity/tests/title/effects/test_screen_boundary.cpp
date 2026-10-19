#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_pass.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <cstdio>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <vector>

// Same-input oracle for the bucket-4 source boundary: the complete generated
// NEW DrawScreenEffects body (0x1b92d8) runs inside ScreenPass on flat RAM;
// the callees it reaches, their order and their register arguments must equal
// what ScreenPass::ReadParameters read at the call, for every switch
// combination and parameter set.
namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
struct Callee { uint32_t address, a0, a1, a2; uint8_t a3; uint32_t f12; };
std::vector<Callee> callees;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t address, CpuContext& c) {
    callees.push_back({address, c.cpuRegs.GPR.r[4].UL[0], c.cpuRegs.GPR.r[5].UL[0], c.cpuRegs.GPR.r[6].UL[0],
                       uint8_t(c.cpuRegs.GPR.r[7].UL[0]), c.fpuRegs.fpr[12].UL});
}
}
namespace {
using S = title::ScreenPass;
namespace ram = strip_oracle::memory;
unsigned checks = 0, cases = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("screen boundary case ") + std::to_string(cases) + " line " + std::to_string(__LINE__) + ": " + #value); } while (0)

bool Reader(uint32_t at, void* out, size_t size) {
    if (at > ram::bytes.size() || size > ram::bytes.size() - at) return false;
    std::memcpy(out, ram::bytes.data() + at, size);
    return true;
}
CpuContext Context(uint32_t gp = S::kGlobalPointer) {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = gp;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    context.vuRegs.VF[0].w = 1;
    return context;
}
struct Owner {
    std::unique_ptr<S> pass;
    S::Body installed;
    unsigned captures = 0, slot_frees = 0;
    std::vector<std::shared_ptr<const S::Batch>> reserved;
    bool frame_ok = true, reserve_ok = true;
    uint32_t frame_height = 448;
};
S::Hooks MakeHooks(Owner& o, bool selected) {
    S::Hooks hooks;
    hooks.enabled = selected;
    hooks.frame = [&o](S::Frame& frame) {
        if (!o.frame_ok) return false;
        frame = S::Frame{0x70000, 0x150000, 512, o.frame_height, 7, 31};
        return true;
    };
    hooks.reserve = [&o](std::shared_ptr<const S::Batch> batch) {
        if (!o.reserve_ok) return false;
        o.reserved.push_back(std::move(batch));
        return true;
    };
    return hooks;
}
std::unique_ptr<Owner> MakeOwner(bool selected = false) {
    auto owner = std::make_unique<Owner>();
    owner->pass = std::make_unique<S>(Reader, MakeHooks(*owner, selected));
    return owner;
}
bool Install(Owner& o, bool accept = true) {
    return o.pass->Install(
        [&o](uint32_t address) {
            ++o.captures;
            CHECK(address == S::kDraw || address == S::kFreeTextureSlots);
            if (address == S::kFreeTextureSlots)
                return S::Body([&o](CpuContext& c, uint32_t entry) {
                    // The source's keepTime-1 free, forwarded with its arguments.
                    CHECK(entry == S::kFreeTextureSlots);
                    CHECK(c.cpuRegs.GPR.r[4].UL[0] == S::kTextureContext && c.cpuRegs.GPR.r[5].UL[0] == 1);
                    ++o.slot_frees;
                });
            return S::Body([](CpuContext& c, uint32_t) { strip_oracle::RunGenerated(S::kDraw, c); });
        },
        [&o, accept](uint32_t address, S::Body body) {
            CHECK(address == S::kDraw);
            o.installed = std::move(body);
            return accept;
        });
}
void Scene(bool blur, bool filter, bool wave, uint32_t height, uint32_t palette, uint32_t second,
           bool blends, float amount) {
    ram::bytes.fill(0);
    ram::write<uint8_t>(S::kBlurSwitch, blur); ram::write<uint8_t>(S::kFilterSwitch, filter);
    ram::write<uint8_t>(S::kWaveSwitch, wave);
    ram::write<uint32_t>(S::kScreenWidth, 512);
    ram::write<uint32_t>(S::kScreenHeight, height);
    ram::write<uint32_t>(S::kFilterPalette, palette); ram::write<uint32_t>(S::kFilterSecond, second);
    ram::write<uint8_t>(S::kFilterBlends, blends); ram::write<float>(S::kFilterAmount, amount);
    // The authored palette tables, their first-colour pointers and the filter's
    // on flag (ScreenPalettes), and the wave shader's rows/amplitude.
    for (uint32_t table = 0; table < title::ScreenPalettes::kCount; ++table) {
        ram::write<uint32_t>(title::ScreenPalettes::kFirstColours + table * 4,
                             title::ScreenPalettes::kTables + table * title::ScreenPalettes::kStride);
        for (uint32_t entry = 0; entry < 256; ++entry)
            ram::write<uint32_t>(title::ScreenPalettes::kTables + table * title::ScreenPalettes::kStride + entry * 4,
                                 0x5a000000u | (table << 16) | (entry * 0x0101u));
    }
    ram::write<uint8_t>(title::ScreenPalettes::kOn, 1);
    ram::write<uint32_t>(S::kWaveShader + S::kWaveRows, 0x340000);
    ram::write<float>(S::kWaveShader + S::kWaveAmplitude, 2.5f);
    for (uint32_t row = 0; row < 16; ++row)
        for (uint32_t axis = 0; axis < 4; ++axis)
            ram::write<float>(0x340000 + row * 16 + axis * 4, float(row) + float(axis) / 8);
}

// The selected arm: no generated body runs for a provable call, the
// keepTime-1 slot free is forwarded, the batch carries the read values; a call
// that cannot be proved refuses to the model (the complete body runs).
void Selected() {
    auto owner_ptr = MakeOwner(true);
    auto& owner = *owner_ptr;
    CHECK(owner.pass->Selected());
    CHECK(Install(owner) && owner.captures == 2);
    for (unsigned variant = 0; variant < 16; ++variant) {
        ++cases;
        const bool blur = variant & 1, filter = variant & 2, wave = variant & 4;
        const uint32_t height = variant & 8 ? 480 : 448;
        owner.frame_height = height;
        Scene(blur, filter, wave, height, variant % 5, (variant / 3) % 5, false, 0);
        strip_oracle::callees.clear();
        const auto before = owner.pass->Counts();
        const size_t reserved = owner.reserved.size(), frees = owner.slot_frees;
        auto context = Context();
        owner.installed(context, S::kDraw);
        const auto after = owner.pass->Counts();
        CHECK(strip_oracle::callees.empty()); // the model body never ran
        CHECK(after.model_calls == before.model_calls && after.refused == before.refused);
        if (!(blur || filter || wave)) {
            CHECK(after.empty == before.empty + 1 && owner.slot_frees == frees && owner.reserved.size() == reserved);
            continue;
        }
        CHECK(after.replaced == before.replaced + 1 && owner.slot_frees == frees + 1);
        CHECK(owner.reserved.size() == reserved + 1);
        const auto& batch = *owner.reserved.back();
        CHECK(batch.Ready() && !batch.Complete() && !batch.Failed());
        // The frame boundary (Graphics INTERFACES section 12): a reserved frame
        // refuses a CLOSED boundary and is admitted by one whose settle found
        // an OPEN recording -- the slot is one of its unlinked items, delivered
        // once the pass completes (the shadow pass's Ready arm). Red before:
        // every boundary with a reservation in flight refused (allon_w1's
        // RootPlaySaveAtTitle: "No admissible boundary in 120 consecutive
        // boundaries; ... graphics_screen").
        CHECK(!owner.pass->CanCheckpoint() && !owner.pass->CanCheckpoint(false));
        CHECK(owner.pass->CanCheckpoint(true));
        CHECK(batch.frame.height == height && batch.frame.width == 512);
        CHECK(batch.work.screen.height == height && batch.work.screen.blur == blur &&
              batch.work.screen.filter == filter && batch.work.wave == wave);
        if (filter) {
            title::ScreenPalettes::Palette expected;
            CHECK(title::ScreenPalettes::Resolve(Reader, variant % 5, (variant / 3) % 5, false, 0, expected) ==
                  title::ScreenPalettes::Result::Ok);
            CHECK(batch.work.screen.palette == expected && (expected[1] >> 16 & 0xff) == variant % 5);
        }
        if (wave) {
            CHECK(batch.work.waves.amplitude == 2.5f);
            for (uint32_t row = 0; row < 16; ++row)
                for (uint32_t axis = 0; axis < 3; ++axis)
                    CHECK(batch.work.waves.rows[row][axis] == float(row) + float(axis) / 8);
        }
        CHECK(batch.Finish(true) && !batch.Finish(true) && !batch.Ready() && batch.Complete() && !batch.Failed());
        CHECK(!batch.Cancel()); // a no-op after Finish
        CHECK(owner.pass->CanCheckpoint() && owner.pass->CanCheckpoint(true));
    }
    CHECK(owner.pass->Counts().calls == 16 && owner.pass->Counts().replaced == 14 && owner.pass->Counts().empty == 2);
    CHECK(owner.pass->Counts().faults == 0 && owner.pass->Counts().filter_off == 0);
    // FilterColours switched on but g_ColourFilterOn clear: the frame draws
    // nothing visible; the slot free is still the source's (no reservation).
    ++cases;
    Scene(false, true, false, 448, 1, 0, false, 0);
    ram::write<uint8_t>(title::ScreenPalettes::kOn, 0);
    { auto c = Context(); owner.installed(c, S::kDraw); }
    CHECK(owner.pass->Counts().filter_off == 1 && owner.pass->Counts().replaced == 15 && owner.reserved.size() == 14);
    CHECK(owner.slot_frees == 15 && strip_oracle::callees.empty());
    // Refusals to the model: the complete body runs (its callees are reached),
    // nothing is reserved, no owned slot free.
    const auto refuse = [&](S::Refusal expected) {
        ++cases;
        strip_oracle::callees.clear();
        const auto before = owner.pass->Counts();
        const size_t reserved = owner.reserved.size(), frees = owner.slot_frees;
        auto c = Context();
        owner.installed(c, S::kDraw);
        const auto after = owner.pass->Counts();
        CHECK(after.refused == before.refused + 1 && after.model_calls == before.model_calls + 1);
        CHECK(after.last_refusal == expected && after.replaced == before.replaced);
        CHECK(!strip_oracle::callees.empty() && strip_oracle::callees[0].address == S::kFreeTextureSlots);
        CHECK(owner.reserved.size() == reserved && owner.slot_frees == frees);
    };
    Scene(false, true, false, 448, 9, 0, false, 0); // no ninth palette
    refuse(S::Refusal::Palette);
    Scene(false, false, true, 448, 0, 0, false, 0);
    ram::write<uint32_t>(S::kWaveShader + S::kWaveRows, 0x340004); // misaligned rows
    refuse(S::Refusal::Waves);
    Scene(true, false, false, 448, 0, 0, false, 0);
    owner.frame_ok = false;
    refuse(S::Refusal::Frame);
    owner.frame_ok = true;
    owner.frame_height = 480; // the source height and the captured frame disagree
    refuse(S::Refusal::Extent);
    owner.frame_height = 448;
    owner.reserve_ok = false;
    refuse(S::Refusal::Reserve);
    owner.reserve_ok = true;
    // A failed delivery is sticky: every later call refuses as faulted. The
    // failed batch is complete for the checkpoint (nothing survives a frame).
    { auto c = Context(); owner.installed(c, S::kDraw); }
    CHECK(owner.reserved.size() == 15 && !owner.pass->CanCheckpoint() && owner.reserved.back()->Finish(false));
    CHECK(owner.reserved.back()->Failed() && owner.reserved.back()->Complete() && !owner.reserved.back()->Cancel());
    CHECK(owner.pass->Counts().faults == 1 && owner.pass->CanCheckpoint());
    refuse(S::Refusal::Faulted);
    refuse(S::Refusal::Faulted);
    // A reservation the root released unexecuted (GraphicsBridge::Discard ->
    // GraphicsOutput::CancelOwned) is cancelled: no credit, no fault, and the
    // closed boundary is admissible again. Finish after Cancel is refused.
    { ++cases;
      auto fresh_ptr = MakeOwner(true); auto& fresh = *fresh_ptr;
      CHECK(Install(fresh));
      Scene(true, false, false, 448, 0, 0, false, 0);
      auto c = Context(); fresh.installed(c, S::kDraw);
      CHECK(fresh.reserved.size() == 1 && fresh.pass->Counts().replaced == 1 && !fresh.pass->CanCheckpoint());
      const auto& cancelled = *fresh.reserved.back();
      CHECK(cancelled.Cancel() && !cancelled.Cancel() && cancelled.Complete() && !cancelled.Failed() && !cancelled.Ready());
      CHECK(!cancelled.Finish(true) && !cancelled.Finish(false));
      CHECK(fresh.pass->Counts().faults == 0 && fresh.pass->CanCheckpoint() && fresh.pass->CanCheckpoint(true));
      // The next owned call collects it; a later reservation is tracked afresh.
      fresh.installed(c, S::kDraw);
      CHECK(fresh.reserved.size() == 2 && !fresh.pass->CanCheckpoint() && fresh.pass->CanCheckpoint(true));
      CHECK(fresh.reserved.back()->Finish(true) && fresh.pass->CanCheckpoint());
      // ResetAfterQuiescence forgets the reservations (the root's fence, never a restore).
      fresh.installed(c, S::kDraw);
      CHECK(!fresh.pass->CanCheckpoint());
      fresh.pass->ResetAfterQuiescence();
      CHECK(fresh.pass->CanCheckpoint() && fresh.pass->Counts().replaced == 3); }
    // Unreadable parameters refuse too (a foreign $gp).
    { ++cases; strip_oracle::callees.clear(); auto c = Context(0x3114f4); owner.installed(c, S::kDraw);
      CHECK(owner.pass->Counts().unreadable == 1 && owner.pass->Counts().last_refusal == S::Refusal::Unreadable); }
    // A selected pass without its services is refused at construction.
    { bool threw = false; S::Hooks hooks; hooks.enabled = true;
      try { S bad(Reader, hooks); } catch (const std::invalid_argument&) { threw = true; }
      CHECK(threw); }
}

void Boundary() {
    auto owner_ptr = MakeOwner();
    auto& owner = *owner_ptr;
    CHECK(Install(owner) && owner.captures == 2);
    for (unsigned variant = 0; variant < 32; ++variant) {
        ++cases;
        const bool blur = variant & 1, filter = variant & 2, wave = variant & 4;
        const uint32_t height = variant & 8 ? 480 : 448;
        const uint32_t palette = variant % 5, second = (variant / 3) % 5;
        const bool blends = variant & 16;
        const float amount = float(variant) / 31.f;
        Scene(blur, filter, wave, height, palette, second, blends, amount);
        S::Parameters expected;
        CHECK(S::ReadParameters(Reader, Context(), expected) == S::Result::Ok);
        CHECK(expected.blur == blur && expected.filter == filter && expected.wave == wave &&
              expected.width == 512 && expected.height == height);
        strip_oracle::callees.clear();
        auto context = Context();
        owner.installed(context, S::kDraw);
        const auto& reached = strip_oracle::callees;
        if (!expected.Any()) { CHECK(reached.empty() && expected.base == 0); continue; }
        CHECK(expected.base == ((512u * height * 10u) >> 8));
        std::vector<uint32_t> order{S::kFreeTextureSlots, S::kCopyDepth};
        if (blur) order.push_back(S::kBlur);
        if (filter) order.push_back(S::kFilter);
        if (wave) order.push_back(S::kWave);
        CHECK(reached.size() == order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            CHECK(reached[i].address == order[i]);
            if (order[i] == S::kFreeTextureSlots) CHECK(reached[i].a0 == S::kTextureContext && reached[i].a1 == 1);
            else CHECK(reached[i].a0 == expected.base);
            if (order[i] == S::kFilter) {
                uint32_t bits = 0; std::memcpy(&bits, &expected.amount, 4);
                CHECK(reached[i].a1 == expected.palette && reached[i].a2 == expected.second &&
                      bool(reached[i].a3) == expected.blends && reached[i].f12 == bits);
                CHECK(reached[i].a1 == palette && reached[i].a2 == second && bool(reached[i].a3) == blends);
            }
        }
    }
    const auto counts = owner.pass->Counts();
    CHECK(counts.calls == 32 && counts.model_calls == 32 && counts.empty == 4 && counts.effects == 28);
    CHECK(counts.blur == 16 && counts.filter == 16 && counts.wave == 16 && !counts.interior && !counts.unreadable);
    CHECK(owner.pass->CanCheckpoint());
}

void Refusals() {
    Scene(true, false, false, 448, 0, 0, false, 0);
    S::Parameters parameters;
    CHECK(S::ReadParameters(Reader, Context(0x3114f4), parameters) == S::Result::UnsupportedSource);
    ram::write<uint32_t>(S::kScreenWidth, 640);
    CHECK(S::ReadParameters(Reader, Context(), parameters) == S::Result::UnsupportedSource);
    ram::write<uint8_t>(S::kBlurSwitch, 0); // no effect: any extent is the source's early return
    CHECK(S::ReadParameters(Reader, Context(), parameters) == S::Result::Ok && !parameters.Any());
    CHECK(S::ReadParameters(nullptr, Context(), parameters) == S::Result::InvalidMemory);
    // An interior resume is counted, never attributed; a rejecting installer leaves only forwarding.
    { auto op = MakeOwner(); auto& o = *op; CHECK(Install(o));
      Scene(true, true, true, 448, 1, 2, true, .5f);
      strip_oracle::callees.clear();
      auto c = Context(); o.installed(c, S::kDraw + 0x20); // resumes past the switch reads: no callee is proved
      CHECK(o.pass->Counts().interior == 1 && o.pass->Counts().calls == 0 && o.pass->Counts().model_calls == 1); }
    { auto op = MakeOwner(); auto& o = *op; CHECK(!Install(o, false));
      Scene(true, false, false, 448, 0, 0, false, 0);
      strip_oracle::callees.clear();
      auto c = Context(); o.installed(c, S::kDraw);
      CHECK(strip_oracle::callees.size() == 3 && o.pass->Counts().calls == 0); }
    { auto op = MakeOwner(); auto& o = *op; CHECK(Install(o));
      Scene(true, false, false, 448, 0, 0, false, 0);
      strip_oracle::callees.clear();
      auto c = Context(); o.installed(c, S::kDraw);
      CHECK(strip_oracle::callees.size() == 3 && o.slot_frees == 0 && !o.pass->Selected()); }
    { S pass(nullptr); CHECK(!pass.Install([](uint32_t) { return S::Body(); }, [](uint32_t, S::Body) { return true; })); }
}
}

int main() {
    try {
        Boundary();
        Refusals();
        Selected();
        std::printf("ScreenBoundary: %u checks/%u cases, %llu complete generated boundary calls; callee order and arguments pinned\n",
                    checks, cases, static_cast<unsigned long long>(strip_oracle::calls));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
