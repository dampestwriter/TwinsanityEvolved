#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_bridge.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

#include "engine/gs/hle/queue/draw_ir_thread_queue.h"

namespace {
using Bridge = title::GraphicsBridge;
using Result = Bridge::Result;
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

DrawIr Draw(uint32_t label) {
    DrawIr draw;
    draw.seq = label;
    draw.src_frame = 9999;
    draw.vertex_count = 3;
    draw.verts.resize(3);
    draw.verts[0].x = float(label);
    draw.raster.frame.fbp = label * 8192;
    return draw;
}

// The fixture edits only source writer links; it is not a VIF/GIF parser.
// Legacy work occupies callbacks too, so native delivery cannot run before
// deferred legacy assembly even when the guest walk has already returned.
struct TestGraphicsBridge final : ps2r::gs::DrawIrSink {
    static constexpr uint32_t kBuckets = Bridge::kFrameBucketsAddress;
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x400000);
    std::vector<std::function<void()>> slots;
    std::vector<DrawIr> output;
    std::vector<std::thread::id> threads;
    std::vector<DrawIr> pending;
    ps2r::gs::DrawIrSink* destination = this;
    std::function<bool()> drain = [] { return true; };
    bool flushed = true;

    void OnDraw(const DrawIr& draw) override {
        output.push_back(draw);
        threads.push_back(std::this_thread::get_id());
    }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}

    void Word(uint32_t at, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) memory.at(at + i) = uint8_t(value >> (8 * i));
    }
    uint32_t Word(uint32_t at) const {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) value |= uint32_t(memory.at(at + i)) << (8 * i);
        return value;
    }
    uint32_t Writer(uint32_t bucket) const { return kBuckets + 8 + bucket * 0x38; }
    void Link(uint32_t tag, uint32_t next, bool head = false) {
        Word(tag, 0x20000000u | (head ? 1u : 0u));
        Word(tag + 4, next);
        Word(tag + 8, 0);
        Word(tag + 12, 0);
        if (head) {
            Word(tag + 16, 0); Word(tag + 20, 0);
            Word(tag + 24, 0); Word(tag + 28, 0x13000000u);
        }
    }
    void WriterAt(uint32_t writer, uint32_t first, uint32_t last) {
        Word(writer, first);
        Word(writer + 4, first);
        Word(writer + 8, last);
        Word(writer + 0x24, 0xdeadbeef);  // legacy material cache
    }
    title::GraphicsBuckets::Reader Reader() {
        return [this](uint32_t at, uint8_t* out, size_t size) {
            if (at > memory.size() || size > memory.size() - at) return false;
            std::memcpy(out, memory.data() + at, size);
            return true;
        };
    }
    Bridge::Hooks Hooks() {
        Bridge::Hooks hooks;
        hooks.enqueue = [this](std::function<void()> callback) {
            slots.push_back(std::move(callback));
            return true;
        };
        hooks.flush_legacy = [this] {
            if (!flushed) return false;
            for (const auto& draw : pending) destination->OnDraw(draw);
            pending.clear();
            return true;
        };
        hooks.sink = destination;
        hooks.drain = [this] { return drain(); };
        return hooks;
    }
    void Legacy(uint32_t label) {
        slots.push_back([this, label] { pending.push_back(Draw(label)); });
    }
    void Deliver() {
        for (auto& slot : slots) slot();
        slots.clear();
    }
    std::vector<uint32_t> Labels() const {
        std::vector<uint32_t> labels;
        for (const auto& draw : output) labels.push_back(draw.seq);
        return labels;
    }
};

