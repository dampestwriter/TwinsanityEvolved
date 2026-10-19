#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>

#include "tests/lib/fake_backend.h"

namespace {
using Graphics = title::Graphics;
using Resources = title::GraphicsResources;
using Result = Resources::Result;
using ps2r::render::SurfaceId;
using ps2r::render::SurfaceDesc;
using ps2r::render::SurfaceFormat;
int checks = 0, failures = 0;
void Check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

struct Backend : ps2r::test::FakeBackend {
    bool fail_create = false, throw_upload = false, throw_destroy = false, throw_describe = false;
    int maximum = 4096;
    std::vector<std::thread::id> owners;
    ps2r::render::Caps QueryCaps() const override {
        auto caps = FakeBackend::QueryCaps();
        caps.max_texture_size = maximum;
        return caps;
    }
    SurfaceId CreateSurface(const SurfaceDesc& desc) override {
        owners.push_back(std::this_thread::get_id());
        if (fail_create) return {};
        return FakeBackend::CreateSurface(desc);
    }
    SurfaceDesc DescribeSurface(SurfaceId surface) const override {
        if (throw_describe) throw std::runtime_error("describe failure");
        return FakeBackend::DescribeSurface(surface);
    }
    void Upload(SurfaceId surface, const ps2r::render::Rect& rect,
                const void* pixels, size_t stride) override {
        owners.push_back(std::this_thread::get_id());
        if (throw_upload) throw std::runtime_error("upload failure");
        FakeBackend::Upload(surface, rect, pixels, stride);
    }
    void DestroySurface(SurfaceId surface) override {
        owners.push_back(std::this_thread::get_id());
        if (throw_destroy) throw std::runtime_error("destroy failure");
        FakeBackend::DestroySurface(surface);
    }
};

ps2r::gs::DrawIr Triangle() {
    ps2r::gs::DrawIr draw;
    draw.vertex_count = 3;
    draw.verts.resize(3);
    draw.verts[1].x = 1;
    draw.verts[2].y = 1;
    return draw;
}

void TestOrderedUploadsAndOwnedBindings() {
    Backend backend;
    Resources resources(backend);
    Graphics queue;
    Graphics::Handle texture;
    CHECK(resources.Register(queue, {3, 2, SurfaceFormat::Rgba8, false, 1}, &texture) == Result::Ok);
    CHECK(backend.creates == 0); // registration is a producer operation
    CHECK(queue.BeginFrame(5) == Graphics::Result::Ok);
    CHECK(queue.RecordDraw(queue.End(24), Triangle(), {texture}) == Graphics::Result::Ok);
    std::vector<uint8_t> pixels{1,2,3,0x80, 4,5,6,0xff, 7,8,9,0,
                                0xaa,0xaa,0xaa,0xaa, // row padding
                                10,11,12,0x40, 13,14,15,0x78, 16,17,18,0x7f};
    CHECK(queue.RecordUpload(queue.End(0), texture, {0,0,3,2}, 16, pixels) == Graphics::Result::Ok);
    std::fill(pixels.begin(), pixels.end(), 0xee); // snapshot, not borrowed pixels
    CHECK(queue.ReleaseResource(texture) == Graphics::Result::Ok);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(resources.Counts().live_bindings == 1);

    SurfaceId surface;
    Graphics::Lease external;
    Graphics::Hooks hooks;
    size_t fences = 0, draws = 0;
    hooks.drain = [&] { ++fences; return true; };
    hooks.upload = [&](const Graphics::Upload& upload) {
        CHECK(upload.frame == 5);
        return resources.Upload(upload) == Result::Ok;
    };
    hooks.draw = [&](const ps2r::gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings) {
        CHECK(draw.src_frame == 5 && bindings.size() == 1);
        CHECK(resources.Resolve(bindings[0], &surface) == Result::Ok);
        external = bindings[0];
        // OLD upload ordering/freshness pin: draw sees the owned upload that
        // precedes it in bucket order, even though RecordDraw was called first.
        CHECK(backend.uploads == 1);
        const std::vector<uint8_t> expected{1,2,3,0x80,4,5,6,0xff,7,8,9,0,
                                          10,11,12,0x40,13,14,15,0x78,16,17,18,0x7f};
        CHECK(backend.At(surface).rgba == expected);
        ++draws;
        return true;
    };
    CHECK(queue.Submit(hooks) == Graphics::Result::Ok);
    CHECK(draws == 1 && fences == 3);
    CHECK(resources.Counts().uploads == 1 && resources.Counts().uploaded_bytes == 24);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(backend.Has(surface)); // external consumer still owns the allocation
    std::thread producer_release([&] { external.reset(); });
    producer_release.join();
    CHECK(backend.destroys == 0); // last release never calls the context off-thread
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(!backend.Has(surface) && resources.Counts().live_surfaces == 0);
    CHECK(std::all_of(backend.owners.begin(), backend.owners.end(),
                      [](std::thread::id id) { return id == std::this_thread::get_id(); }));
}

Graphics::Lease Capture(Graphics& queue, const Graphics::Handle& handle) {
    Graphics::Lease result;
    CHECK(queue.BeginFrame(0) == Graphics::Result::Ok);
    CHECK(queue.RecordDraw(queue.End(24), Triangle(), {handle}) == Graphics::Result::Ok);
    Graphics::Hooks hooks;
    hooks.draw = [&](const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>& bindings) {
        result = bindings.at(0); return true;
    };
    hooks.drain = [] { return true; };
    CHECK(queue.Submit(hooks) == Graphics::Result::Ok);
    return result;
}

void TestValidationAndFreshVersions() {
    Backend backend;
    Resources resources(backend, {64, 4096, 4});
    Graphics queue;
    Graphics::Handle first, second;
    CHECK(resources.Register(queue, {0,1}, &first) == Result::InvalidDescription);
    CHECK(resources.Register(queue, {65,1}, &first) == Result::InvalidDescription);
    CHECK(resources.Register(queue, {3,4,SurfaceFormat::Rgba8,false,2}, &first) == Result::InvalidDescription);
    CHECK(resources.Register(queue, {4,4,static_cast<SurfaceFormat>(255)}, &first) == Result::InvalidDescription);
    CHECK(resources.Counts().registered == 0);
    CHECK(resources.Register(queue, {2,2}, &first) == Result::Ok);
    auto a = Capture(queue, first);
    Graphics::Upload upload{a, {0,0,2,2}, 8, std::vector<uint8_t>(16, 0x78), 0};
    for (size_t stride : {size_t(0), size_t(7), std::numeric_limits<size_t>::max()}) {
        upload.stride = stride;
        CHECK(resources.Upload(upload) == Result::InvalidUpload);
    }
    upload.stride = 8;
    upload.rectangle = {-1,0,2,2};
    CHECK(resources.Upload(upload) == Result::InvalidUpload);
    upload.rectangle = {0,0,3,2};
    CHECK(resources.Upload(upload) == Result::InvalidUpload);
    upload.rectangle = {0,0,2,2};
    upload.pixels.resize(15);
    CHECK(resources.Upload(upload) == Result::InvalidUpload);
    CHECK(backend.creates == 0 && backend.uploads == 0);

    auto forged = std::make_shared<Graphics::Resource>(*a);
    forged->description.width = 4;
    upload.resource = forged;
    CHECK(resources.Upload(upload) == Result::InvalidResource);
    forged->description = a->description;
    forged->lifetime = std::shared_ptr<const void>(a->lifetime.get(), [](const void*) {});
    upload.resource = forged; // same address, different ownership is not a binding
    SurfaceId unchanged{123};
    CHECK(resources.Resolve(forged, &unchanged) == Result::InvalidResource && unchanged.id == 123);
    upload.resource = a;
    upload.pixels.resize(16, 0x78);
    CHECK(resources.Upload(upload) == Result::Ok);
    SurfaceId old_surface;
    CHECK(resources.Resolve(a, &old_surface) == Result::Ok);
    CHECK(queue.ReleaseResource(first) == Graphics::Result::Ok);

    // A replacement asset has fresh storage while old queued work owns the old
    // version. No guest address/VRAM slot or upload epoch can alias these.
    CHECK(resources.Register(queue, {2,2}, &second) == Result::Ok);
    auto b = Capture(queue, second);
    upload.resource = b;
    upload.pixels.assign(16, 0x80);
    CHECK(resources.Upload(upload) == Result::Ok);
    SurfaceId new_surface;
    CHECK(resources.Resolve(b, &new_surface) == Result::Ok);
    CHECK(old_surface.id != new_surface.id);
    CHECK(backend.At(old_surface).rgba[3] == 0x78);
    CHECK(backend.At(new_surface).rgba[3] == 0x80);
    // Single row requires only its texels; trailing stride padding is unused.
    upload.rectangle = {0,1,2,1};
    upload.pixels.assign(8, 0x7f);
    upload.stride = 20;
    CHECK(resources.Upload(upload) == Result::Ok);
    CHECK(backend.At(new_surface).rgba[11] == 0x7f);
    CHECK(backend.At(new_surface).rgba[3] == 0x80);
    Resources foreign(backend);
    CHECK(foreign.Resolve(b, &unchanged) == Result::InvalidResource);
    CHECK(resources.Resolve({}, &unchanged) == Result::InvalidResource);
    CHECK(queue.Reset() == Graphics::Result::Ok);
    a.reset(); b.reset(); forged.reset(); upload.resource.reset();
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(resources.Counts().live_bindings == 0 && backend.destroys == 2);
}

void TestFailedDeliveryPinsUntilRecovery() {
    for (bool create_failure : {false, true}) {
        Backend backend;
        backend.fail_create = create_failure;
        backend.throw_upload = !create_failure;
        Resources resources(backend);
        Graphics queue;
        Graphics::Handle texture;
        CHECK(resources.Register(queue, {2,2}, &texture) == Result::Ok);
        CHECK(queue.BeginFrame(1) == Graphics::Result::Ok);
        CHECK(queue.RecordUpload(queue.End(0), texture, {0,0,2,2}, 8,
                                 std::vector<uint8_t>(16)) == Graphics::Result::Ok);
        CHECK(queue.RecordDraw(queue.End(24), Triangle(), {texture}) == Graphics::Result::Ok);
        CHECK(queue.ReleaseResource(texture) == Graphics::Result::Ok);
        Graphics::Hooks hooks;
        size_t calls = 0;
        hooks.drain = [] { return true; };
        hooks.upload = [&](const Graphics::Upload& upload) {
            ++calls;
            CHECK(resources.Upload(upload) == Result::BackendFailed);
            SurfaceId untouched{321};
            CHECK(resources.Resolve(upload.resource, &untouched) == Result::BackendFailed);
            CHECK(untouched.id == 321);
            return false;
        };
        hooks.draw = [&](const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>&) {
            CHECK(false); return true;
        };
        CHECK(queue.Submit(hooks) == Graphics::Result::DispatchFailed);
        CHECK(queue.CurrentState() == Graphics::State::Faulted);
        CHECK(resources.CollectAfterQuiescence() == Result::Ok);
        CHECK(resources.Counts().live_bindings == 1); // failed frame still owns it
        CHECK(backend.destroys == 0);
        CHECK(queue.Recover(hooks) == Graphics::Result::Ok);
        CHECK(calls == 1); // failed work is never replayed by recovery
        CHECK(resources.CollectAfterQuiescence() == Result::Ok);
        CHECK(resources.Counts().live_bindings == 0);
        CHECK(backend.destroys == (create_failure ? 0 : 1));
        CHECK(resources.Counts().failures == 1);
    }
}

void TestTypedDrawFailureAndLimits() {
    Backend backend;
    Resources resources(backend, {64, 4096, 1});
    Graphics queue;
    Graphics::Handle texture, other;
    CHECK(resources.Register(queue, {2,2}, &texture) == Result::Ok);
    CHECK(resources.Register(queue, {2,2}, &other) == Result::LimitExceeded);
    CHECK(queue.BeginFrame(1) == Graphics::Result::Ok);
    CHECK(queue.RecordDraw(queue.End(24), Triangle(), {texture}) == Graphics::Result::Ok);
    CHECK(queue.ReleaseResource(texture) == Graphics::Result::Ok);
    Graphics::Hooks hooks;
    size_t called = 0;
    hooks.draw = [&](const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>& bindings) {
        ++called;
        SurfaceId surface;
        CHECK(resources.Resolve(bindings.at(0), &surface) == Result::Ok);
        return false;
    };
    hooks.drain = [] { return true; };
    CHECK(queue.Submit(hooks) == Graphics::Result::DispatchFailed);
    CHECK(queue.Counts().draws == 0 && backend.creates == 1);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok && backend.destroys == 0);
    CHECK(queue.Recover(hooks) == Graphics::Result::Ok);
    CHECK(called == 1);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok && backend.destroys == 1);
    CHECK(resources.Register(queue, {2,2}, &other) == Result::Ok);
    CHECK(queue.Reset() == Graphics::Result::Ok);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);

    Graphics empty(Graphics::Limits{1, 1, 0});
    CHECK(resources.Register(empty, {2,2}, &texture) == Result::QueueRejected);
    CHECK(resources.Counts().live_bindings == 0);
}

