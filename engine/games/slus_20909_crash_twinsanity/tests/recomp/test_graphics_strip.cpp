#include "games/slus_20909_crash_twinsanity/composition/graphics_strip.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <cstdio>
#include <cstring>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_draw.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_scene_fixture.h"

namespace {
using namespace ps2r;
using App = app::GraphicsStrip;
using Scene = title::GraphicsScene;
using World = test::StripSceneFixture;
int checks = 0,failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)
constexpr Scene::Frame source{0,8192,64,32,7,31};

title::StripIntent::Output Strip(uint8_t alpha = 64) {
    gs::DrawIr material;
    material.prim_type = 4;
    material.gouraud = material.alpha_blend = material.raster.fba = material.raster.colclamp = true;
    material.raster.frame = {source.color_buffer,source.width,0,0};
    material.raster.zbuf = {source.depth_buffer,0,true};
    material.raster.test.zte = true; material.raster.test.ztst = 2;
    material.raster.alpha = {0,1,0,1,0};
    material.raster.scissor = {0,0,63,31};
    material.off_x = 2016; material.off_y = 2032;
    const title::StripDraw::Position positions[]{{4.5f,3.25f},{4.5f,25},{42.25f,3.25f},{42.25f,25}};
    const uint32_t colors[]{uint32_t(alpha)<<24|0x274fd3,uint32_t(alpha)<<24|0x3157e1,
                            uint32_t(alpha)<<24|0xa54733,uint32_t(alpha)<<24|0xb71723};
    title::StripIntent::Output intent;
    intent.bucket = 24;
    CHECK(title::StripDraw::Build(material,{64,32,0,0},positions,colors,4,{},intent.draw) ==
          title::StripDraw::Result::Ok);
    intent.draw.src_frame = source.source_frame;
    return intent;
}
void Seed(World& world) {
    auto& target = world.cache.GetOrCreate(source.color_buffer,64,32,0,source.epoch);
    world.surfaces.Clear(target.views.color,target.views.depth,0x37674fa3,0.75f);
    target.coherence.valid_rect.Union(0,0,64,32);
    target.coherence.has_content = true;
    target.coherence.needs_frame_clear = false;
    target.coherence.last_clear_frame = world.presenter.EeFrameCount();
    target.coherence.last_z_wrote = true;
    target.coherence.last_zbp = source.depth_buffer;
    target.coherence.last_z_seq = 11;
    target.identity.render_fbw = 64;
    target.identity.has_32bit_content = target.identity.was_32bit = true;
    world.pipeline.RestoreDrawOrder(11);
}
title::SceneState::Snapshot Snapshot(World& world) {
    title::SceneState::Snapshot snapshot;
    std::string why;
    CHECK(world.scene.CaptureState(snapshot,&why));
    if (!why.empty()) std::fprintf(stderr,"%s\n",why.c_str());
    return snapshot;
}

void CaptureAndFailure() {
    World world(source);
    Seed(world);
    std::vector<App::Work> queue;
    Scene::Frame current = source;
    bool available = true,accept = true;
    std::vector<std::pair<uint32_t,uint32_t>> refused;
    App output([&](auto& frame) { frame = current; return available; },
        [&](auto bucket,auto work) {
            CHECK(bucket == 24);
            if (!accept) return false;
            queue.push_back(std::move(work)); return true;
        },[&](uint32_t producer,uint32_t detail) { refused.emplace_back(producer,detail); });
    App::Retained retained;
    const auto color = world.cache.Find(0)->views.color;
    auto intent = Strip();
    intent.producer = title::StripIntent::kPlainAddress;
    CHECK(output.Submit(intent) && queue.size() == 1);
    // The already accepted source frame and source vertices must survive a
    // following setup and source buffer reuse before actual queue delivery.
    current = {0x70000,0x150000,512,448,88,90};
    intent.draw.verts[0].r = 0; intent.draw.verts[0].x = 900;
    CHECK(queue.back()(world.scene,retained) == App::Outcome::Completed);
    CHECK(world.backend.draws == 1 && world.backend.last_params.color.id == color.id &&
          world.backend.last.src_frame == source.source_frame);
    CHECK(world.backend.last.verts[0].r == 0xd3 && world.backend.last.verts[0].x == 4.5f);
    CHECK(!world.backend.last_params.do_frame_clear && world.backend.last.raster.fba &&
          world.backend.last.raster.test.ztst == 2 && world.backend.last.raster.zbuf.zmsk);
    CHECK(world.pipeline.DrawSeq() == 1 && world.pipeline.DrawOrder() == 12 &&
          world.cache.Find(0)->coherence.last_z_seq == 11);
    CHECK(output.Counts().captured == 1 && output.Counts().completed == 1 && !output.Counts().failures);
    CHECK(!world.backend.uploads && !world.backend.readbacks && !world.backend.presents);
    queue.clear();
    available = false;
    CHECK(!output.Submit(Strip()));
    available = true;
    CHECK(!output.Submit(Strip())); // mismatching complete source target
    current = source; accept = false;
    CHECK(!output.Submit(Strip()) && queue.empty());
    accept = true;
    auto aliased = Strip();
    aliased.producer = title::StripIntent::kTintedAddress;
    CHECK(output.Submit(aliased));
    // A retained-pipeline state readable only at the slot (aliased colour
    // pages) REFUSES before drawing: the outcome is Retained with the SAME
    // captured draw for the retained route, attributed when the owner asks.
    world.cache.AliasPages().MarkAliased(0,8192);
    CHECK(queue.back()(world.scene,retained) == App::Outcome::Retained);
    CHECK(world.backend.draws == 1 && output.Counts().completed == 1 && output.Counts().failures == 0 &&
          output.Counts().retained == 1 &&
          output.Counts().first_refusal == (App::kRetained | uint32_t(Scene::Result::UnclosedDependency)));
    CHECK(retained.draw.verts.size() == aliased.draw.verts.size() && retained.draw.src_frame == source.source_frame &&
          retained.draw.verts[0].r == 0xd3 && retained.draw.verts[0].x == 4.5f && retained.draw.raster.frame.fbp == source.color_buffer);
    CHECK(refused.empty() && bool(retained.attribute));
    retained.attribute();
    CHECK(refused.size() == 1 && refused[0].first == title::StripIntent::kTintedAddress &&
          refused[0].second == (App::kRetained | uint32_t(Scene::Result::UnclosedDependency)));
    world.cache.AliasPages().ClearAliased(0,8192);
    // Once the dependency closes the same slot completes; the first refusal is kept.
    CHECK(queue.back()(world.scene,retained) == App::Outcome::Completed);
    CHECK(world.backend.draws == 2 && output.Counts().completed == 2 && output.Counts().retained == 1 &&
          output.Counts().first_refusal == (App::kRetained | uint32_t(Scene::Result::UnclosedDependency)));
    // A stale captured frame (source buffer reuse) is a pre-draw refusal too, not a fault.
    CHECK(output.Submit(Strip()));
    world.cache.AliasPages().MarkCpuDirty(0,8192);
    CHECK(queue.back()(world.scene,retained) == App::Outcome::Retained && output.Counts().retained == 2);
    world.cache.AliasPages().ClearCpuDirty(0,8192);
    CHECK(output.Counts().failures == 0);

    // Every active source material/target dependency is fixed before selection.
    for (unsigned which = 0; which < 7; ++which) {
        auto invalid = Strip();
        if (which == 0) invalid.bucket = 26;
        if (which == 1) invalid.draw.tex_enabled = true;
        if (which == 2) invalid.draw.raster.zbuf.zbp += 8192;
        if (which == 3) invalid.draw.raster.frame.fbw += 64;
        if (which == 4) invalid.draw.raster.fba = false;
        if (which == 5) invalid.draw.raster.alpha.a = 2;
        if (which == 6) invalid.draw.raster.test.date = true;
        CHECK(!output.Submit(std::move(invalid)));
    }
    CHECK(output.Counts().captured == 3);
}

