#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace title {
namespace {
using Desc = ps2r::render::SurfaceDesc;
using Format = ps2r::render::SurfaceFormat;
bool Same(const Desc& a, const Desc& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.renderable == b.renderable && a.scale == b.scale;
}
bool Valid(const Desc& d, const GraphicsResources::Limits& limits) {
    if (!d.width || !d.height || d.width > limits.max_dimension ||
        d.height > limits.max_dimension || uint64_t(d.width) * d.height > limits.max_pixels ||
        !d.scale || d.width % d.scale || d.height % d.scale)
        return false;
    return d.format == Format::Rgba8 || d.format == Format::Depth32f ||
           d.format == Format::R8 || d.format == Format::DepthStencil24_8;
}
bool ValidUpload(const Graphics::Upload& upload, const Desc& d) {
    const auto& r = upload.rectangle;
    if (d.format != Format::Rgba8 || r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
        uint64_t(r.x) + uint32_t(r.w) > d.width ||
        uint64_t(r.y) + uint32_t(r.h) > d.height)
        return false;
    const uint64_t row = uint64_t(r.w) * 4;
    if (upload.stride < row || upload.pixels.size() < row) return false;
    // Division checks the multiplication before performing it, including
    // hostile SIZE_MAX strides and a one-row upload without trailing padding.
    return uint64_t(r.h - 1) <= (upload.pixels.size() - row) / upload.stride;
}
}  // namespace

struct GraphicsResources::Impl {
    struct Binding { Desc description; };
    struct Entry {
        std::shared_ptr<const Binding> binding;
        Desc description;
        ps2r::render::SurfaceId surface;
        // Held here, not by Binding: the last producer-side binding release
        // must never run an external allocation's deleter off the context owner.
        std::shared_ptr<const void> external_owner;
        bool failed = false;
    };
    ps2r::render::Backend& backend;
    Limits limits;
    mutable std::mutex mutex;
    // A token's address identifies a registered allocation version, not guest
    // memory. The owner retains the token until fenced collection, preventing
    // its address being recycled while an old surface exists. Backend calls
    // always happen with mutex released.
    std::unordered_map<const void*, std::shared_ptr<Entry>> entries;
    std::atomic<uint64_t> registered{0}, created{0}, uploads{0}, destroyed{0};
    std::atomic<uint64_t> uploaded_bytes{0}, failures{0};
    std::atomic<size_t> live_surfaces{0};

    Impl(ps2r::render::Backend& value, Limits cap) : backend(value), limits(cap) {}

    std::shared_ptr<Entry> Find(const Graphics::Lease& resource) const {
        if (!resource || !resource->lifetime) return {};
        std::lock_guard<std::mutex> guard(mutex);
        const auto at = entries.find(resource->lifetime.get());
        if (at == entries.end()) return {};
        const auto& binding = at->second->binding;
        if (binding.get() != resource->lifetime.get() ||
            binding.owner_before(resource->lifetime) || resource->lifetime.owner_before(binding) ||
            !Same(binding->description, resource->description))
            return {};
        return at->second;
    }
    Result Ensure(Entry& entry) {
        if (entry.failed) return Result::BackendFailed;
        if (entry.surface.id) return Result::Ok;
        try {
            const int maximum = backend.QueryCaps().max_texture_size;
            if (maximum <= 0 || entry.description.width > uint32_t(maximum) ||
                entry.description.height > uint32_t(maximum)) {
                entry.failed = true;
                ++failures;
                return Result::BackendFailed;
            }
            entry.surface = backend.CreateSurface(entry.description);
            if (!entry.surface.id) {
                entry.failed = true;
                ++failures;
                return Result::BackendFailed;
            }
            ++created;
            ++live_surfaces;
            return Result::Ok;
        } catch (...) {
            entry.failed = true;
            ++failures;
            return Result::BackendFailed;
        }
    }
};

GraphicsResources::GraphicsResources(ps2r::render::Backend& backend)
    : GraphicsResources(backend, Limits{}) {}
GraphicsResources::GraphicsResources(ps2r::render::Backend& backend, Limits limits)
    : impl_(new Impl(backend, limits)) {}
GraphicsResources::~GraphicsResources() {
    // The explicit quiescence contract also covers failed allocations.
    for (const auto& pair : impl_->entries) {
        if (pair.second->surface.id && !pair.second->external_owner) {
            try { impl_->backend.DestroySurface(pair.second->surface); }
            catch (...) { std::terminate(); }
        }
    }
}

GraphicsResources::Result GraphicsResources::Register(
    Graphics& queue, const Desc& description, Graphics::Handle* handle) {
    auto& p = *impl_;
    if (!handle || !Valid(description, p.limits)) return Result::InvalidDescription;
    const auto binding = std::make_shared<const Impl::Binding>(Impl::Binding{description});
    const auto entry = std::make_shared<Impl::Entry>();
    entry->binding = binding;
    entry->description = description;
    {
        std::lock_guard<std::mutex> guard(p.mutex);
        if (p.entries.size() >= p.limits.resources) return Result::LimitExceeded;
        p.entries.emplace(binding.get(), entry);
    }
    // Queue registration is a producer operation and does not call the backend.
    // Do not hold the registry lock across another owner's operation.
    const auto result = queue.RegisterResource(description, binding, handle);
    if (result != Graphics::Result::Ok) {
        std::lock_guard<std::mutex> guard(p.mutex);
        p.entries.erase(binding.get());
        return Result::QueueRejected;
    }
    ++p.registered;
    return Result::Ok;
}

