#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "engine/gs/hle/queue/draw_ir_thread_queue.h"

namespace {
using Graphics = title::Graphics;
using Result = Graphics::Result;
using ps2r::gs::DrawIr;

int checks = 0, failures = 0;
void Check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

struct TestGraphicsSubmission final : ps2r::gs::DrawIrSink {
    std::vector<std::string> events;
    std::vector<DrawIr> draws;
    std::vector<std::thread::id> threads;
    std::function<void()> on_draw;
    void OnDraw(const DrawIr& draw) override {
        if (on_draw) on_draw();
        draws.push_back(draw);
        events.push_back("D" + std::to_string(draw.seq));
        threads.push_back(std::this_thread::get_id());
    }
    void OnJob(const ps2r::gs::JobRec&) override { events.push_back("unexpected-job"); }
    void OnFrame(const ps2r::gs::JobRec&) override { events.push_back("unexpected-frame"); }
};

DrawIr Draw(uint32_t label) {
    DrawIr draw;
    draw.seq = label;
    draw.src_frame = 999;
    draw.vertex_count = 3;
    draw.verts.resize(3);
    draw.verts[0].x = 1.25f;
    draw.verts[1].x = 2.5f;
    draw.verts[2].a = 128;
    draw.raster.frame.fbw = 640;
    draw.raster.frame.fbmsk = 0x12345678;
    return draw;
}

Graphics::Hooks Hooks(TestGraphicsSubmission& sink) {
    Graphics::Hooks hooks;
    hooks.sink = &sink;
    hooks.drain = [&sink] {
        sink.events.push_back("drain");
        return true;
    };
    return hooks;
}

Graphics::Handle Resource(Graphics& graphics, std::shared_ptr<const void> lifetime,
                          uint32_t width = 2, uint32_t height = 2) {
    Graphics::Handle result;
    ps2r::render::SurfaceDesc desc;
    desc.width = width;
    desc.height = height;
    CHECK(graphics.RegisterResource(desc, std::move(lifetime), &result) == Result::Ok);
    return result;
}

void TestBucketAndInsertionOrder() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    auto hooks = Hooks(sink);
    DrawIr pending;
    hooks.legacy = [&](const Graphics::LegacySpan& span, ps2r::gs::DrawIrSink&) {
        sink.events.push_back("L" + std::to_string(span.token));
        pending = Draw(static_cast<uint32_t>(span.token));
        return true;
    };
    hooks.flush_legacy = [&](ps2r::gs::DrawIrSink& output) {
        sink.events.push_back("flush");
        output.OnDraw(pending);
        return true;
    };
    CHECK(graphics.BeginFrame(17) == Result::Ok);
    const auto end = graphics.End(24);
    Graphics::Anchor tail, middle, marker;
    CHECK(graphics.RecordLegacy(end, {90, std::make_shared<const int>(1)}, &tail) == Result::Ok);
    CHECK(graphics.RecordDraw(tail, Draw(20), {}, &middle) == Result::Ok);
    CHECK(graphics.RecordDraw(tail, Draw(30)) == Result::Ok);
    CHECK(graphics.InsertAnchor(middle, &marker) == Result::Ok);
    CHECK(graphics.RecordLegacy(marker, {10, std::make_shared<const int>(2)}) == Result::Ok);
    CHECK(graphics.RecordDraw(marker, Draw(15)) == Result::Ok);
    CHECK(graphics.RecordDraw(end, Draw(95)) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(2), Draw(2)) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(0), Draw(0)) == Result::Ok);
    CHECK(sink.events.empty());  // producer call order has emitted nothing
    CHECK(graphics.Counts().submissions == 0);
    CHECK(graphics.Submit(hooks) == Result::Ok);
    const std::vector<std::string> expected{
        "D0", "D2", "L10", "flush", "D10", "drain", "D15", "D20", "D30",
        "L90", "flush", "D90", "drain", "D95", "drain"};
    CHECK(sink.events == expected);
    CHECK(graphics.PendingEntries() == 0);
    CHECK(graphics.CurrentState() == Graphics::State::Idle);
    CHECK(graphics.Counts().draws == 6);
    CHECK(graphics.Counts().legacy_spans == 2);
    CHECK(graphics.Counts().replaced_bucket_jobs[24] == 4);
    CHECK(graphics.Counts().replaced_bucket_jobs[0] == 1);
    CHECK(graphics.Counts().replaced_bucket_jobs[2] == 1);
    CHECK(graphics.Submit(hooks) == Result::InvalidState);
    CHECK(graphics.BeginFrame(18) == Result::Ok);
    CHECK(graphics.RecordDraw(tail, Draw(999)) == Result::InvalidAnchor);
    CHECK(graphics.Reset() == Result::Ok);
    CHECK(graphics.Counts().submissions == 1);  // census is not reset
}