void TestBackendLimitsAndFailedCollection() {
    Backend backend;
    Resources resources(backend);
    Graphics queue;
    Graphics::Handle texture;
    CHECK(resources.Register(queue, {2,2}, &texture) == Result::Ok);
    auto lease = Capture(queue, texture);
    backend.maximum = 1;
    SurfaceId surface{123};
    CHECK(resources.Resolve(lease, &surface) == Result::BackendFailed);
    CHECK(surface.id == 123 && backend.creates == 0);
    CHECK(queue.Reset() == Graphics::Result::Ok);
    lease.reset();
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    backend.maximum = 4096;
    CHECK(resources.Register(queue, {2,2}, &texture) == Result::Ok);
    lease = Capture(queue, texture);
    CHECK(resources.Resolve(lease, &surface) == Result::Ok);
    CHECK(queue.Reset() == Graphics::Result::Ok);
    lease.reset();
    backend.throw_destroy = true;
    CHECK(resources.CollectAfterQuiescence() == Result::BackendFailed);
    CHECK(resources.Counts().live_surfaces == 1 && resources.Counts().live_bindings == 1);
    CHECK(backend.Has(surface));
    backend.throw_destroy = false; // explicit backend recovery before retry
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(resources.Counts().live_surfaces == 0 && !backend.Has(surface));
}