GraphicsResources::Result GraphicsResources::RegisterExisting(
    Graphics& queue, const Desc& description, ps2r::render::SurfaceId surface,
    std::shared_ptr<const void> lifetime, Graphics::Handle* handle) {
    auto& p = *impl_;
    if (!handle || !Valid(description, p.limits)) return Result::InvalidDescription;
    if (!surface.id || !lifetime) return Result::InvalidResource;
    try {
        if (!Same(p.backend.DescribeSurface(surface), description)) return Result::InvalidResource;
    } catch (...) {
        ++p.failures;
        return Result::BackendFailed;
    }
    std::shared_ptr<Impl::Entry> entry;
    std::shared_ptr<const Impl::Binding> binding;
    bool inserted = false;
    {
        std::lock_guard<std::mutex> guard(p.mutex);
        // Backend allocations, imports and collection share one context owner;
        // producer Register never reads or writes an existing entry's surface.
        for (const auto& pair : p.entries) {
            const auto& existing = pair.second;
            if (existing->surface.id != surface.id) continue;
            if (!existing->external_owner || !Same(existing->description, description) ||
                existing->external_owner.owner_before(lifetime) ||
                lifetime.owner_before(existing->external_owner))
                return Result::InvalidResource;
            if (existing->failed) return Result::BackendFailed;
            entry = existing;
            break;
        }
        if (!entry) {
            if (p.entries.size() >= p.limits.resources) return Result::LimitExceeded;
            entry = std::make_shared<Impl::Entry>();
            entry->binding = std::make_shared<const Impl::Binding>(Impl::Binding{description});
            entry->description = description;
            entry->surface = surface;
            entry->external_owner = std::move(lifetime);
            p.entries.emplace(entry->binding.get(), entry);
            inserted = true;
        }
        binding = entry->binding;
    }
    const auto result = queue.RegisterResource(description, binding, handle);
    if (result != Graphics::Result::Ok) {
        if (inserted) {
            std::lock_guard<std::mutex> guard(p.mutex);
            p.entries.erase(binding.get());
        }
        return Result::QueueRejected;
    }
    if (inserted) ++p.live_surfaces;
    ++p.registered;
    return Result::Ok;
}

GraphicsResources::Result GraphicsResources::Upload(const Graphics::Upload& upload) {
    auto& p = *impl_;
    const auto entry = p.Find(upload.resource);
    if (!entry) return Result::InvalidResource;
    if (!ValidUpload(upload, entry->description)) return Result::InvalidUpload;
    const auto ready = p.Ensure(*entry);
    if (ready != Result::Ok) return ready;
    try {
        p.backend.Upload(entry->surface, upload.rectangle, upload.pixels.data(), upload.stride);
    } catch (...) {
        entry->failed = true;
        ++p.failures;
        return Result::BackendFailed;
    }
    ++p.uploads;
    p.uploaded_bytes += uint64_t(upload.rectangle.w) * upload.rectangle.h * 4;
    return Result::Ok;
}

GraphicsResources::Result GraphicsResources::Resolve(
    const Graphics::Lease& resource, ps2r::render::SurfaceId* surface) {
    if (!surface) return Result::InvalidResource;
    const auto entry = impl_->Find(resource);
    if (!entry) return Result::InvalidResource;
    const auto ready = impl_->Ensure(*entry);
    if (ready == Result::Ok) *surface = entry->surface;
    return ready;
}

GraphicsResources::Result GraphicsResources::CollectAfterQuiescence() {
    auto& p = *impl_;
    for (;;) {
        std::shared_ptr<Impl::Entry> expired;
        {
            std::lock_guard<std::mutex> guard(p.mutex);
            for (auto it = p.entries.begin(); it != p.entries.end(); ++it) {
                if (it->second->binding.use_count() == 1) {
                    expired = it->second;
                    break;
                }
            }
        }
        if (!expired) return Result::Ok;
        if (expired->surface.id) {
            if (!expired->external_owner) {
                try { p.backend.DestroySurface(expired->surface); }
                catch (...) {
                    // Retain bookkeeping; the caller must restore backend service
                    // before attempting collection again.
                    ++p.failures;
                    return Result::BackendFailed;
                }
                ++p.destroyed;
            }
            --p.live_surfaces;
        }
        {
            std::lock_guard<std::mutex> guard(p.mutex);
            p.entries.erase(expired->binding.get());
        }
    }
}

GraphicsResources::Counters GraphicsResources::Counts() const {
    const auto& p = *impl_;
    Counters out;
    out.registered = p.registered;
    out.created = p.created;
    out.uploads = p.uploads;
    out.destroyed = p.destroyed;
    out.uploaded_bytes = p.uploaded_bytes;
    out.failures = p.failures;
    out.live_surfaces = p.live_surfaces;
    {
        std::lock_guard<std::mutex> guard(p.mutex);
        out.live_bindings = p.entries.size();
    }
    return out;
}

}  // namespace title