void TestAppendAndSplice() {
    TestGraphicsBridge fixture;
    Bridge bridge(fixture.Hooks());
    const auto reader = fixture.Reader();
    CHECK(bridge.BeginFrame(19) == Result::Ok);
    fixture.WriterAt(fixture.Writer(24), 0x1000, 0x1000);
    fixture.Link(0x1000, 0x1000, true);
    const auto untouched = fixture.memory;
    auto draw = Draw(10);
    CHECK(bridge.AppendStrip(24, reader, draw) == Result::Ok);
    CHECK(bridge.AppendStrip(24, reader, Draw(11)) == Result::Ok);
    CHECK(fixture.memory == untouched);  // includes last2DMaterial and every tag
    draw.verts[0].x = -1;
    draw.raster.frame.fbp = 0;  // recorded state must be owned

    // A later legacy BeginPacket links THROUGH the existing native anchor.
    // Its EndPacket becomes the predecessor for the next native shape.
    fixture.Word(0x1004, 0x1100);
    fixture.Link(0x1100, 0x1200);
    fixture.Link(0x1200, 0);
    fixture.Word(fixture.Writer(24) + 8, 0x1200);
    CHECK(bridge.AppendStrip(24, reader, Draw(30)) == Result::Ok);

    // Material flush splices an independent writer into this bucket. The
    // previous native work remains at the old predecessor, before the splice.
    fixture.Word(0x1204, 0x2000);
    fixture.Link(0x2000, 0x2100);
    fixture.Link(0x2100, 0);
    fixture.Word(fixture.Writer(24) + 8, 0x2100);
    CHECK(bridge.AppendStrip(24, reader, Draw(50)) == Result::Ok);
    fixture.WriterAt(fixture.Writer(2), 0x3000, 0x3000);
    fixture.Link(0x3000, 0x1000, true);
    CHECK(bridge.AppendStrip(2, reader, Draw(5)) == Result::Ok);

    Bridge::Submission submission;
    CHECK(bridge.Seal(fixture.kBuckets, reader, &submission) == Result::Ok);
    CHECK(bridge.EndWalk(submission) == Result::Unvisited);
    CHECK(bridge.WantsTag(submission, 0x1000));
    CHECK(!bridge.WantsTag(submission, 0x1000, true));
    CHECK(!bridge.WantsTag(submission, 0x1010));
    const std::map<uint32_t, uint32_t> legacy{{0x1100, 20}, {0x2000, 40}};
    uint32_t tag = 0x3000;
    size_t visits = 0;
    while (tag != 0 && visits++ < 10) {
        const auto work = legacy.find(tag);
        if (work != legacy.end()) fixture.Legacy(work->second);
        const auto result = bridge.AfterTag(submission, tag);
        CHECK(result == Result::Ok || result == Result::NoMatch);
        tag = fixture.Word(tag + 4);
    }
    CHECK(tag == 0 && visits == 6);
    CHECK(fixture.output.empty());
    CHECK(bridge.EndWalk(submission) == Result::Ok);
    CHECK(bridge.Poll(submission) == Result::Pending);
    CHECK(bridge.AfterTag(submission, 0x1000) == Result::AlreadyQueued);
    CHECK(!bridge.WantsTag(submission, 0x1000));
    CHECK(bridge.AfterTag(submission, 0x1000, true) == Result::NoMatch);
    CHECK(bridge.Counts().completed == 0);

    // Original LinkRenderBuckets can now rotate and reset its frame storage.
    std::fill(fixture.memory.begin(), fixture.memory.end(), 0);
    CHECK(bridge.BeginFrame(20) == Result::Ok);
    fixture.Deliver();
    CHECK(fixture.Labels() == std::vector<uint32_t>({5, 10, 11, 20, 30, 40, 50}));
    CHECK(fixture.output[1].verts[0].x == 10);
    CHECK(fixture.output[1].raster.frame.fbp == 10 * 8192);
    CHECK(fixture.output[1].src_frame == 19);
    CHECK(bridge.Poll(submission) == Result::Ok);
    const auto counts = bridge.Counts();
    CHECK(counts.recorded == 5 && counts.queued_groups == 4 && counts.completed == 5);
    CHECK(counts.replaced_bucket_jobs[2] == 1 && counts.replaced_bucket_jobs[24] == 4);
    CHECK(counts.failed_groups == 0);
}

