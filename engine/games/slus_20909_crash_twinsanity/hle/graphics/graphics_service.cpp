#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"

#include <array>
#include <deque>
#include <stdexcept>
#include <utility>

#include "engine/ee/cpu/cpu_state.h"

namespace title {
namespace {
uint32_t Word(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

bool Physical(uint32_t address, size_t size, uint32_t& out) {
    const uint32_t segment = address & 0xe0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) return false;
    out = address & 0x1fffffffu;
    return out < 0x02000000u && size <= 0x02000000u - out;
}

bool Read(const GraphicsBuckets::Reader& reader, uint32_t address, void* out, size_t size) {
    uint32_t physical = 0;
    if (!out || !Physical(address, size, physical)) return false;
    try { return reader && reader(physical, static_cast<uint8_t*>(out), size); }
    catch (...) { return false; }
}
}  // namespace

struct GraphicsService::Impl {
    struct Pending {
        uint32_t root = 0;
        bool started = false;
        GraphicsBridge::Submission submission;
    };
    explicit Impl(Hooks value) : hooks(std::move(value)), bridge(hooks.delivery) {}
    Hooks hooks;
    GraphicsBridge bridge;
    std::array<Body, kStripAddresses.size()> strips;
    Body link;
    bool installed = false, frame_open = false, faulted = false;
    uint32_t frame = 0;
    uint64_t current_draws = 0;
    Counters counts;
    std::deque<std::shared_ptr<Pending>> pending;
    std::shared_ptr<Pending> walking;
};

GraphicsService::GraphicsService(Hooks hooks) : impl_(new Impl(std::move(hooks))) {}
GraphicsService::~GraphicsService() = default;

bool GraphicsService::Install(const Capture& capture, const Installer& installer) {
    auto& p = *impl_;
    if (p.installed) return true;
    const auto& d = p.hooks.delivery;
    if (!capture || !installer || !p.hooks.read || !p.hooks.environment ||
        !p.hooks.flush_staged || !d.enqueue || !d.flush_legacy || !d.sink || !d.drain)
        return false;
    if (p.hooks.strip_selection == StripSelection::All && !p.hooks.record_strip) return false;
    std::array<Body, kStripAddresses.size()> strips;
    for (size_t i = 0; i != strips.size(); ++i) {
        strips[i] = capture(kStripAddresses[i]);
        if (!strips[i]) return false;
    }
    auto link = capture(kLinkAddress);
    if (!link) return false;
    p.strips = strips;
    p.link = link;
    const auto rollback = [&](size_t count) {
        while (count != 0) {
            --count;
            if (!installer(kStripAddresses[count], strips[count]))
                Fault("Graphics dispatch rollback failed");
        }
    };
    for (size_t i = 0; i != strips.size(); ++i) {
        if (!installer(kStripAddresses[i], [this, i](CpuContext& c, uint32_t entry) {
                StripVariant(i, c, entry);
            })) {
            rollback(i);
            return false;
        }
    }
    if (!installer(kLinkAddress, [this](CpuContext& c, uint32_t entry) { Link(c, entry); })) {
        rollback(strips.size());
        return false;
    }
    p.installed = true;
    return true;
}

uint32_t GraphicsService::TryStrip(uint32_t producer, const CpuContext& context) {
    auto& p = *impl_;
    const auto& reader = p.hooks.read;
    MaterialState::Environment environment;
    if (!p.hooks.environment(environment)) return kEnvironment;
    const auto material_read = [&reader](uint32_t address, void* bytes, size_t size) {
        return Read(reader, address, bytes, size);
    };
    StripIntent::Output intent;
    const auto result = StripIntent::Capture(material_read, producer, context.cpuRegs.GPR.r[4].UL[0],
        context.cpuRegs.GPR.r[5].UL[0], context.cpuRegs.GPR.r[6].UL[0],environment,intent);
    if (result == StripIntent::Result::Empty && p.hooks.strip_selection == StripSelection::All) {
        ++p.counts.empty_producers;
        return 0; // source intent completed; no draw/submission completion credit
    }
    if (result != StripIntent::Result::Ok) return kCapture | uint32_t(result);
    if (p.hooks.strip_selection == StripSelection::All) {
        bool accepted = false;
        try { accepted = p.hooks.record_strip(std::move(intent)); }
        catch (...) { Fault("Graphics owned Strip capture failed", producer, kRecordRefused); }
        if (!accepted) return kRecordRefused;
        ++p.counts.owned_strip_producers;
        return 0;
    }
    if (!p.frame_open) {
        if (p.bridge.BeginFrame(p.frame) != GraphicsBridge::Result::Ok)
            Fault("Graphics frame recording could not start");
        p.frame_open = true;
    }
    if (p.bridge.AppendStrip(intent.bucket, reader, intent.draw) != GraphicsBridge::Result::Ok)
        return kRecordRefused;
    ++p.current_draws;
    return 0;
}

bool GraphicsService::RecordOwned(uint32_t bucket, std::function<bool()> work,
                                  const std::vector<Graphics::Lease>& leases,
                                  std::function<void()> cancel) {
    auto& p = *impl_;
    if (!p.installed || p.faulted || !work) return false;
    if (!p.frame_open) {
        if (p.bridge.BeginFrame(p.frame) != GraphicsBridge::Result::Ok)
            Fault("Graphics owned frame recording could not start");
        p.frame_open = true;
    }
    if (p.bridge.AppendWork(bucket, p.hooks.read, std::move(work), leases, std::move(cancel)) !=
        GraphicsBridge::Result::Ok)
        return false;
    ++p.current_draws; // outstanding source entries, including owned groups
    return true;
}

void GraphicsService::Strip(CpuContext& context, uint32_t entry) {
    StripVariant(0, context, entry);
}

void GraphicsService::StripVariant(size_t variant, CpuContext& context, uint32_t entry) {
    auto& p = *impl_;
    if (p.faulted || !p.strips[variant]) Fault("Graphics Strip has no valid continuation");
    auto& counts = p.counts.variants[variant];
    const auto model = [&] {
        ++counts.model_calls;
        ++p.counts.model_producers;
        p.strips[variant](context, entry);
    };
    const uint32_t producer = kStripAddresses[variant];
    const bool selected = p.hooks.strip_selection == StripSelection::All ||
        (variant == 0 && p.hooks.strip_selection == StripSelection::Plain);
    const auto refuse = [&](GraphicsRefusals::Reason reason, uint32_t detail) {
        if (selected && p.hooks.refuse) p.hooks.refuse(reason, producer, detail);
        model();
    };
    if (entry != 0 && entry != producer) {
        // Nothing of the owned arm has started at an interior label: the
        // original continuation finishes the producer (ordering refusal).
        ++p.counts.interior_fallback;
        ++counts.interior_fallback;
        refuse(GraphicsRefusals::Reason::Ordering, kInterior);
        return;
    }
    ++p.counts.strip_calls;
    ++counts.calls;
    const uint32_t refusal = selected ? TryStrip(producer, context) : kCapture;
    if (refusal == 0) {
        ++p.counts.replaced_producers;
        ++counts.replaced;
        return;
    }
    ++p.counts.fallback;
    refuse(refusal == kEnvironment || refusal == kRecordRefused ? GraphicsRefusals::Reason::Ordering
                                                                 : GraphicsRefusals::Reason::UnsupportedState,
           refusal);
}

void GraphicsService::Link(CpuContext& context, uint32_t entry) {
    auto& p = *impl_;
    if (p.faulted || !p.link) Fault("Graphics Link has no valid continuation");
    if (entry != 0 && entry != kLinkAddress) {
        ++p.counts.interior_fallback;
        p.link(context, entry);
        return;
    }
    uint32_t buckets = 0;
    if (!Physical(context.cpuRegs.GPR.r[4].UL[0], GraphicsBuckets::kObjectBytes, buckets) ||
        buckets != GraphicsBridge::kFrameBucketsAddress) {
        p.link(context, entry);
        return;
    }
    if (p.frame_open) {
        auto pending = std::make_shared<Impl::Pending>();
        if (p.current_draws != 0) {
            std::array<uint8_t, 4> root{};
            if (!Read(p.hooks.read, buckets + 8, root.data(), root.size()) ||
                !Physical(Word(root.data()), 16, pending->root) || (pending->root & 15))
                Fault("Graphics frame root is unavailable");
        }
        if (p.bridge.Seal(buckets, p.hooks.read, &pending->submission) != GraphicsBridge::Result::Ok)
            Fault("Graphics accepted producer lost its predecessor before Link");
        if (p.current_draws != 0) {
            // Add, never replace: D1_CHCR in the original Link may first flush
            // the PREVIOUS parked walk. StartBucket makes bucket 0.first at
            // the selected first chain buffer's start (TR:buckets.cpp:55,68,92).
            p.pending.push_back(std::move(pending));
            ++p.counts.linked_submissions;
        }
        p.frame_open = false;
        p.current_draws = 0;
    }
    if (p.hooks.linked_buckets) {
        std::array<uint32_t, Graphics::kBucketCount> firsts{};
        bool complete = true;
        for (size_t b = 0; b < firsts.size() && complete; ++b) {
            std::array<uint8_t, 4> word{};
            complete = Read(p.hooks.read, buckets + 8 + uint32_t(b) * 0x38, word.data(), word.size());
            firsts[b] = Word(word.data()) & 0x1fffffff;
        }
        if (complete) p.hooks.linked_buckets(firsts);
    }
    ++p.frame;
    p.link(context, entry);  // retains chain linking, kick and frame-buffer reset
}

void GraphicsService::OnTag(uint32_t address, bool from_scratchpad, bool first_tag) {
    auto& p = *impl_;
    if (p.faulted) Fault("Graphics boundary reached after an ordering failure");
    if (first_tag) {
        EndWalk();
        // A tail resume starts at its END slot, not a frame root. Do not match
        // a tag anywhere in that walk against another frame's accepted draws.
        if (!from_scratchpad) {
            for (const auto& pending : p.pending) {
                if (!pending->started && pending->root == address) {
                    pending->started = true;
                    p.walking = pending;
                    break;
                }
            }
        }
    }
    if (!p.walking || !p.bridge.WantsTag(p.walking->submission, address, from_scratchpad)) return;
    p.hooks.flush_staged();
    if (p.bridge.AfterTag(p.walking->submission, address, from_scratchpad) != GraphicsBridge::Result::Ok)
        Fault("Graphics ordered slot was not accepted");
}

void GraphicsService::EndWalk() {
    auto& p = *impl_;
    if (p.walking) {
        auto result = p.bridge.EndWalk(p.walking->submission);
        if (result == GraphicsBridge::Result::Unvisited) {
            // The kicked chain did not carry these predecessors: the title's
            // renderer "starts over" (ResetBuckets after WaitIdle at a movie
            // start/end) and drops the frame's appended buckets with them.
            // The groups are discarded as the title discarded their draws --
            // released, attributed, never a fault of the walk.
            p.bridge.Discard(p.walking->submission);
            ++p.counts.discarded_walks;
            if (p.hooks.refuse) p.hooks.refuse(GraphicsRefusals::Reason::Ordering, kLinkAddress, kDiscarded);
            result = p.bridge.EndWalk(p.walking->submission);
        }
        if (result != GraphicsBridge::Result::Ok)
            Fault("Graphics walk did not deliver every accepted predecessor");
        p.walking.reset();
    }
    for (auto it = p.pending.begin(); it != p.pending.end();) {
        const auto state = p.bridge.Poll((*it)->submission);
        if (state == GraphicsBridge::Result::DeliveryFailed)
            Fault("Graphics output delivery failed");
        if ((*it)->started && state == GraphicsBridge::Result::Ok) it = p.pending.erase(it);
        else ++it;
    }
}

bool GraphicsService::CanCheckpoint() const {
    const auto& p = *impl_;
    if (p.faulted || p.current_draws != 0) return false;
    for (const auto& pending : p.pending)
        if (p.bridge.Poll(pending->submission) != GraphicsBridge::Result::Ok) return false;
    return true;
}
bool GraphicsService::CanCheckpointUnlinked(GraphicsBridge::Recording* unlinked) const {
    const auto& p = *impl_;
    if (p.faulted) return false;
    for (const auto& pending : p.pending)
        if (p.bridge.Poll(pending->submission) != GraphicsBridge::Result::Ok) return false;
    const auto recording = p.bridge.OpenRecording();
    // Every outstanding source entry of the open frame is a recorded, unlinked
    // item of the bridge's open recording; anything else is not settled.
    if (recording.items != p.current_draws) return false;
    if (unlinked) *unlinked = recording;
    return true;
}
uint32_t GraphicsService::NextFrame() const { return impl_->frame; }
bool GraphicsService::RestoreFrame(uint32_t next) {
    if (!CanCheckpoint()) return false;
    ResetAfterQuiescence();
    impl_->frame = next;
    return true;
}

void GraphicsService::ResetAfterQuiescence() {
    auto& p = *impl_;
    p.walking.reset();
    p.pending.clear();
    p.bridge.Reset();
    p.frame_open = false;
    p.current_draws = 0;
    p.faulted = false;
    p.frame = 0;
}

GraphicsService::Counters GraphicsService::Counts() const {
    const auto& p = *impl_;
    auto out = p.counts;
    out.unsubmitted = p.current_draws;
    for (const auto& pending : p.pending)
        if (p.bridge.Poll(pending->submission) != GraphicsBridge::Result::Ok)
            ++out.pending_submissions;
    out.output = p.bridge.Counts();
    return out;
}

void GraphicsService::Fault(const char* message, uint32_t producer, uint32_t detail) {
    impl_->faulted = true;
    ++impl_->counts.ordering_errors;
    if (impl_->hooks.fault) impl_->hooks.fault(GraphicsRefusals::Reason::Ordering, producer, detail);
    throw std::runtime_error(message);
}

}  // namespace title
