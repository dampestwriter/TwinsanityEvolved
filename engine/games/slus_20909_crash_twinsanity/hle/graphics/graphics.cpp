#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

#include <array>
#include <exception>
#include <limits>
#include <list>
#include <unordered_map>
#include <utility>
#include <variant>

namespace title {

// Specification: TR:src/platform/ps2/renderer/buckets.cpp:205 submits buckets
// in index order. TR:src/platform/ps2/renderer/renderer.h:1339,1347,1355
// distinguishes append from repeated insertion before a writer's tail.
// Source commit: 38d533d0033b48b5d6b49880c49f5eeeade0f38e (PAL specification).
// NTSC LinkRenderBuckets candidate: 0x0017ed50; Strip::Draw: 0x001a9120.
// Neither entry is bound by this class.
struct Graphics::Impl {
    struct Draw {
        ps2r::gs::DrawIr ir;
        std::vector<Lease> resources;
    };
    using Intent = std::variant<std::monostate, Draw, Upload, LegacySpan>;
    struct Entry {
        uint64_t id;
        Intent intent;
    };
    using Entries = std::list<Entry>;
    struct Location {
        uint32_t bucket;
        Entries::iterator entry;
    };

    Limits limits;
    State state = State::Idle;
    uint32_t frame_number = 0;
    std::shared_ptr<const void> frame;
    std::shared_ptr<const void> registry = std::make_shared<const uint8_t>(0);
    std::array<Entries, kBucketCount> buckets;
    std::unordered_map<uint64_t, Location> locations;
    std::unordered_map<uint64_t, Lease> resources;
    uint64_t next_entry = 1, next_resource = 1;
    size_t bytes = 0;
    bool legacy_dirty = false;
    Counters counters;

    explicit Impl(Limits value) : limits(value) {}

    Result MutableState() const {
        if (state == State::Submitting) return Result::Busy;
        return state == State::Faulted ? Result::InvalidState : Result::Ok;
    }

    bool Fits(size_t payload) const {
        return locations.size() < limits.entries &&
               payload <= limits.payload_bytes - bytes && next_entry != 0;
    }

    uint64_t Insert(uint32_t bucket, uint64_t before, Intent intent, size_t payload) {
        auto& entries = buckets[bucket];
        auto at = before == 0 ? entries.end() : locations.at(before).entry;
        const uint64_t id = next_entry;
        auto added = entries.insert(at, Entry{id, std::move(intent)});
        try {
            locations.emplace(id, Location{bucket, added});
        } catch (...) {
            entries.erase(added);
            throw;
        }
        ++next_entry;
        bytes += payload;
        return id;
    }