void TestLegacyInsertionBeforeGlobalAppend() {
    TestGraphicsBridge fixture;
    Bridge bridge(fixture.Hooks());
    auto reader = fixture.Reader();
    CHECK(bridge.BeginFrame(1) == Result::Ok);
    const uint32_t writer = 0x800;
    fixture.WriterAt(writer, 0x4000, 0x4100);
    fixture.Link(0x4000, 0x4100);
    fixture.Link(0x4100, 0);
    // A source material writer was spliced into the global bucket. Its fixed
    // last is also the global append predecessor. Later model insertions
    // change ONLY the local insertion chain before that fixed last.
    fixture.WriterAt(fixture.Writer(24), 0x3000, 0x4100);
    fixture.Link(0x3000, 0x4000, true);
    CHECK(bridge.AppendStrip(24, reader, Draw(5)) == Result::Ok);
    CHECK(bridge.AppendStrip(24, reader, Draw(6)) == Result::Ok);
    fixture.Word(0x4004, 0x4200);
    fixture.Link(0x4200, 0x4300);
    fixture.Link(0x4300, 0x4100);
    fixture.Word(writer + 4, 0x4300);
    fixture.Word(0x4304, 0x4400);
    fixture.Link(0x4400, 0x4500);
    fixture.Link(0x4500, 0x4100);
    fixture.Word(writer + 4, 0x4500);
    // The global writer's next append follows the same fixed last; no native
    // local insertion API or omitted local packet producer is involved.
    fixture.Word(0x4104, 0x4600);
    fixture.Link(0x4600, 0);
    fixture.Word(fixture.Writer(24) + 8, 0x4600);
    CHECK(bridge.AppendStrip(24, reader, Draw(8)) == Result::Ok);
    Bridge::Submission submission;
    CHECK(bridge.Seal(fixture.kBuckets, reader, &submission) == Result::Ok);
    for (uint32_t tag = 0x3000; tag; tag = fixture.Word(tag + 4)) {
        if (tag == 0x4200) fixture.Legacy(3);
        if (tag == 0x4400) fixture.Legacy(4);
        if (tag == 0x4600) fixture.Legacy(7);
        const auto result = bridge.AfterTag(submission, tag);
        CHECK(result == Result::Ok || result == Result::NoMatch);
    }
    fixture.Deliver();
    CHECK(fixture.Labels() == std::vector<uint32_t>({3, 4, 5, 6, 7, 8}));
    CHECK(bridge.Poll(submission) == Result::Ok);
}