void TestEveryBucketAndOwnedDraw() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    CHECK(graphics.BeginFrame(22) == Result::Ok);
    auto draw = Draw(0);
    for (uint32_t index = Graphics::kBucketCount; index-- > 0;) {
        draw.seq = index;
        CHECK(graphics.RecordDraw(graphics.End(index), draw) == Result::Ok);
    }
    draw.verts[0].x = -99;
    draw.raster.frame.fbmsk = 0;
    draw.verts.clear();
    CHECK(graphics.Submit(Hooks(sink)) == Result::Ok);
    CHECK(sink.draws.size() == Graphics::kBucketCount);
    for (size_t i = 0; i < sink.draws.size(); ++i) {
        CHECK(sink.draws[i].seq == i);
        CHECK(sink.draws[i].src_frame == 22);
        CHECK(sink.draws[i].verts[0].x == 1.25f);
        CHECK(sink.draws[i].raster.frame.fbmsk == 0x12345678);
    }
}

void TestUploadOwnershipAndRetirement() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    auto backing = std::make_shared<const int>(7);
    std::weak_ptr<const int> weak = backing;
    auto handle = Resource(graphics, backing);
    backing.reset();
    CHECK(graphics.BeginFrame(31) == Result::Ok);
    std::vector<uint8_t> pixels{1,2,3,4,5,6,7,8,91,92,93,94,9,10,11,12,13,14,15,16};
    const auto original = pixels;
    CHECK(graphics.RecordUpload(graphics.End(3), handle, {0,0,2,2}, 12, pixels) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(3), Draw(42), {handle}) == Result::Ok);
    pixels.assign(100, 0);
    CHECK(graphics.ReleaseResource(handle) == Result::Ok);
    CHECK(graphics.ReleaseResource(handle) == Result::InvalidResource);
    CHECK(!weak.expired());
    CHECK(graphics.RecordDraw(graphics.End(3), Draw(43), {handle}) == Result::InvalidResource);
    auto hooks = Hooks(sink);
    hooks.upload = [&](const Graphics::Upload& upload) {
        CHECK(upload.pixels == original);
        CHECK(upload.stride == 12);
        CHECK(upload.frame == 31);
        CHECK(upload.rectangle.w == 2);
        CHECK(upload.resource->description.height == 2);
        CHECK(!weak.expired());
        sink.events.push_back("upload");
        return true;
    };
    hooks.drain = [&] {
        CHECK(!weak.expired());
        sink.events.push_back("drain");
        return true;
    };
    CHECK(graphics.Submit(hooks) == Result::Ok);
    CHECK((sink.events == std::vector<std::string>{"drain", "upload", "drain", "D42", "drain"}));
    CHECK(weak.expired());
    CHECK(graphics.Counts().uploads == 1);
    CHECK(graphics.Counts().replaced_bucket_jobs[3] == 2);
}

void TestInvalidInputsAreTransactional() {
    Graphics graphics;
    Graphics other;
    CHECK(graphics.BeginFrame(1) == Result::Ok);
    CHECK(other.BeginFrame(1) == Result::Ok);
    const auto handle = Resource(graphics, std::make_shared<const int>(1));
    const auto foreign = Resource(other, std::make_shared<const int>(2));
    const auto end = graphics.End(2);
    CHECK(graphics.RecordDraw(other.End(2), Draw(1)) == Result::InvalidAnchor);
    CHECK(graphics.RecordDraw(graphics.End(28), Draw(1)) == Result::InvalidAnchor);
    CHECK(graphics.RecordDraw(Graphics::Anchor{}, Draw(1)) == Result::InvalidAnchor);
    CHECK(graphics.RecordDraw(end, Draw(1), {foreign}) == Result::InvalidResource);
    CHECK(graphics.RecordLegacy(end, {0, {}}) == Result::InvalidIntent);
    CHECK(graphics.InsertAnchor(end, nullptr) == Result::InvalidIntent);
    auto bad_draw = Draw(1);
    bad_draw.vertex_count = 6;
    CHECK(graphics.RecordDraw(end, bad_draw) == Result::InvalidIntent);
    bad_draw.vertex_count = 2;
    bad_draw.verts.resize(2);
    CHECK(graphics.RecordDraw(end, bad_draw) == Result::InvalidIntent);
    std::vector<uint8_t> pixels(16, 0xaa);
    CHECK(graphics.RecordUpload(end, handle, {-1,0,2,2}, 8, pixels) == Result::InvalidIntent);
    CHECK(graphics.RecordUpload(end, handle, {1,0,2,2}, 8, pixels) == Result::InvalidIntent);
    CHECK(graphics.RecordUpload(end, handle, {0,0,2,2}, 7, pixels) == Result::InvalidIntent);
    CHECK(graphics.RecordUpload(end, handle, {0,0,2,2},
                               std::numeric_limits<size_t>::max(), pixels) == Result::InvalidIntent);
    pixels.pop_back();
    CHECK(graphics.RecordUpload(end, handle, {0,0,2,2}, 8, pixels) == Result::InvalidIntent);
    CHECK(graphics.PendingEntries() == 0);
    CHECK(graphics.PayloadBytes() == 0);
    const auto old_end = end;
    CHECK(graphics.Reset() == Result::Ok);
    CHECK(graphics.BeginFrame(1) == Result::Ok);
    CHECK(graphics.RecordDraw(old_end, Draw(1)) == Result::InvalidAnchor);
    CHECK(graphics.RecordDraw(graphics.End(2), Draw(1), {handle}) == Result::InvalidResource);
    CHECK(graphics.ReleaseResource(handle) == Result::InvalidResource);
}

