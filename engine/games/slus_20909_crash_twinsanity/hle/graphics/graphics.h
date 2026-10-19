#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "engine/gs/hle/ir/draw_ir_sink.h"
#include "engine/render/backend.h"

namespace title {

// Producer-thread staging for title bucket order. Recording never invokes a
// sink. The adapter supplies safe, already-delimited legacy spans; this class
// does not read guest packets or implement hardware transport.
class Graphics {
public:
    static constexpr uint32_t kBucketCount = 28;
    enum class Result {
        Ok, InvalidState, Busy, InvalidAnchor, InvalidResource, InvalidIntent,
        LimitExceeded, MissingAdapter, DispatchFailed
    };
    enum class State { Idle, Recording, Submitting, Faulted };

    struct Limits {
        size_t entries = 65536;
        size_t payload_bytes = 64 * 1024 * 1024;
        size_t resources = 4096;
    };
    struct Counters {
        uint64_t submissions = 0;
        uint64_t draws = 0;
        uint64_t uploads = 0;
        uint64_t legacy_spans = 0;
        // Completed draw/upload intents per bucket, not whole retired buckets.
        // Recorded-but-unsubmitted, failed and fallback work earns no credit.
        std::array<uint64_t, kBucketCount> replaced_bucket_jobs{};
    };

    // Opaque positions cannot be used in another frame or Graphics instance.
    // Inserting repeatedly before one anchor preserves insertion call order.
    class Anchor {
    public:
        Anchor() = default;
    private:
        friend class Graphics;
        std::weak_ptr<const void> frame_;
        uint32_t bucket_ = 0;
        uint64_t entry_ = 0;
    };

    class Handle {
    public:
        Handle() = default;
    private:
        friend class Graphics;
        std::weak_ptr<const void> registry_;
        uint64_t id_ = 0;
    };

    struct Resource {
        uint64_t id = 0;
        ps2r::render::SurfaceDesc description;
        // Owns the adapter's allocation/cache lease, not a borrowed guest
        // pointer. Its deleter must obey the renderer's context ownership.
        std::shared_ptr<const void> lifetime;
    };
    using Lease = std::shared_ptr<const Resource>;

    struct Upload {
        Lease resource;
        ps2r::render::Rect rectangle;
        size_t stride = 0;
        std::vector<uint8_t> pixels;  // owned RGBA8, top row first
        uint32_t frame = 0;
    };

    struct LegacySpan {
        uint64_t token = 0;  // adapter identity; zero is permitted
        // Retains the prepared span and all referenced buffers through Drain.
        // The adapter must snapshot or pin storage against guest reuse.
        std::shared_ptr<const void> lifetime;
    };

    struct Hooks {
        ps2r::gs::DrawIrSink* sink = nullptr;
        // Complete typed draw delivery with explicit resource bindings. When
        // supplied this owns native draws; sink is needed only by legacy spans.
        // False/throwing means failed submission, never fallback/replay.
        std::function<bool(const ps2r::gs::DrawIr&, const std::vector<Lease>&)> draw;
        // Produce this span's existing work into the SAME sink. Neither hook
        // may defer producer-side packet reads until after returning.
        std::function<bool(const LegacySpan&, ps2r::gs::DrawIrSink&)> legacy;
        // Finish legacy vertex assembly and publish pending upload payloads
        // before another ordered item is submitted.
        std::function<bool(ps2r::gs::DrawIrSink&)> flush_legacy;
        // Enqueue an explicit resource upload on the render owner. No fake
        // UploadVRAMTexture JobRec or producer-thread Backend call is made here.
        std::function<bool(const Upload&)> upload;
        // Fence all previously submitted work, including upload payloads.
        // True means queued consumers no longer borrow any submitted inputs.
        std::function<bool()> drain;
    };

    Graphics();
    explicit Graphics(Limits limits);
    // A Faulted submission MUST be recovered/drained before destruction.
    // Terminating on that contract violation avoids releasing in-flight leases.
    ~Graphics();
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    Result BeginFrame(uint32_t frame);
    Anchor End(uint32_t bucket) const;
    Result InsertAnchor(const Anchor& before, Anchor* inserted);
    Result RecordDraw(const Anchor& before, const ps2r::gs::DrawIr& draw,
                      const std::vector<Handle>& resources = {},
                      Anchor* inserted = nullptr);
    Result RecordUpload(const Anchor& before, const Handle& resource,
                        ps2r::render::Rect rectangle, size_t stride,
                        const std::vector<uint8_t>& pixels,
                        Anchor* inserted = nullptr);
    Result RecordLegacy(const Anchor& before, const LegacySpan& span,
                        Anchor* inserted = nullptr);

    // Resource handles are registry-local. Releasing a handle prevents further
    // recording with it; already-recorded work retains its allocation lease.
    // Draw resources are retained through Drain and passed to Hooks::draw.
    // The producer/renderer contract defines their semantic binding order.
    Result RegisterResource(ps2r::render::SurfaceDesc description,
                            std::shared_ptr<const void> lifetime, Handle* handle);
    Result ReleaseResource(const Handle& handle);
    // Retain an explicit binding for a named target or another consumer.
    // Invalid/stale/foreign handles leave output unchanged.
    Result RetainResource(const Handle& handle, Lease* output) const;

    // Preflight required adapters before any side effect. Visits buckets in
    // order and each bucket's actual anchor order. Flush/drain every legacy
    // crossing; drain before/after uploads and at the end. No frame/present event
    // is invented. The parent remains responsible for presentation and waits.
    Result Submit(const Hooks& hooks);
    // Failed/throwing adapters leave a Faulted frame pinned and non-replayable.
    // Recover only flushes outstanding legacy work and drains; never replays it.
    Result Recover(const Hooks& hooks);
    // Drops unsubmitted work/resources and invalidates anchors/handles.
    // Refused during submission or while failed work still needs recovery.
    Result Reset();
    State CurrentState() const;
    size_t PendingEntries() const;
    size_t PayloadBytes() const;
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Result CheckAnchor(const Anchor& before) const;
    Lease FindResource(const Handle& handle) const;
    void SetAnchor(uint32_t bucket, uint64_t entry, Anchor* out) const;
};

}  // namespace title