void TestExistingSurfaceLifetimeAndIdentity() {
    Backend backend;
    const SurfaceDesc description{8,4,SurfaceFormat::Rgba8,true,2};
    const auto surface = backend.CreateSurface(description);
    size_t released = 0;
    std::thread::id release_thread;
    auto owner = std::shared_ptr<const void>(new int(0), [&](const void* value) {
        delete static_cast<const int*>(value);
        ++released;
        release_thread = std::this_thread::get_id();
        backend.DestroySurface(surface); // actual allocation owner's deleter
    });
    Resources resources(backend, {64, 4096, 1});
    Graphics queue;
    Graphics::Handle first, alias;
    CHECK(resources.RegisterExisting(queue, description, surface, owner, &first) == Result::Ok);
    // An additional queue handle does not consume another allocation slot.
    CHECK(resources.RegisterExisting(queue, description, surface, owner, &alias) == Result::Ok);
    Graphics::Lease a, b;
    CHECK(queue.RetainResource(first, &a) == Graphics::Result::Ok);
    CHECK(queue.RetainResource(alias, &b) == Graphics::Result::Ok);
    CHECK(a != b && a->lifetime == b->lifetime);
    SurfaceId resolved;
    CHECK(resources.Resolve(a, &resolved) == Result::Ok && resolved.id == surface.id);
    CHECK(resources.Resolve(b, &resolved) == Result::Ok && resolved.id == surface.id);
    CHECK(backend.creates == 1 && backend.uploads == 0 && backend.readbacks == 0);
    CHECK(resources.Counts().created == 0 && resources.Counts().live_surfaces == 1);
    CHECK(resources.Counts().live_bindings == 1 && resources.Counts().registered == 2);
    CHECK(queue.BeginFrame(1) == Graphics::Result::Ok);
    CHECK(queue.RecordDraw(queue.End(24), Triangle(), {first}) == Graphics::Result::Ok);
    CHECK(queue.ReleaseResource(first) == Graphics::Result::Ok);
    CHECK(queue.ReleaseResource(alias) == Graphics::Result::Ok);
    a.reset(); b.reset(); owner.reset();
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(released == 0 && backend.Has(surface)); // queued draw pins the real allocation
    Graphics::Lease consumer;
    Graphics::Hooks hooks;
    hooks.draw = [&](const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>& bindings) {
        consumer = bindings.at(0);
        return resources.Resolve(consumer, &resolved) == Result::Ok && resolved.id == surface.id;
    };
    hooks.drain = [] { return true; };
    CHECK(queue.Submit(hooks) == Graphics::Result::Ok);
    CHECK(resources.CollectAfterQuiescence() == Result::Ok && released == 0);
    std::thread producer_release([&] { consumer.reset(); });
    producer_release.join();
    CHECK(released == 0 && backend.Has(surface));
    CHECK(resources.CollectAfterQuiescence() == Result::Ok);
    CHECK(released == 1 && release_thread == std::this_thread::get_id());
    CHECK(backend.destroys == 1 && !backend.Has(surface));
    CHECK(resources.Counts().destroyed == 0 && resources.Counts().live_surfaces == 0);
}