void TestLimitsAndCanceledLeases() {
    Graphics graphics(Graphics::Limits{2, sizeof(ps2r::gs::VertexRec) * 3, 1});
    auto backing = std::make_shared<const int>(3);
    std::weak_ptr<const int> weak = backing;
    const auto handle = Resource(graphics, backing);
    backing.reset();
    Graphics::Handle extra;
    ps2r::render::SurfaceDesc desc;
    desc.width = desc.height = 1;
    CHECK(graphics.RegisterResource(desc, std::make_shared<const int>(9), &extra) == Result::LimitExceeded);
    CHECK(graphics.BeginFrame(1) == Result::Ok);
    const auto end = graphics.End(24);
    CHECK(graphics.RecordDraw(end, Draw(1), {handle}) == Result::Ok);
    CHECK(graphics.RecordDraw(end, Draw(2)) == Result::LimitExceeded);
    Graphics::Anchor marker;
    CHECK(graphics.InsertAnchor(end, &marker) == Result::Ok);
    CHECK(graphics.InsertAnchor(end, &marker) == Result::LimitExceeded);
    CHECK(graphics.ReleaseResource(handle) == Result::Ok);
    CHECK(!weak.expired());
    CHECK(graphics.Reset() == Result::Ok);
    CHECK(weak.expired());
    CHECK(graphics.PayloadBytes() == 0);
    CHECK(graphics.Counts().draws == 0);
}

void TestAdapterPreflightAndFailureRecovery() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    auto hooks = Hooks(sink);
    CHECK(graphics.BeginFrame(1) == Result::Ok);
    auto handle = Resource(graphics, std::make_shared<const int>(3), 1, 1);
    CHECK(graphics.RecordDraw(graphics.End(0), Draw(1)) == Result::Ok);
    CHECK(graphics.RecordUpload(graphics.End(27), handle, {0,0,1,1}, 4, {1,2,3,4}) == Result::Ok);
    CHECK(graphics.Submit(hooks) == Result::MissingAdapter);
    CHECK(sink.events.empty());  // discovers missing late adapter before early draws
    CHECK(graphics.Reset() == Result::Ok);

    auto backing = std::make_shared<const int>(8);
    std::weak_ptr<const int> weak = backing;
    CHECK(graphics.BeginFrame(2) == Result::Ok);
    CHECK(graphics.RecordLegacy(graphics.End(2), {0, backing}) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(2), Draw(2)) == Result::Ok);
    backing.reset();
    unsigned replayed = 0;
    hooks.legacy = [&](const Graphics::LegacySpan&, ps2r::gs::DrawIrSink&) {
        ++replayed;
        return true;
    };
    CHECK(graphics.Submit(hooks) == Result::MissingAdapter);
    CHECK(replayed == 0);
    hooks.flush_legacy = [](ps2r::gs::DrawIrSink&) { return false; };
    CHECK(graphics.Submit(hooks) == Result::DispatchFailed);
    CHECK(replayed == 1);
    CHECK(graphics.CurrentState() == Graphics::State::Faulted);
    CHECK(!weak.expired());
    CHECK(sink.events.empty());  // following native draw never overtakes failed flush
    CHECK(graphics.Counts().replaced_bucket_jobs[2] == 0);
    CHECK(graphics.Submit(hooks) == Result::InvalidState);
    CHECK(graphics.Reset() == Result::InvalidState);
    CHECK(graphics.BeginFrame(3) == Result::InvalidState);
    CHECK(graphics.Recover(hooks) == Result::DispatchFailed);
    hooks.flush_legacy = [](ps2r::gs::DrawIrSink& output) {
        output.OnDraw(Draw(99));
        return true;
    };
    CHECK(graphics.Recover(hooks) == Result::Ok);
    CHECK(replayed == 1);  // recovery flushes pending work, never replays the span
    CHECK(weak.expired());
    CHECK((sink.events == std::vector<std::string>{"D99", "drain"}));
    CHECK(graphics.Counts().submissions == 0);
}

