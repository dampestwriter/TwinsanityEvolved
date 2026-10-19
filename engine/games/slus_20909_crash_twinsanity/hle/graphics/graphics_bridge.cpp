#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_bridge.h"

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace title {
namespace {
uint32_t Word(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

bool RamAddress(uint32_t address, size_t size, uint32_t* physical) {
    // Title pointers use main RAM or its KSEG aliases. Do not alias an MMIO,
    // scratchpad or out-of-range pointer into RAM with the walker's wrap mask.
    const uint32_t segment = address & 0xe0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) return false;
    const uint32_t at = address & 0x1fffffffu;
    if (at >= 0x02000000u || size > 0x02000000u - at) return false;
    *physical = at;
    return true;
}

bool Read(const GraphicsBuckets::Reader& reader, uint32_t address,
          uint8_t* out, size_t size) {
    try { return reader && reader(address, out, size); }
    catch (...) { return false; }
}

bool LinkShape(const std::array<uint8_t, 32>& bytes, size_t* size) {
    // Only source-created writer links: EndPacket/StartWriter NEXT with no
    // payload, or StartBucket NEXT with its one FLUSHA quadword. This is an
    // eligibility check, not a command interpreter.
    const uint32_t kind = Word(bytes.data());
    if ((kind != 0x20000000u && kind != 0x20000001u) ||
        Word(bytes.data() + 8) || Word(bytes.data() + 12)) return false;
    *size = kind == 0x20000000u ? 16 : 32;
    return true;
}
}  // namespace

struct GraphicsBridge::Batch {
    enum class State { Recorded, Queued, Delivering, Complete, Failed, Discarded };
    struct Item {
        ps2r::gs::DrawIr draw;
        std::vector<Graphics::Lease> leases;
        std::function<bool()> work;
        std::function<void()> cancel;
    };
    struct Group {
        uint32_t bucket = 0;
        std::array<uint8_t, 32> anchor{};
        size_t anchor_size = 0;
        std::vector<Item> items;
        std::atomic<State> state{State::Recorded};
    };
    std::shared_ptr<std::atomic<bool>> epoch;
    std::unordered_map<uint32_t, std::shared_ptr<Group>> groups;
};

struct GraphicsBridge::Impl {
    struct Counts {
        std::atomic<uint64_t> recorded{0}, queued_groups{0}, completed{0}, failed_groups{0}, discarded_groups{0};
        std::atomic<uint64_t> recorded_work{0}, completed_work{0}, refused_work{0};
        std::array<std::atomic<uint64_t>, Graphics::kBucketCount> buckets{};
    };
    Hooks hooks;
    uint32_t frame = 0;
    std::shared_ptr<Batch> recording;
    std::shared_ptr<std::atomic<bool>> epoch = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    // Failed delivery may have published borrowed resource references. Keep
    // those leases until explicit quiescent Reset, not just callback return.
    std::vector<std::shared_ptr<Batch>> retained;
};

GraphicsBridge::GraphicsBridge(Hooks hooks) : impl_(new Impl) {
    impl_->hooks = std::move(hooks);
}
GraphicsBridge::~GraphicsBridge() { Reset(); }

GraphicsBridge::Result GraphicsBridge::BeginFrame(uint32_t frame) {
    if (impl_->recording) return Result::InvalidState;
    impl_->frame = frame;
    impl_->recording = std::make_shared<Batch>();
    impl_->recording->epoch = impl_->epoch;
    // A completed or cancelled batch has no consumer borrows left.
    auto& retained = impl_->retained;
    for (auto it = retained.begin(); it != retained.end();) {
        bool complete = true;
        for (const auto& entry : (*it)->groups) {
            const auto state = entry.second->state.load();
            complete &= state == Batch::State::Complete || state == Batch::State::Discarded;
        }
        if (complete) it = retained.erase(it);
        else ++it;
    }
    return Result::Ok;
}

GraphicsBridge::Result GraphicsBridge::AppendStrip(
    uint32_t bucket, const GraphicsBuckets::Reader& reader, const ps2r::gs::DrawIr& draw,
    const std::vector<Graphics::Lease>& leases) {
    if (draw.vertex_count != draw.verts.size() || draw.verts.empty() || draw.verts.size() % 3 != 0)
        return Result::InvalidInput;
    return Append(bucket, reader, &draw, {}, leases, {});
}

GraphicsBridge::Result GraphicsBridge::AppendWork(
    uint32_t bucket, const GraphicsBuckets::Reader& reader, std::function<bool()> work,
    const std::vector<Graphics::Lease>& leases, std::function<void()> cancel) {
    if (!work) return Result::InvalidInput;
    return Append(bucket, reader, nullptr, std::move(work), leases, std::move(cancel));
}

GraphicsBridge::Result GraphicsBridge::Append(
    uint32_t bucket, const GraphicsBuckets::Reader& reader, const ps2r::gs::DrawIr* draw,
    std::function<bool()> work, const std::vector<Graphics::Lease>& leases, std::function<void()> cancel) {
    if (!impl_->recording) return Result::InvalidState;
    const auto& hooks = impl_->hooks;
    if (!hooks.enqueue || !hooks.flush_legacy || (draw && !hooks.sink) || !hooks.drain)
        return Result::MissingAdapter;
    if (bucket >= Graphics::kBucketCount) return Result::InvalidInput;
    for (const auto& lease : leases) if (!lease) return Result::InvalidInput;
    // NTSC FUN_001a6068 (recompiled_shard_011.cpp:7620) uses this global,
    // bucket*0x38 and +0x10: FrameBuckets +8 plus RenderBucket.last +8.
    // TR:src/platform/ps2/renderer/draw2d.cpp:48, renderer.h:1339/1369
    // specify append-only chaining. Inserted material writers are NOT exposed
    // as native producers here; they require their own anchor ownership.
    uint8_t pointer[4];
    if (!Read(reader, kFrameBucketsAddress + bucket * 0x38 + 0x10, pointer, 4))
        return Result::Unavailable;
    uint32_t tag = 0;
    if ((Word(pointer) & 15) || !RamAddress(Word(pointer), 16, &tag))
        return Result::UnsupportedAnchor;
    std::array<uint8_t, 32> anchor{};
    if (!Read(reader, tag, anchor.data(), 16)) return Result::Unavailable;
    size_t anchor_size = 0;
    if (!LinkShape(anchor, &anchor_size)) return Result::UnsupportedAnchor;
    if (anchor_size == 32 &&
        (!RamAddress(tag, 32, &tag) || !Read(reader, tag + 16, anchor.data() + 16, 16)))
        return Result::Unavailable;
    if (anchor_size == 32 &&
        (Word(anchor.data() + 16) || Word(anchor.data() + 20) ||
         Word(anchor.data() + 24) || Word(anchor.data() + 28) != 0x13000000u))
        return Result::UnsupportedAnchor;

    auto& groups = impl_->recording->groups;
    const auto existing = groups.find(tag);
    auto group = existing == groups.end() ? std::make_shared<Batch::Group>() : existing->second;
    if (existing != groups.end() && group->bucket != bucket) return Result::UnsupportedAnchor;
    // BeginPacket changes only the link address. Repeated skipped producers
    // share this predecessor and append owned draws in call order.
    if (existing == groups.end()) {
        group->bucket = bucket;
        group->anchor = anchor;
        group->anchor_size = anchor_size;
    }
    Batch::Item item{draw ? *draw : ps2r::gs::DrawIr{}, leases, std::move(work), std::move(cancel)};
    item.draw.src_frame = impl_->frame;
    group->items.push_back(std::move(item));
    if (existing == groups.end()) groups.emplace(tag, std::move(group));
    if (draw) impl_->counts->recorded.fetch_add(1, std::memory_order_relaxed);
    else impl_->counts->recorded_work.fetch_add(1, std::memory_order_relaxed);
    return Result::Ok;
}

GraphicsBridge::Result GraphicsBridge::Seal(
    uint32_t buckets, const GraphicsBuckets::Reader& reader, Submission* output) {
    if (!impl_->recording || !output) return Result::InvalidState;
    uint32_t physical = 0;
    if (!RamAddress(buckets, GraphicsBuckets::kObjectBytes, &physical) ||
        physical != kFrameBucketsAddress) return Result::NoMatch;
    for (const auto& entry : impl_->recording->groups) {
        const auto& group = *entry.second;
        std::array<uint8_t, 32> current{};
        if (!Read(reader, entry.first, current.data(), group.anchor_size))
            return Result::Unavailable;
        if (std::memcmp(current.data(), group.anchor.data(), 4) ||
            std::memcmp(current.data() + 8, group.anchor.data() + 8, group.anchor_size - 8))
            return Result::AnchorChanged;
    }
    impl_->retained.push_back(impl_->recording);
    output->batch_ = std::move(impl_->recording);
    return Result::Ok;
}

bool GraphicsBridge::WantsTag(
    const Submission& submission, uint32_t address, bool from_scratchpad) const {
    const auto batch = submission.batch_;
    if (!batch || batch->epoch != impl_->epoch || !batch->epoch->load() ||
        from_scratchpad || (address & 15) || address >= 0x02000000u) return false;
    const auto at = batch->groups.find(address);
    return at != batch->groups.end() && at->second->state.load() == Batch::State::Recorded;
}

GraphicsBridge::Result GraphicsBridge::AfterTag(
    const Submission& submission, uint32_t address, bool from_scratchpad) {
    const auto batch = submission.batch_;
    if (!batch || batch->epoch != impl_->epoch) return Result::InvalidState;
    if (!batch->epoch->load()) return Result::Cancelled;
    if (from_scratchpad || (address & 15) || address >= 0x02000000u) return Result::NoMatch;
    const auto at = batch->groups.find(address);
    if (at == batch->groups.end()) return Result::NoMatch;
    auto group = at->second;
    auto expected = Batch::State::Recorded;
    if (!group->state.compare_exchange_strong(expected, Batch::State::Queued))
        return expected == Batch::State::Failed ? Result::DeliveryFailed : Result::AlreadyQueued;
    const auto counts = impl_->counts;
    const auto hooks = impl_->hooks;
    const auto fail = [group, counts] {
        if (group->state.exchange(Batch::State::Failed) != Batch::State::Failed)
            counts->failed_groups.fetch_add(1, std::memory_order_relaxed);
    };
    auto deliver = [group, epoch = batch->epoch, counts, hooks, fail] {
        if (!epoch->load()) return;
        auto queued = Batch::State::Queued;
        if (!group->state.compare_exchange_strong(queued, Batch::State::Delivering)) return;
        try {
            if (!hooks.flush_legacy()) { fail(); return; }
            size_t draws = 0, work = 0, refused = 0;
            for (const auto& item : group->items) {
                if (item.work) {
                    // False = refused at this slot before anything was drawn:
                    // the slot is released (the caller attributed it); the
                    // rest of the group and the walk continue. A throw is the
                    // fault (the catch below).
                    if (item.work()) ++work; else ++refused;
                } else {
                    hooks.sink->OnDraw(item.draw);
                    ++draws;
                }
            }
            if (!hooks.drain()) { fail(); return; }
            counts->completed.fetch_add(draws, std::memory_order_relaxed);
            counts->completed_work.fetch_add(work, std::memory_order_relaxed);
            counts->refused_work.fetch_add(refused, std::memory_order_relaxed);
            counts->buckets[group->bucket].fetch_add(group->items.size(), std::memory_order_relaxed);
            group->state.store(Batch::State::Complete);
        } catch (...) { fail(); }
    };
    try {
        if (!hooks.enqueue(std::move(deliver))) { fail(); return Result::DeliveryFailed; }
    } catch (...) { fail(); return Result::DeliveryFailed; }
    counts->queued_groups.fetch_add(1, std::memory_order_relaxed);
    return group->state.load() == Batch::State::Failed ? Result::DeliveryFailed : Result::Ok;
}

GraphicsBridge::Result GraphicsBridge::EndWalk(const Submission& submission) const {
    const auto batch = submission.batch_;
    if (!batch || batch->epoch != impl_->epoch) return Result::InvalidState;
    if (!batch->epoch->load()) return Result::Cancelled;
    bool unvisited = false;
    for (const auto& entry : batch->groups) {
        const auto state = entry.second->state.load();
        if (state == Batch::State::Failed) return Result::DeliveryFailed;
        unvisited |= state == Batch::State::Recorded;
    }
    return unvisited ? Result::Unvisited : Result::Ok;
}

GraphicsBridge::Result GraphicsBridge::Poll(const Submission& submission) const {
    const auto result = EndWalk(submission);
    if (result != Result::Ok) return result;
    for (const auto& entry : submission.batch_->groups) {
        const auto state = entry.second->state.load();
        if (state != Batch::State::Complete && state != Batch::State::Discarded) return Result::Pending;
    }
    return Result::Ok;
}

size_t GraphicsBridge::Discard(const Submission& submission) {
    const auto batch = submission.batch_;
    if (!batch || batch->epoch != impl_->epoch || !batch->epoch->load()) return 0;
    size_t discarded = 0;
    for (auto& entry : batch->groups) {
        auto& group = *entry.second;
        auto expected = Batch::State::Recorded;
        if (!group.state.compare_exchange_strong(expected, Batch::State::Discarded)) continue;
        for (auto& item : group.items) {
            if (item.cancel) item.cancel();
            item.work = {};
            item.cancel = {};
        }
        ++discarded;
    }
    impl_->counts->discarded_groups.fetch_add(discarded, std::memory_order_relaxed);
    return discarded;
}

GraphicsBridge::Recording GraphicsBridge::OpenRecording() const {
    Recording out;
    if (!impl_->recording) return out;
    for (const auto& entry : impl_->recording->groups) {
        const auto& group = *entry.second;
        if (group.state.load() != Batch::State::Recorded) continue;
        ++out.groups;
        out.items += uint32_t(group.items.size());
        if (group.bucket < out.bucket_items.size()) out.bucket_items[group.bucket] += uint32_t(group.items.size());
    }
    return out;
}

void GraphicsBridge::Reset() {
    impl_->epoch->store(false);
    impl_->recording.reset();
    impl_->retained.clear();
    impl_->epoch = std::make_shared<std::atomic<bool>>(true);
}

GraphicsBridge::Counters GraphicsBridge::Counts() const {
    Counters out;
    const auto& c = *impl_->counts;
    out.recorded = c.recorded.load(std::memory_order_relaxed);
    out.queued_groups = c.queued_groups.load(std::memory_order_relaxed);
    out.completed = c.completed.load(std::memory_order_relaxed);
    out.failed_groups = c.failed_groups.load(std::memory_order_relaxed);
    out.discarded_groups = c.discarded_groups.load(std::memory_order_relaxed);
    out.recorded_work = c.recorded_work.load(std::memory_order_relaxed);
    out.completed_work = c.completed_work.load(std::memory_order_relaxed);
    out.refused_work = c.refused_work.load(std::memory_order_relaxed);
    for (size_t i = 0; i < out.replaced_bucket_jobs.size(); ++i)
        out.replaced_bucket_jobs[i] = c.buckets[i].load(std::memory_order_relaxed);
    return out;
}

}  // namespace title