    void ClearFrame() {
        locations.clear();
        for (auto& bucket : buckets) bucket.clear();
        frame.reset();
        bytes = 0;
        legacy_dirty = false;
        state = State::Idle;
    }
};

Graphics::Graphics() : Graphics(Limits{}) {}
Graphics::Graphics(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
Graphics::~Graphics() {
    if (impl_->state == State::Faulted || impl_->state == State::Submitting)
        std::terminate();
}

Graphics::Result Graphics::BeginFrame(uint32_t frame) {
    if (impl_->state == State::Submitting) return Result::Busy;
    if (impl_->state != State::Idle) return Result::InvalidState;
    impl_->frame = std::make_shared<const uint8_t>(0);
    impl_->frame_number = frame;
    impl_->state = State::Recording;
    return Result::Ok;
}

Graphics::Anchor Graphics::End(uint32_t bucket) const {
    Anchor result;
    if (impl_->state == State::Recording && bucket < kBucketCount)
        SetAnchor(bucket, 0, &result);
    return result;
}

void Graphics::SetAnchor(uint32_t bucket, uint64_t entry, Anchor* out) const {
    if (!out) return;
    out->frame_ = impl_->frame;
    out->bucket_ = bucket;
    out->entry_ = entry;
}

Graphics::Result Graphics::CheckAnchor(const Anchor& before) const {
    if (impl_->state == State::Submitting) return Result::Busy;
    if (impl_->state != State::Recording) return Result::InvalidState;
    if (before.frame_.lock() != impl_->frame || before.bucket_ >= kBucketCount)
        return Result::InvalidAnchor;
    if (before.entry_ != 0) {
        const auto it = impl_->locations.find(before.entry_);
        if (it == impl_->locations.end() || it->second.bucket != before.bucket_)
            return Result::InvalidAnchor;
    }
    return Result::Ok;
}

Graphics::Result Graphics::InsertAnchor(const Anchor& before, Anchor* inserted) {
    const auto check = CheckAnchor(before);
    if (check != Result::Ok) return check;
    if (!inserted) return Result::InvalidIntent;
    if (!impl_->Fits(0)) return Result::LimitExceeded;
    const auto id = impl_->Insert(before.bucket_, before.entry_, std::monostate{}, 0);
    SetAnchor(before.bucket_, id, inserted);
    return Result::Ok;
}

Graphics::Lease Graphics::FindResource(const Handle& handle) const {
    if (handle.registry_.lock() != impl_->registry) return {};
    const auto it = impl_->resources.find(handle.id_);
    return it == impl_->resources.end() ? Lease{} : it->second;
}

Graphics::Result Graphics::RegisterResource(ps2r::render::SurfaceDesc description,
                                            std::shared_ptr<const void> lifetime,
                                            Handle* handle) {
    const auto state = impl_->MutableState();
    if (state != Result::Ok) return state;
    constexpr auto maximum = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    if (!handle || !lifetime || description.width == 0 || description.height == 0 ||
        description.width > maximum || description.height > maximum || description.scale == 0)
        return Result::InvalidResource;
    if (impl_->resources.size() >= impl_->limits.resources || impl_->next_resource == 0)
        return Result::LimitExceeded;
    const auto id = impl_->next_resource;
    auto resource = std::make_shared<const Resource>(Resource{id, description, std::move(lifetime)});
    impl_->resources.emplace(id, std::move(resource));
    ++impl_->next_resource;
    handle->registry_ = impl_->registry;
    handle->id_ = id;
    return Result::Ok;
}

Graphics::Result Graphics::ReleaseResource(const Handle& handle) {
    const auto state = impl_->MutableState();
    if (state != Result::Ok) return state;
    if (!FindResource(handle)) return Result::InvalidResource;
    impl_->resources.erase(handle.id_);
    return Result::Ok;
}

Graphics::Result Graphics::RetainResource(const Handle& handle, Lease* output) const {
    if (!output) return Result::InvalidResource;
    auto resource = FindResource(handle);
    if (!resource) return Result::InvalidResource;
    *output = std::move(resource);
    return Result::Ok;
}

Graphics::Result Graphics::RecordDraw(const Anchor& before, const ps2r::gs::DrawIr& draw,
                                      const std::vector<Handle>& resources, Anchor* inserted) {
    const auto check = CheckAnchor(before);
    if (check != Result::Ok) return check;
    // Existing DrawIr consumers accept expanded triangles, not strip vertices.
    if (draw.vertex_count != draw.verts.size() || draw.verts.size() % 3 != 0)
        return Result::InvalidIntent;
    if (draw.verts.size() > impl_->limits.payload_bytes / sizeof(ps2r::gs::VertexRec))
        return Result::LimitExceeded;
    const size_t bytes = draw.verts.size() * sizeof(ps2r::gs::VertexRec);
    if (!impl_->Fits(bytes) || resources.size() > impl_->limits.resources)
        return Result::LimitExceeded;
    Impl::Draw owned;
    owned.resources.reserve(resources.size());
    for (const auto& handle : resources) {
        auto lease = FindResource(handle);
        if (!lease) return Result::InvalidResource;
        owned.resources.push_back(std::move(lease));
    }
    owned.ir = draw;
    owned.ir.src_frame = impl_->frame_number;
    const auto id = impl_->Insert(before.bucket_, before.entry_, std::move(owned), bytes);
    SetAnchor(before.bucket_, id, inserted);
    return Result::Ok;
}

Graphics::Result Graphics::RecordUpload(const Anchor& before, const Handle& resource,
                                        ps2r::render::Rect rectangle, size_t stride,
                                        const std::vector<uint8_t>& pixels, Anchor* inserted) {
    const auto check = CheckAnchor(before);
    if (check != Result::Ok) return check;
    auto lease = FindResource(resource);
    if (!lease) return Result::InvalidResource;
    const auto& desc = lease->description;
    if (desc.format != ps2r::render::SurfaceFormat::Rgba8 ||
        rectangle.x < 0 || rectangle.y < 0 || rectangle.w <= 0 || rectangle.h <= 0)
        return Result::InvalidIntent;
    const auto x = static_cast<uint32_t>(rectangle.x), y = static_cast<uint32_t>(rectangle.y);
    const auto w = static_cast<uint32_t>(rectangle.w), h = static_cast<uint32_t>(rectangle.h);
    if (x > desc.width || w > desc.width - x || y > desc.height || h > desc.height - y)
        return Result::InvalidIntent;
    if (std::numeric_limits<size_t>::max() / size_t(w) < 4) return Result::InvalidIntent;
    const size_t row_bytes = size_t(w) * 4;
    if (stride < row_bytes ||
        size_t(h - 1) > (std::numeric_limits<size_t>::max() - row_bytes) / stride)
        return Result::InvalidIntent;
    const size_t bytes = size_t(h - 1) * stride + row_bytes;
    if (pixels.size() < bytes) return Result::InvalidIntent;
    if (!impl_->Fits(bytes)) return Result::LimitExceeded;
    Upload owned{std::move(lease), rectangle, stride, {}, impl_->frame_number};
    owned.pixels.assign(pixels.begin(), pixels.begin() + bytes);
    const auto id = impl_->Insert(before.bucket_, before.entry_, std::move(owned), bytes);
    SetAnchor(before.bucket_, id, inserted);
    return Result::Ok;
}

Graphics::Result Graphics::RecordLegacy(const Anchor& before, const LegacySpan& span,
                                        Anchor* inserted) {
    const auto check = CheckAnchor(before);
    if (check != Result::Ok) return check;
    if (!span.lifetime) return Result::InvalidIntent;
    if (!impl_->Fits(0)) return Result::LimitExceeded;
    const auto id = impl_->Insert(before.bucket_, before.entry_, span, 0);
    SetAnchor(before.bucket_, id, inserted);
    return Result::Ok;
}

Graphics::Result Graphics::Submit(const Hooks& hooks) {
    if (impl_->state == State::Submitting) return Result::Busy;
    if (impl_->state != State::Recording) return Result::InvalidState;
    if (!hooks.drain) return Result::MissingAdapter;
    for (const auto& bucket : impl_->buckets) {
        for (const auto& entry : bucket) {
            const auto& intent = entry.intent;
            if (std::holds_alternative<Impl::Draw>(intent) && !hooks.sink && !hooks.draw)
                return Result::MissingAdapter;
            if (std::holds_alternative<Upload>(intent) && !hooks.upload)
                return Result::MissingAdapter;
            if (std::holds_alternative<LegacySpan>(intent) &&
                (!hooks.sink || !hooks.legacy || !hooks.flush_legacy))
                return Result::MissingAdapter;
        }
    }
    impl_->state = State::Submitting;
    try {
        for (const auto& bucket : impl_->buckets) {
            for (const auto& entry : bucket) {
                if (const auto* draw = std::get_if<Impl::Draw>(&entry.intent)) {
                    if (hooks.draw) {
                        if (!hooks.draw(draw->ir, draw->resources)) {
                            impl_->state = State::Faulted;
                            return Result::DispatchFailed;
                        }
                    } else {
                        hooks.sink->OnDraw(draw->ir);
                    }
                } else if (const auto* upload = std::get_if<Upload>(&entry.intent)) {
                    if (!hooks.drain() || !hooks.upload(*upload) || !hooks.drain()) {
                        impl_->state = State::Faulted;
                        return Result::DispatchFailed;
                    }
                } else if (const auto* span = std::get_if<LegacySpan>(&entry.intent)) {
                    impl_->legacy_dirty = true;
                    if (!hooks.legacy(*span, *hooks.sink) || !hooks.flush_legacy(*hooks.sink)) {
                        impl_->state = State::Faulted;
                        return Result::DispatchFailed;
                    }
                    impl_->legacy_dirty = false;
                    if (!hooks.drain()) {
                        impl_->state = State::Faulted;
                        return Result::DispatchFailed;
                    }
                }
            }
        }
        if (!hooks.drain()) {
            impl_->state = State::Faulted;
            return Result::DispatchFailed;
        }
    } catch (...) {
        impl_->state = State::Faulted;
        return Result::DispatchFailed;
    }
    // Commit accounting only after the complete ordered stream was drained.
    ++impl_->counters.submissions;
    for (size_t index = 0; index < impl_->buckets.size(); ++index) {
        for (const auto& entry : impl_->buckets[index]) {
            if (std::holds_alternative<Impl::Draw>(entry.intent)) {
                ++impl_->counters.draws;
                ++impl_->counters.replaced_bucket_jobs[index];
            } else if (std::holds_alternative<Upload>(entry.intent)) {
                ++impl_->counters.uploads;
                ++impl_->counters.replaced_bucket_jobs[index];
            } else if (std::holds_alternative<LegacySpan>(entry.intent)) {
                ++impl_->counters.legacy_spans;
            }
        }
    }
    impl_->ClearFrame();
    return Result::Ok;
}

Graphics::Result Graphics::Recover(const Hooks& hooks) {
    if (impl_->state == State::Submitting) return Result::Busy;
    if (impl_->state != State::Faulted) return Result::InvalidState;
    if (!hooks.drain || (impl_->legacy_dirty && (!hooks.sink || !hooks.flush_legacy)))
        return Result::MissingAdapter;
    impl_->state = State::Submitting;
    try {
        if (impl_->legacy_dirty) {
            if (!hooks.flush_legacy(*hooks.sink)) {
                impl_->state = State::Faulted;
                return Result::DispatchFailed;
            }
            impl_->legacy_dirty = false;
        }
        if (!hooks.drain()) {
            impl_->state = State::Faulted;
            return Result::DispatchFailed;
        }
    } catch (...) {
        impl_->state = State::Faulted;
        return Result::DispatchFailed;
    }
    impl_->ClearFrame();
    return Result::Ok;
}

Graphics::Result Graphics::Reset() {
    const auto state = impl_->MutableState();
    if (state != Result::Ok) return state;
    auto next_registry = std::make_shared<const uint8_t>(0);
    impl_->ClearFrame();
    impl_->resources.clear();
    impl_->registry = std::move(next_registry);
    return Result::Ok;
}

Graphics::State Graphics::CurrentState() const { return impl_->state; }
size_t Graphics::PendingEntries() const { return impl_->locations.size(); }
size_t Graphics::PayloadBytes() const { return impl_->bytes; }
Graphics::Counters Graphics::Counts() const { return impl_->counters; }

}  // namespace title
