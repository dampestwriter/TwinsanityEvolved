#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_fixture.h"

#include <cstdio>

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
}
int main() {
    using R = title::ShadowRenderer;
    ps2r::test::FakeBackend backend;
    ShadowFixture fixture(backend, 512, 448);
    R renderer(backend, fixture.resources);
    auto volume = ShadowFixture::Volume(256, 224, 8, 4294967040.f);
    const title::ShadowDraw::Frame frame{4, 512, 448};
    auto scene = fixture.scene;
    scene.publish = {};
    CHECK(renderer.Draw(frame, {volume}, scene) == R::Result::InvalidScene);
    CHECK(backend.draws == 0 && fixture.publications == 0);
    scene = fixture.scene;
    scene.images[2] = scene.images[0];
    CHECK(renderer.Draw(frame, {volume}, scene) == R::Result::InvalidScene);
    CHECK(backend.draws == 0);
    CHECK(renderer.Draw(frame, {volume}, fixture.scene) == R::Result::Ok);
    CHECK(fixture.publications == 1);
    CHECK(backend.draws == 7 && backend.stencil_clears == 1 && backend.const_quads == 34);
    CHECK(renderer.Counts().passes == 1 && renderer.Counts().strips == 1);
    CHECK(renderer.Counts().allocations == 6);
    CHECK(backend.stencil_calls.front().clear_value == 15);
    CHECK(renderer.Draw(frame, {}, fixture.scene) == R::Result::Ok);
    CHECK(renderer.Counts().allocations == 6 && fixture.publications == 2);
    // A real publication failure leaves the pass faulted, not silently replayed.
    fixture.publication_ok = false;
    CHECK(renderer.Draw(frame, {volume}, fixture.scene) == R::Result::PublicationFailure);
    const int submitted = backend.draws;
    CHECK(renderer.Draw(frame, {volume}, fixture.scene) == R::Result::Faulted);
    CHECK(backend.draws == submitted && renderer.Counts().passes == 2);
    renderer.ResetAfterQuiescence();
    CHECK(fixture.resources.CollectAfterQuiescence() == title::GraphicsResources::Result::Ok);
    CHECK(backend.destroys == 6);
    fixture.publication_ok = true;
    CHECK(renderer.Draw(frame, {volume}, fixture.scene) == R::Result::Ok);
    CHECK(renderer.Counts().passes == 3);
    // A foreign registry with plausible descriptors is not a scene allocation.
    title::GraphicsResources foreign(backend);
    title::Graphics queue;
    title::Graphics::Handle handle;
    CHECK(foreign.Register(queue, fixture.scene.images[0]->description, &handle) ==
          title::GraphicsResources::Result::Ok);
    scene = fixture.scene;
    CHECK(queue.RetainResource(handle, &scene.images[0]) == title::Graphics::Result::Ok);
    CHECK(renderer.Draw(frame, {}, scene) == R::Result::InvalidScene);
    CHECK(renderer.Counts().passes == 3);
    std::printf("ShadowRenderer: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