void TestGenerationAndRealQueue() {
    TestGraphicsBridge fixture;
    ps2r::gs::DrawIrThreadQueue queue(fixture, {},
        ps2r::gs::DrawIrThreadQueue::Policy::Pipelined, 1);
    queue.Start();
    fixture.destination = &queue;
    fixture.drain = [&queue] { queue.Sync(); return true; };
    Bridge bridge(fixture.Hooks());
    auto reader = fixture.Reader();
    auto allocation = std::make_shared<int>(5);
    std::weak_ptr<int> alive = allocation;
    auto resource = std::make_shared<title::Graphics::Resource>();
    resource->lifetime = allocation;
    CHECK(bridge.BeginFrame(1) == Result::Ok);
    fixture.WriterAt(fixture.Writer(25), 0x5000, 0x5000);
    fixture.Link(0x5000, 0);
    CHECK(bridge.AppendStrip(25, reader, Draw(10), {resource}) == Result::Ok);
    Bridge::Submission old_frame;
    CHECK(bridge.Seal(fixture.kBuckets, reader, &old_frame) == Result::Ok);
    fixture.Legacy(5);
    CHECK(bridge.AfterTag(old_frame, 0x5000) == Result::Ok);
    resource.reset(); allocation.reset();
    CHECK(!alive.expired());

    // Reusing EXACTLY the same guest anchor is a distinct sealed submission.
    CHECK(bridge.BeginFrame(2) == Result::Ok);
    CHECK(bridge.AppendStrip(25, reader, Draw(20)) == Result::Ok);
    Bridge::Submission new_frame;
    CHECK(bridge.Seal(fixture.kBuckets, reader, &new_frame) == Result::Ok);
    fixture.Legacy(15);
    CHECK(bridge.AfterTag(new_frame, 0x5000) == Result::Ok);
    CHECK(bridge.AfterTag(old_frame, 0x5000) == Result::AlreadyQueued);
    auto duplicate = fixture.slots[1];  // duplicate adapter delivery cannot replay the draw
    std::thread ordered_owner([&] { fixture.Deliver(); duplicate(); });
    ordered_owner.join();
    CHECK(fixture.Labels() == std::vector<uint32_t>({5, 10, 15, 20}));
    CHECK(fixture.output[1].src_frame == 1 && fixture.output[3].src_frame == 2);
    for (const auto thread : fixture.threads) CHECK(thread != std::this_thread::get_id());
    CHECK(bridge.Poll(old_frame) == Result::Ok && bridge.Poll(new_frame) == Result::Ok);
    CHECK(bridge.Counts().completed == 2);
    queue.Stop();
    // All borrowed consumer references have drained before runtime reset.
    old_frame = {};
    duplicate = {};
    bridge.Reset();
    CHECK(alive.expired());
}

void TestEligibilityAndLostAnchor() {
    TestGraphicsBridge fixture;
    auto reader = fixture.Reader();
    Bridge bridge(fixture.Hooks());
    CHECK(bridge.BeginFrame(1) == Result::Ok);
    CHECK(bridge.BeginFrame(2) == Result::InvalidState);
    fixture.WriterAt(fixture.Writer(0), 0x6000, 0x6000);
    fixture.Link(0x6000, 0);
    CHECK(bridge.AppendStrip(28, reader, Draw(1)) == Result::InvalidInput);
    auto invalid = Draw(1); invalid.vertex_count = 4;
    CHECK(bridge.AppendStrip(0, reader, invalid) == Result::InvalidInput);
    invalid.vertex_count = 0; invalid.verts.clear();
    CHECK(bridge.AppendStrip(0, reader, invalid) == Result::InvalidInput);
    CHECK(bridge.AppendStrip(0, {}, Draw(1)) == Result::Unavailable);
    auto throws = [](uint32_t, uint8_t*, size_t) -> bool { throw std::runtime_error("read"); };
    CHECK(bridge.AppendStrip(0, throws, Draw(1)) == Result::Unavailable);
    for (const uint32_t prefix : {0x20000000u, 0x40000000u, 0x60000000u, 0x70000000u, 0xc0000000u}) {
        fixture.Word(fixture.Writer(0) + 8, prefix | 0x6000);
        CHECK(bridge.AppendStrip(0, reader, Draw(1)) == Result::UnsupportedAnchor);
    }
    fixture.Word(fixture.Writer(0) + 8, 0x80006000);
    fixture.Word(0x6000, 0x20000002);  // outside the source writer-link shape
    CHECK(bridge.AppendStrip(0, reader, Draw(1)) == Result::UnsupportedAnchor);
    fixture.Link(0x6000, 0, true);
    fixture.Word(0x601c, 0);
    CHECK(bridge.AppendStrip(0, reader, Draw(1)) == Result::UnsupportedAnchor);
    fixture.Word(0x601c, 0x13000000);
    CHECK(bridge.AppendStrip(0, reader, Draw(1)) == Result::Ok);
    fixture.Word(fixture.Writer(0) + 8, 0xa0006000);
    CHECK(bridge.AppendStrip(0, reader, Draw(2)) == Result::Ok);
    Bridge::Submission submission;
    CHECK(bridge.Seal(fixture.kBuckets + 4, reader, &submission) == Result::NoMatch);
    CHECK(bridge.Seal(0x40000000 | fixture.kBuckets, reader, &submission) == Result::NoMatch);
    fixture.Word(0x6008, 0x11000000);
    CHECK(bridge.Seal(fixture.kBuckets, reader, &submission) == Result::AnchorChanged);
    fixture.Word(0x6008, 0);
    fixture.Word(0x6004, 0x6100);  // only the link may change
    CHECK(bridge.Seal(fixture.kBuckets, reader, &submission) == Result::Ok);
    CHECK(bridge.EndWalk(submission) == Result::Unvisited);  // never silently drop accepted geometry
    CHECK(bridge.AfterTag(submission, 0x6100) == Result::NoMatch);
    CHECK(bridge.EndWalk(submission) == Result::Unvisited);
    CHECK(bridge.AfterTag(submission, 0x6000) == Result::Ok);
    auto delayed = fixture.slots.front();
    fixture.slots.clear();
    bridge.Reset();
    delayed();  // a detached pre-reset slot is inert
    CHECK(fixture.output.empty() && bridge.Counts().completed == 0);
    CHECK(bridge.Poll(submission) == Result::InvalidState);
}

