#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using namespace title;
using F = TextureBindingFixture;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)
constexpr uint32_t shape = 0x140000, material = 0x150000, target = 0x160000;
// RAM, and the fixture's scratchpad for the title's stack (TR entry.cpp:83).
PrimitiveMaterial::Read Reader(F& f, bool scratchpad = true) {
    return [&f, scratchpad](uint32_t address, void* out, size_t size) {
        if ((address & 0xffffc000) == 0x70000000) {
            const uint32_t offset = address & 0x3fff;
            if (!scratchpad || size > f.scratchpad.size() - offset) return false;
            std::memcpy(out, f.scratchpad.data() + offset, size); return true;
        }
        const uint32_t at = address & 0x1fffffff;
        if (at > f.ram.size() || size > f.ram.size() - at) return false;
        std::memcpy(out, f.ram.data() + at, size); return true;
    };
}
void Scene(F& f) {
    TextureAssets::View view;
    CHECK(f.bindings.Read(F::object, F::stream, view) == TextureBindings::Result::Ok);
    f.Shader(14);
    // Mix, STQ, no fog, GEQUAL, no depth writes, FBA.
    f.Put(F::shader, uint64_t{1} | uint64_t{2} << 21 | uint64_t{1} << 26 |
          uint64_t{1} << 27 | uint64_t{1} << 28 | uint64_t{1} << 55 | uint64_t{1} << 59, 8);
    f.Put(shape, material); f.Put(shape + 0xc, 0x2f6b50);
    f.Put(shape + 0x18, 0x3f800000); f.Put(shape + 0x1c, 0x3f800000);
    f.Put(material, F::shader); f.Put(material + 0x10, 1); f.Put(material + 0x20, 24);
    f.Put(MaterialState::kDisplayWidth, 512); f.Put(MaterialState::kDisplayHeight, 448);
    f.Put(MaterialState::kDisplayPixelBytes, 2); f.Put(MaterialState::kDepthBufferPage, 168);
    f.Put(PrimitiveMaterial::kScreenWidth, 512); f.Put(PrimitiveMaterial::kScreenHeight, 448);
    f.Put(MaterialState::kRenderTarget, target);
    f.Put(target + 0xc, 512); f.Put(target + 0x10, 448);
    f.Put(target + 0x18, 512); f.Put(target + 0x1c, 448);
}
void AtomicCapture() {
    F f;
    Scene(f);
    auto read = Reader(f);
    const auto before = f.ram;
    MaterialState::Environment env;
    env.colour_clamp = true;
    env.dither = 1; env.dither_matrix = 0x72503614;
    SpriteIntent::Output out;
    CHECK(SpriteIntent::Capture(read, f.bindings, SpriteIntent::kColoured, shape, 0x80605040, 0, env, out) == SpriteIntent::Result::Ok);
    CHECK(f.ram == before && out.draw.verts.size() == 6 && out.bucket == 24);
    CHECK(out.sampling.asset.image && out.draw.raster.dthe == 1 && out.draw.raster.dimx == 0x72503614);
    CHECK(out.draw.raster.frame.psm == 0 && out.draw.raster.fba && out.draw.raster.zbuf.zmsk);
    CHECK(out.draw.raster.alpha.a == 0 && out.draw.raster.alpha.b == 1 && out.draw.raster.alpha.d == 1);
    CHECK(out.draw.tex.k == -69 && out.draw.tex.mmin == 1 && out.draw.tex.mxl == 4);
    const auto generation = out.sampling.asset.token.generation;
    const auto reject = [&](uint32_t entry, uint32_t object, SpriteIntent::Result expected) {
        const auto ram = f.ram;
        CHECK(SpriteIntent::Capture(read, f.bindings, entry, object, 0, 0, env, out) == expected);
        CHECK(f.ram == ram && out.sampling.asset.token.generation == generation && out.draw.verts.size() == 6);
    };
    reject(SpriteIntent::kPlain + 4, shape, SpriteIntent::Result::UnsupportedEntry);
    // A placed draw's matrix is a stack object, and the title's stack is the
    // scratchpad (TR particles2d.cpp:479/483; entry.cpp:83): the same identity
    // matrix read from RAM and from the stack captures the same quad; a reader
    // without the window refuses it (InvalidMemory), never fabricates one.
    {
        constexpr uint32_t ram_matrix = 0x158000, stack_matrix = 0x70003c40;
        for (unsigned i = 0; i < 4; ++i) f.Put(ram_matrix + i * 20, 0x3f800000);
        std::memcpy(f.scratchpad.data() + (stack_matrix & 0x3fff), f.ram.data() + ram_matrix, 64);
        const auto placed = f.ram, stack = f.scratchpad;
        SpriteIntent::Output from_ram, from_stack;
        CHECK(SpriteIntent::Capture(read, f.bindings, SpriteIntent::kPlaced, shape, ram_matrix, 0, env, from_ram) == SpriteIntent::Result::Ok);
        CHECK(SpriteIntent::Capture(read, f.bindings, SpriteIntent::kPlaced, shape, stack_matrix, 0, env, from_stack) == SpriteIntent::Result::Ok);
        CHECK(from_ram.draw.verts.size() == 6 && from_stack.draw.verts.size() == 6 &&
              std::memcmp(from_ram.draw.verts.data(), from_stack.draw.verts.data(), 6 * sizeof(ps2r::gs::VertexRec)) == 0);
        CHECK(SpriteIntent::Capture(read, f.bindings, SpriteIntent::kPlaced, shape, 0x70003ff0, 0, env, from_stack) == SpriteIntent::Result::InvalidMemory);
        CHECK(SpriteIntent::Capture(Reader(f, false), f.bindings, SpriteIntent::kPlaced, shape, stack_matrix, 0, env, from_stack) == SpriteIntent::Result::InvalidMemory);
        CHECK(f.ram == placed && f.scratchpad == stack);
    }
    reject(SpriteIntent::kPlain, shape + 1, SpriteIntent::Result::InvalidMemory);
    reject(SpriteIntent::kPlain, shape | 0x40000000, SpriteIntent::Result::InvalidMemory);
    f.Put(shape + 0xc, 0);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMemory);
    f.Put(shape + 0xc, 0x2f6b50);
    f.Put(0x2f6b50 + 0x28, 4, 2);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::UnsupportedOwner);
    f.Put(0x2f6b50 + 0x28, 0, 2);
    f.Put(target + 0xc, 0);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMaterial);
    f.Put(target + 0xc, 512);
    f.Put(shape + 0x10, 0x7fc00000);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidGeometry);
    f.Put(shape + 0x10, 0);
    f.Put(material + 0x10, 0);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMaterial);
    f.Put(material + 0x10, 1);
    for (uint32_t type : {3u, 5u, 10u, 16u, 17u, 24u, 28u, 29u}) {
        f.Shader(type);
        reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMaterial);
    }
    f.Shader(14, -69, 0, true, false);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMaterial);
    f.Shader(14);
    CHECK(f.assets.Reset() == TextureAssets::Result::Ok);
    reject(SpriteIntent::kPlain, shape, SpriteIntent::Result::InvalidMaterial);
}
void ServiceOwnership() {
    F f;
    Scene(f);
    CHECK(!SpriteService::Hooks{}.enabled);
    auto read = Reader(f);
    unsigned originals = 0, recorded = 0;
    std::map<uint32_t, SpriteService::Body> bodies;
    const auto capture = [&](uint32_t) -> SpriteService::Body {
        return [&](CpuContext&, uint32_t) { ++originals; };
    };
    const auto install = [&](uint32_t address, SpriteService::Body body) {
        bodies[address] = std::move(body); return true;
    };
    SpriteService disabled(f.bindings, {});
    CHECK(disabled.Install(capture, install));
    CpuContext c{};
    c.cpuRegs.GPR.r[4].UL[0] = shape;
    bodies.at(SpriteIntent::kPlain)(c, SpriteIntent::kPlain + 4);
    CHECK(originals == 1 && disabled.Counts().model_calls == 1);
    SpriteService::Hooks hooks;
    hooks.enabled = true; hooks.read = read;
    bool selected = false, accept = true, environment_ready = true, record_throws = false;
    hooks.selected = [&](uint32_t) { return selected; };
    hooks.environment = [&](MaterialState::Environment& e) { e.colour_clamp = true; return environment_ready; };
    SpriteIntent::Output retained;
    hooks.record = [&](const SpriteIntent::Output& out) {
        if (record_throws) throw std::runtime_error("record");
        if (!accept) return false;
        ++recorded; retained = out; return true;
    };
    struct Refusal { GraphicsRefusals::Reason reason; uint32_t producer, detail; };
    std::vector<Refusal> refusals;
    unsigned faults = 0;
    hooks.refuse = [&](auto reason, uint32_t producer, uint32_t detail) { refusals.push_back({reason, producer, detail}); };
    hooks.fault = [&](auto, uint32_t, uint32_t) { ++faults; };
    SpriteService service(f.bindings, hooks);
    CHECK(service.Install(capture, install));
    CHECK(!service.Install(capture, install));
    bodies.at(SpriteIntent::kPlain)(c, SpriteIntent::kPlain);
    CHECK(originals == 2 && recorded == 0);
    selected = true;
    bodies.at(SpriteIntent::kPlain)(c, 0);
    CHECK(originals == 2 && recorded == 1 && service.Counts().replaced == 1);
    CHECK(service.Counts().model_calls == 1 && service.Counts().failed == 0);
    // Root refused the reservation (nothing recorded): a counted refusal, the
    // original draws, and the producer stays live for the next call.
    accept = false;
    bodies.at(SpriteIntent::kPlain)(c, 0);
    CHECK(service.Counts().failed == 0 && service.Counts().refused == 1 && service.Counts().replaced == 1);
    CHECK(originals == 3 && service.Counts().model_calls == 2);
    CHECK(refusals.size() == 1 && refusals[0].reason == GraphicsRefusals::Reason::Ordering &&
          refusals[0].producer == SpriteIntent::kPlain && refusals[0].detail == SpriteService::kRecordRefused);
    accept = true;
    bodies.at(SpriteIntent::kPlain)(c, 0);
    CHECK(recorded == 2 && originals == 3 && service.Counts().replaced == 2);
    // No frame environment yet (ordering) and an unsupported source shape
    // (capture result) refuse the same way, attributed with their detail.
    environment_ready = false;
    bodies.at(SpriteIntent::kPlain)(c, 0);
    environment_ready = true;
    CHECK(originals == 4 && refusals.size() == 2 && refusals[1].detail == SpriteService::kEnvironment);
    f.Put(shape + 0x10, 0x7fc00000);
    bodies.at(SpriteIntent::kPlain)(c, 0);
    f.Put(shape + 0x10, 0);
    CHECK(originals == 5 && refusals.size() == 3 &&
          refusals[2].reason == GraphicsRefusals::Reason::UnsupportedState &&
          refusals[2].detail == (SpriteService::kCapture | uint32_t(SpriteIntent::Result::InvalidGeometry)));
    CHECK(service.Counts().refused == 3 && service.Counts().model_calls == 4 && service.Counts().failed == 0);
    // A throwing record is a fault after the root accepted: attributed, thrown, no replay.
    record_throws = true;
    bool threw = false;
    try { bodies.at(SpriteIntent::kPlain)(c, 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && service.Counts().failed == 1 && faults == 1 && originals == 5);
    threw = false;
    try { bodies.at(SpriteIntent::kPlain)(c, 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && recorded == 2 && originals == 5); // no replay after a fault
    CHECK(f.assets.Release(retained.sampling.asset.token) == TextureAssets::Result::Ok);
    TextureAssets::View image;
    CHECK(f.assets.Find(retained.sampling.asset.lease, image) == TextureAssets::Result::Ok);
    CHECK(image.image == retained.sampling.asset.image);
    CHECK(f.queue.Counts().legacy_spans == 0);
}
void InstallAndContinuationFailures() {
    F f;
    Scene(f);
    unsigned captures = 0, installs = 0, originals = 0, records = 0;
    std::map<uint32_t, SpriteService::Body> installed;
    SpriteService::Hooks hooks;
    hooks.enabled = true; hooks.read = Reader(f);
    hooks.environment = [](MaterialState::Environment&) { return true; };
    hooks.selected = [](uint32_t) { return false; };
    hooks.record = [&](const SpriteIntent::Output&) { ++records; return true; };
    const auto capture = [&](uint32_t) -> SpriteService::Body {
        ++captures;
        return [&](CpuContext&, uint32_t) { ++originals; };
    };
    const auto install = [&](uint32_t address, SpriteService::Body body) {
        CHECK(captures == 4); // no partial capture/install interleaving
        ++installs;
        installed[address] = std::move(body); return true;
    };
    SpriteService service(f.bindings, hooks);
    CHECK(service.Install(capture, install) && installs == 4);
    CpuContext c{};
    c.cpuRegs.GPR.r[4].UL[0] = shape;
    // An interior entry of a selected producer refuses to its original
    // continuation (ordering); nothing of the owned arm had started.
    installed.at(SpriteIntent::kPlain)(c, SpriteIntent::kPlain + 4);
    CHECK(originals == 1 && records == 0 && service.Counts().failed == 0 && service.Counts().refused == 1);
    bool threw = false;
    // A partially installed producer is faulted: it can neither replay nor refuse.
    captures = installs = 0;
    SpriteService partial(f.bindings, hooks);
    CHECK(!partial.Install(capture, [&](uint32_t address, SpriteService::Body body) {
        if (++installs == 2) return false;
        installed[address] = std::move(body); return true;
    }));
    CHECK(captures == 4 && installs == 2);
    threw = false;
    try { installed.at(SpriteIntent::kPlain)(c, 0); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && originals == 1 && records == 0);
}
}
int main() {
    AtomicCapture(); ServiceOwnership(); InstallAndContinuationFailures();
    std::printf("SpriteService: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