void TestFinalDrainFailureAndReentrancy() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    auto backing = std::make_shared<const int>(11);
    std::weak_ptr<const int> weak = backing;
    auto handle = Resource(graphics, backing);
    backing.reset();
    CHECK(graphics.BeginFrame(3) == Result::Ok);
    const auto anchor = graphics.End(4);
    CHECK(graphics.RecordDraw(anchor, Draw(4), {handle}) == Result::Ok);
    CHECK(graphics.ReleaseResource(handle) == Result::Ok);
    sink.on_draw = [&] {
        CHECK(graphics.Reset() == Result::Busy);
        CHECK(graphics.BeginFrame(10) == Result::Busy);
        CHECK(graphics.RecordDraw(anchor, Draw(5)) == Result::Busy);
        CHECK(graphics.ReleaseResource(handle) == Result::Busy);
    };
    auto hooks = Hooks(sink);
    hooks.drain = [] { return false; };
    CHECK(graphics.Submit(hooks) == Result::DispatchFailed);
    CHECK(!weak.expired());
    CHECK(graphics.Counts().draws == 0);
    CHECK(graphics.Recover(hooks) == Result::DispatchFailed);
    hooks.drain = [] { return true; };
    CHECK(graphics.Recover(hooks) == Result::Ok);
    CHECK(weak.expired());
    CHECK(sink.draws.size() == 1);

    sink.on_draw = [] { throw std::runtime_error("consumer failed"); };
    CHECK(graphics.BeginFrame(4) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(0), Draw(5)) == Result::Ok);
    CHECK(graphics.Submit(hooks) == Result::DispatchFailed);
    CHECK(graphics.Recover(hooks) == Result::Ok);
    CHECK(graphics.Counts().submissions == 0);
}

void TestExistingThreadQueue() {
    Graphics graphics;
    TestGraphicsSubmission sink;
    ps2r::gs::DrawIrThreadQueue queue(
        sink, {}, ps2r::gs::DrawIrThreadQueue::Policy::Pipelined, 1);
    queue.Start();
    auto backing = std::make_shared<const int>(5);
    std::weak_ptr<const int> weak = backing;
    auto handle = Resource(graphics, backing);
    backing.reset();
    CHECK(graphics.BeginFrame(5) == Result::Ok);
    Graphics::Anchor tail;
    CHECK(graphics.RecordLegacy(graphics.End(24), {8, std::make_shared<const int>(8)}, &tail) == Result::Ok);
    CHECK(graphics.RecordDraw(tail, Draw(7), {handle}) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(24), Draw(9), {handle}) == Result::Ok);
    CHECK(graphics.RecordDraw(graphics.End(2), Draw(2), {handle}) == Result::Ok);
    CHECK(graphics.ReleaseResource(handle) == Result::Ok);
    bool expired_on_consumer = false;
    sink.on_draw = [&] { expired_on_consumer |= weak.expired(); };
    Graphics::Hooks hooks;
    hooks.sink = &queue;
    DrawIr deferred;
    hooks.legacy = [&](const Graphics::LegacySpan& span, ps2r::gs::DrawIrSink&) {
        deferred = Draw(static_cast<uint32_t>(span.token));
        return true;
    };
    hooks.flush_legacy = [&](ps2r::gs::DrawIrSink& output) {
        output.OnDraw(deferred);
        return true;
    };
    hooks.drain = [&] {
        CHECK(!weak.expired());
        queue.Sync();
        return true;
    };
    CHECK(graphics.Submit(hooks) == Result::Ok);
    queue.Stop();
    CHECK(!expired_on_consumer);
    CHECK(weak.expired());
    CHECK((sink.events == std::vector<std::string>{"D2", "D7", "D8", "D9"}));
    for (const auto& thread : sink.threads) CHECK(thread != std::this_thread::get_id());
    CHECK(queue.Count().enqueued == 4);
    CHECK(queue.Count().consumed == 4);
}
}  // namespace

int main() {
    TestBucketAndInsertionOrder();
    TestEveryBucketAndOwnedDraw();
    TestUploadOwnershipAndRetirement();
    TestInvalidInputsAreTransactional();
    TestLimitsAndCanceledLeases();
    TestAdapterPreflightAndFailureRecovery();
    TestFinalDrainFailureAndReentrancy();
    TestExistingThreadQueue();
    std::printf("GraphicsSubmission: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