void TestAdapterFailures() {
    TestGraphicsBridge fixture;
    const auto reader = fixture.Reader();
    fixture.WriterAt(fixture.Writer(0), 0x7000, 0x7000);
    fixture.Link(0x7000, 0);
    auto missing = fixture.Hooks(); missing.enqueue = {};
    Bridge disabled(missing);
    CHECK(disabled.BeginFrame(1) == Result::Ok);
    CHECK(disabled.AppendStrip(0, reader, Draw(1)) == Result::MissingAdapter);
    CHECK(disabled.Counts().recorded == 0);

    auto rejecting = fixture.Hooks();
    rejecting.enqueue = [](std::function<void()>) { return false; };
    Bridge rejected(rejecting);
    CHECK(rejected.BeginFrame(1) == Result::Ok);
    CHECK(rejected.AppendStrip(0, reader, Draw(1)) == Result::Ok);
    Bridge::Submission bad;
    CHECK(rejected.Seal(fixture.kBuckets, reader, &bad) == Result::Ok);
    CHECK(rejected.AfterTag(bad, 0x7000) == Result::DeliveryFailed);
    CHECK(rejected.EndWalk(bad) == Result::DeliveryFailed);
    CHECK(rejected.AfterTag(bad, 0x7000) == Result::DeliveryFailed);
    CHECK(rejected.Counts().failed_groups == 1 && rejected.Counts().completed == 0);

    Bridge failed(fixture.Hooks());
    CHECK(failed.BeginFrame(1) == Result::Ok);
    auto lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lifetime;
    auto resource = std::make_shared<title::Graphics::Resource>();
    resource->lifetime = lifetime;
    CHECK(failed.AppendStrip(0, reader, Draw(2), {resource}) == Result::Ok);
    CHECK(failed.Seal(fixture.kBuckets, reader, &bad) == Result::Ok);
    CHECK(failed.AfterTag(bad, 0x7000) == Result::Ok);
    resource.reset(); lifetime.reset();
    fixture.flushed = false;
    fixture.Deliver();
    CHECK(failed.Poll(bad) == Result::DeliveryFailed);
    CHECK(fixture.output.empty() && failed.Counts().completed == 0);
    CHECK(!weak.expired());
    bad = {};
    CHECK(!weak.expired());  // bridge pins failed work until quiescent reset
    failed.Reset();
    CHECK(weak.expired());
}

