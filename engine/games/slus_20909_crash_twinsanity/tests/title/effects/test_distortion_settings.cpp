#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_settings.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_material_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <cstdio>
#include <stdexcept>

// DistortionSettings wraps the actual complete generated type18 settings body
// (NTSC 0x1d5f90) on the same RAM the DistortionMaterial oracle uses. The owned
// version must equal the settings the original packet carried at the call and
// must stay fixed when the source constructor writes linear/K/L afterwards.
namespace {
using S = title::DistortionSettings;
using M = title::DistortionMaterial;
namespace ram = strip_oracle::memory;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion settings line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Base = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 59;

struct Fixture {
    DistortionMaterialOracle oracle;
    S owner{oracle.Reader()};
    S::Body installed;
    unsigned captures = 0, installs = 0;
};
bool Install(Fixture& f, bool accept = true) {
    return f.owner.Install(
        [&f](uint32_t address) {
            ++f.captures;
            CHECK(address == S::kCall);
            return S::Body([](CpuContext& c, uint32_t) { strip_oracle::RunGenerated(S::kCall, c); });
        },
        [&f, accept](uint32_t address, S::Body body) {
            ++f.installs;
            CHECK(address == S::kCall);
            f.installed = std::move(body);
            return accept;
        });
}
// The source constructor's settings virtual: a0 is the shader object.
void Call(Fixture& f, uint32_t shader, uint32_t entry = S::kCall) {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    context.cpuRegs.GPR.r[4].UL[0] = shader;
    context.vuRegs.VF[0].w = 1;
    f.installed(context, entry);
}

void Versions() {
    Fixture f;
    CHECK(Install(f) && f.captures == 1 && f.installs == 1);
    M::Settings nothing;
    CHECK(!f.owner.Current(f.oracle.kShader, nothing));
    for (unsigned variant = 0; variant < 24; ++variant) {
        const M::Settings source{Base | uint64_t(variant % 16) << 1 | uint64_t(variant % 3 != 0) << 56 |
            (variant & 4 ? uint64_t{1} << 38 | uint64_t{1} << 41 | uint64_t{2} << 43 : 0),
            int16_t(variant % 2 ? -32768 : 69), int16_t(variant % 3 ? 1 : -1)};
        f.oracle.Reset(source, variant % 3 ? 448 : 480, variant);
        const auto before = DistortionMaterialOracle::Calls();
        Call(f, f.oracle.kShader);
        CHECK(DistortionMaterialOracle::Calls() > before); // the original body ran
        M::Settings owned;
        uint64_t generation = 0;
        CHECK(f.owner.Current(f.oracle.kShader, owned, &generation) && generation == variant + 1);
        CHECK(owned.attributes == source.attributes && owned.lod_k == source.lod_k && owned.lod_l == source.lod_l);
        // The constructor's later writes (TR materials.cpp MakeDistortionMaterial:
        // linear, L=1, K=-200 after the settings virtual) do not reach the version.
        const auto original = f.oracle.Original(true);
        CHECK((ram::read<uint64_t>(f.oracle.kShader) & (uint64_t{1} << 55)) &&
              ram::read<int16_t>(f.oracle.kShader + 0x18) == -200);
        CHECK(f.owner.Current(f.oracle.kShader, owned) && owned.attributes == source.attributes &&
              owned.lod_k == source.lod_k && owned.lod_l == source.lod_l);
        ps2r::gs::DrawIr material;
        CHECK(M::Build(owned, f.oracle.frame, f.oracle.target, 0, material) == M::Result::Ok);
        CHECK(material.tex.mmag == bool(source.attributes & (uint64_t{1} << 55)));
        CHECK(original.size() == 2);
    }
    const auto counts = f.owner.Counts();
    CHECK(counts.calls == 24 && counts.captured == 24 && !counts.rejected && !counts.interior);
    CHECK(f.owner.CanCheckpoint());
}

void Relocation() {
    // A new level makes a new shader object: the old address is no longer a
    // current version, the new one is, and the generation advances.
    Fixture f;
    CHECK(Install(f));
    f.oracle.Reset({Base, 0, 0}, 448, 1);
    Call(f, f.oracle.kShader);
    constexpr uint32_t other = 0x20000;
    ram::write<uint64_t>(other, Base | uint64_t{2} << 1);
    ram::write<uint32_t>(other + 0x64, 0x18);
    ram::write<uint32_t>(other + 0x6c, M::kVtable);
    Call(f, other);
    M::Settings owned; uint64_t generation = 0;
    CHECK(!f.owner.Current(f.oracle.kShader, owned));
    CHECK(f.owner.Current(other, owned, &generation) && generation == 2 && owned.attributes == (Base | uint64_t{2} << 1));
    CHECK(!f.owner.Current(0, owned));
    // Not the retail type18 object: no version is current until the next accepted call.
    ram::write<uint32_t>(other + 0x6c, 0x2faec8);
    Call(f, other);
    CHECK(!f.owner.Current(other, owned) && f.owner.Counts().rejected == 1 && f.owner.Counts().captured == 2);
    ram::write<uint32_t>(other + 0x6c, M::kVtable);
    Call(f, other);
    CHECK(f.owner.Current(other, owned, &generation) && generation == 4);
}

void Checkpoint() {
    Fixture f;
    CHECK(Install(f));
    S::Snapshot empty;
    CHECK(f.owner.CaptureState(empty) && empty.shader == 0 && empty.generation == 0 && S::Validate(empty));
    f.oracle.Reset({Base | uint64_t{1} << 56, 7, -3}, 480, 2);
    Call(f, f.oracle.kShader);
    S::Snapshot snapshot;
    CHECK(f.owner.CaptureState(snapshot) && snapshot.shader == f.oracle.kShader && snapshot.generation == 1);
    CHECK(snapshot.settings.attributes == (Base | uint64_t{1} << 56) && snapshot.settings.lod_k == 7 &&
          snapshot.settings.lod_l == -3);
    Fixture restored;
    CHECK(Install(restored));
    CHECK(restored.owner.Restore(snapshot));
    M::Settings owned; uint64_t generation = 0;
    CHECK(restored.owner.Current(f.oracle.kShader, owned, &generation) && generation == 1 &&
          owned.lod_k == 7 && owned.lod_l == -3);
    // Later calls continue the restored clock.
    f.oracle.Reset({Base, 0, 0}, 448, 0);
    Call(restored, f.oracle.kShader);
    CHECK(restored.owner.Current(f.oracle.kShader, owned, &generation) && generation == 2 && !owned.lod_k);
    S::Snapshot bad = snapshot; bad.version = 2;
    CHECK(!S::Validate(bad) && !restored.owner.Restore(bad));
    bad = snapshot; bad.shader = 0x02000000 - 0x10;
    CHECK(!S::Validate(bad));
    bad = snapshot; bad.shader |= 2;
    CHECK(!S::Validate(bad));
    bad = snapshot; bad.generation = 0;
    CHECK(!S::Validate(bad));
    bad = snapshot; bad.settings.attributes = uint64_t{1} << 30; // fog: outside the proved family
    CHECK(!S::Validate(bad));
    bad = snapshot; bad.shader = 0; bad.generation = 0;
    CHECK(S::Validate(bad) && restored.owner.Restore(bad) && !restored.owner.Current(f.oracle.kShader, owned));
}

void Refusals() {
    // An interior resume has no proved shader argument: refused, never captured.
    { Fixture f; CHECK(Install(f));
      f.oracle.Reset({Base, 0, 0}, 448, 0);
      bool thrown = false;
      try { Call(f, f.oracle.kShader, S::kCall + 8); } catch (const std::runtime_error&) { thrown = true; }
      M::Settings owned;
      CHECK(thrown && f.owner.Counts().interior == 1 && !f.owner.Counts().calls &&
            !f.owner.Current(f.oracle.kShader, owned) && !f.owner.CanCheckpoint());
      S::Snapshot snapshot;
      CHECK(!f.owner.CaptureState(snapshot)); }
    // A rejecting installer leaves the owner uninstalled and forwarding only.
    { Fixture f; CHECK(!Install(f, false) && f.installs == 1);
      f.oracle.Reset({Base, 0, 0}, 448, 0);
      const auto before = DistortionMaterialOracle::Calls();
      Call(f, f.oracle.kShader);
      M::Settings owned;
      CHECK(DistortionMaterialOracle::Calls() > before && !f.owner.Counts().calls &&
            !f.owner.Current(f.oracle.kShader, owned)); }
    { S owner(nullptr); CHECK(!owner.Install([](uint32_t) { return S::Body(); }, [](uint32_t, S::Body) { return true; })); }
    { Fixture f; CHECK(!f.owner.Install([](uint32_t) { return S::Body(); }, [](uint32_t, S::Body) { return true; })); }
}
}

int main() {
    try {
        Versions();
        Relocation();
        Checkpoint();
        Refusals();
        std::printf("DistortionSettings: %u checks, %llu complete generated settings calls; version pinned at the call\n",
                    checks, static_cast<unsigned long long>(DistortionMaterialOracle::Calls()));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