void PixelsAndRestore(render::GlDevice& backend,uint8_t factor,uint8_t alpha) {
    World retained(source,&backend,factor),owned(source,&backend,factor);
    Seed(retained); Seed(owned);
    const auto before = Snapshot(owned);
    const auto intent = Strip(alpha);
    CHECK(retained.pipeline.DrawGeneric(intent.draw).drew);
    std::vector<App::Work> queue;
    App output([](auto& frame) { frame = source; return true; },
        [&](auto,auto work) { queue.push_back(std::move(work)); return true; });
    App::Retained slot;
    CHECK(output.Submit(intent) && queue.back()(owned.scene,slot) == App::Outcome::Completed);
    const auto a = Snapshot(retained),b = Snapshot(owned);
    CHECK(a.targets.size() == 1 && b.targets.size() == 1 && before.targets.size() == 1);
    if (a.targets.empty() || b.targets.empty() || before.targets.empty()) return;
    CHECK(a.targets[0].color == b.targets[0].color);
    CHECK(a.targets[0].depth == b.targets[0].depth && b.targets[0].depth == before.targets[0].depth);
    CHECK(b.targets[0].history.last_z_seq == 11 && b.order == 12);
    // Outside the source rectangle is unchanged, including destination alpha.
    CHECK(std::equal(b.targets[0].color.begin(),b.targets[0].color.begin()+4,
                     before.targets[0].color.begin()));
    // Covered alpha includes FBA after blending. This is real backend output.
    const size_t covered = (size_t(12*factor)*64*factor + 12*factor)*4;
    CHECK((b.targets[0].color.at(covered+3) & 128) != 0);
    std::string why;
    World restored(source,&backend,factor);
    CHECK(restored.scene.StageState(b,&why) && restored.scene.PublishState(&why));
    auto following = Strip(uint8_t(128-alpha/2));
    following.draw.verts[0].r = 19;
    CHECK(retained.pipeline.DrawGeneric(following.draw).drew);
    CHECK(restored.scene.DrawStrip(following,source) == Scene::Result::Ok);
    const auto continued = Snapshot(restored),reference = Snapshot(retained);
    CHECK(reference.targets[0].color == continued.targets[0].color &&
          reference.targets[0].depth == continued.targets[0].depth);
    CHECK(continued.order == 13 && !continued.targets[0].history.needs_frame_clear);
    CHECK(glGetError() == GL_NO_ERROR);
}
} // namespace

int main(int argc,char** argv) {
    if (argc > 1 && std::strcmp(argv[1],"--gl") == 0) {
        platform::Window window;
        platform::GlContext context;
        if (!window.Create(true,128,64,"Graphics Strip") || !context.Create(window)) return 77;
        render::GlDevice backend;
        CHECK(backend.Init([&] { window.Swap(); }));
        backend.SetWindowSize(128,64);
        for (uint8_t scale : {1,2})
            for (uint8_t alpha : {0,64,128,200}) PixelsAndRestore(backend,scale,alpha);
        backend.Shutdown();
    } else CaptureAndFailure();
    std::printf("GraphicsStrip: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