// The ordered-group lifecycle at delivery: the owned callback's FALSE is a
// refusal AT THE SLOT (the owned scene declined before drawing) -- the item
// publishes nothing, the slot is released, the rest of the group and the walk
// continue (completed_work excludes it, refused_work counts it, Poll is Ok);
// only a THROW faults the group (DeliveryFailed, never replayed). This is the
// shape of the all-on frame-62/119 groups (the Send's display copy, the first
// owned text group) whose refusal used to fault the walk and latch the output.
enum class Delivery { Completes, Refuses, Throws };
void TestOwnedWorkOrderingAndFailure() {
    for (Delivery delivery : {Delivery::Completes,Delivery::Refuses,Delivery::Throws}) {
        const bool succeeds = delivery != Delivery::Throws;   // the group completes
        const bool drew = delivery == Delivery::Completes;    // the owned item drew
        TestGraphicsBridge fixture;
        Bridge bridge(fixture.Hooks());
        fixture.WriterAt(fixture.Writer(27),0x8000,0x8000);
        fixture.Link(0x8000,0);
        CHECK(bridge.BeginFrame(7) == Result::Ok);
        fixture.Legacy(1);
        CHECK(bridge.AppendStrip(27,fixture.Reader(),Draw(2)) == Result::Ok);
        auto owner = std::make_shared<int>(3);
        std::weak_ptr<int> weak = owner;
        size_t calls = 0;
        CHECK(bridge.AppendWork(27,fixture.Reader(),[&,owner,delivery] {
            ++calls;
            CHECK(fixture.Labels() == std::vector<uint32_t>({1,2}));
            if (delivery == Delivery::Refuses) return false;  // declined before drawing
            fixture.OnDraw(Draw(uint32_t(*owner)));
            if (delivery == Delivery::Throws) throw std::runtime_error("renderer failed");
            return true;
        }) == Result::Ok);
        owner.reset();
        CHECK(!weak.expired());
        CHECK(bridge.AppendStrip(27,fixture.Reader(),Draw(4)) == Result::Ok);
        Bridge::Submission submission;
        CHECK(bridge.Seal(fixture.kBuckets,fixture.Reader(),&submission) == Result::Ok);
        CHECK(bridge.AfterTag(submission,0x8000) == Result::Ok);
        CHECK(calls == 0 && fixture.output.empty());
        fixture.Deliver();
        CHECK(calls == 1);
        CHECK(fixture.Labels() == (delivery == Delivery::Completes ? std::vector<uint32_t>{1,2,3,4} :
                                   delivery == Delivery::Refuses ? std::vector<uint32_t>{1,2,4} :
                                                                   std::vector<uint32_t>{1,2,3}));
        CHECK(bridge.Counts().recorded_work == 1 && bridge.Counts().recorded == 2);
        CHECK(bridge.Counts().completed_work == (drew ? 1u : 0u));
        CHECK(bridge.Counts().refused_work == (delivery == Delivery::Refuses ? 1u : 0u));
        CHECK(bridge.Counts().completed == (succeeds ? 2u : 0u));
        CHECK(bridge.Counts().failed_groups == (succeeds ? 0u : 1u));
        CHECK(bridge.EndWalk(submission) == (succeeds ? Result::Ok : Result::DeliveryFailed));
        CHECK(bridge.Poll(submission) == (succeeds ? Result::Ok : Result::DeliveryFailed));
        CHECK(bridge.AfterTag(submission,0x8000) == (succeeds ? Result::AlreadyQueued : Result::DeliveryFailed));
        CHECK(calls == 1); // a refusal or failure never replays the callback
        submission = {};
        bridge.Reset();
        CHECK(weak.expired());
    }
}
}  // namespace

int main() {
    TestAppendAndSplice();
    TestLegacyInsertionBeforeGlobalAppend();
    TestGenerationAndRealQueue();
    TestEligibilityAndLostAnchor();
    TestAdapterFailures();
    TestOwnedWorkOrderingAndFailure();
    CHECK(Bridge::kFrameBucketsAddress == 0x00323658);
    std::printf("GraphicsBridge: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