void TestExistingSurfaceRejectionsAndFailure() {
    Backend backend;
    const SurfaceDesc description{4,4,SurfaceFormat::Rgba8,true,1};
    const auto surface = backend.CreateSurface(description);
    auto owner = std::make_shared<int>(1);
    {
        Resources resources(backend, {64, 4096, 2});
        Graphics queue;
        Graphics::Handle handle, rejected;
        CHECK(resources.RegisterExisting(queue, {}, surface, owner, &handle) == Result::InvalidDescription);
        CHECK(resources.RegisterExisting(queue, description, {}, owner, &handle) == Result::InvalidResource);
        CHECK(resources.RegisterExisting(queue, description, {surface.id + 1}, owner, &handle) == Result::InvalidResource);
        CHECK(resources.RegisterExisting(queue, description, surface, {}, &handle) == Result::InvalidResource);
        CHECK(resources.RegisterExisting(queue, description, surface, owner, nullptr) == Result::InvalidDescription);
        backend.throw_describe = true;
        CHECK(resources.RegisterExisting(queue, description, surface, owner, &handle) == Result::BackendFailed);
        backend.throw_describe = false;
        CHECK(resources.Counts().live_bindings == 0 && owner.use_count() == 1);
        CHECK(resources.RegisterExisting(queue, description, surface, owner, &handle) == Result::Ok);
        CHECK(resources.RegisterExisting(queue, {2,4,SurfaceFormat::Rgba8,true,1},
                                         surface, owner, &rejected) == Result::InvalidResource);
        auto forged = std::shared_ptr<const void>(owner.get(), [](const void*) {});
        CHECK(resources.RegisterExisting(queue, description, surface, forged, &rejected) == Result::InvalidResource);
        CHECK(resources.Counts().live_bindings == 1);
        Graphics empty(Graphics::Limits{1,1,0});
        CHECK(resources.RegisterExisting(empty, description, surface, owner, &rejected) == Result::QueueRejected);
        CHECK(resources.Counts().live_bindings == 1); // failed alias cannot erase original
        Graphics::Lease lease;
        CHECK(queue.RetainResource(handle, &lease) == Graphics::Result::Ok);
        SurfaceId resolved;
        CHECK(resources.Resolve(lease, &resolved) == Result::Ok && resolved.id == surface.id);

        Graphics::Handle own;
        CHECK(resources.Register(queue, description, &own) == Result::Ok);
        Graphics::Lease own_lease;
        CHECK(queue.RetainResource(own, &own_lease) == Graphics::Result::Ok);
        SurfaceId own_surface;
        CHECK(resources.Resolve(own_lease, &own_surface) == Result::Ok);
        CHECK(resources.RegisterExisting(queue, description, own_surface, owner, &rejected) == Result::InvalidResource);

        backend.throw_upload = true;
        CHECK(resources.Upload({lease, {0,0,4,4}, 16, std::vector<uint8_t>(64), 0}) == Result::BackendFailed);
        CHECK(resources.RegisterExisting(queue, description, surface, owner, &rejected) == Result::BackendFailed);
        CHECK(resources.Resolve(lease, &resolved) == Result::BackendFailed);
        CHECK(queue.Reset() == Graphics::Result::Ok);
        lease.reset(); own_lease.reset();
        CHECK(resources.CollectAfterQuiescence() == Result::Ok);
        CHECK(backend.Has(surface) && !backend.Has(own_surface));
        CHECK(resources.Counts().destroyed == 1 && resources.Counts().live_surfaces == 0);
        CHECK(owner.use_count() == 1); // collection drops the pin, never destroys imported storage
        backend.throw_upload = false;

        // Failed first registration returns its lease without retaining a stale
        // surface entry. Destructor likewise never destroys an imported surface.
        CHECK(resources.RegisterExisting(empty, description, surface, owner, &rejected) == Result::QueueRejected);
        CHECK(resources.Counts().live_bindings == 0 && resources.Counts().live_surfaces == 0);
        CHECK(owner.use_count() == 1);
        CHECK(resources.RegisterExisting(queue, description, surface, owner, &handle) == Result::Ok);
    }
    CHECK(backend.Has(surface) && owner.use_count() == 1);
    backend.DestroySurface(surface);
    CHECK(backend.destroys == 2); // each actual allocation destroyed exactly once
}
}  // namespace

int main() {
    TestOrderedUploadsAndOwnedBindings();
    TestValidationAndFreshVersions();
    TestFailedDeliveryPinsUntilRecovery();
    TestTypedDrawFailureAndLimits();
    TestBackendLimitsAndFailedCollection();
    TestExistingSurfaceLifetimeAndIdentity();
    TestExistingSurfaceRejectionsAndFailure();
    std::printf("GraphicsResources: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
