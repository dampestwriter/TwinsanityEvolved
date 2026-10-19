#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_dump.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pack.h"

namespace title {
struct TextureAssets::Impl {
    struct Entry {
        Token token;
        uint32_t asset_id = 0;
        Graphics::Handle handle;
        std::weak_ptr<const Graphics::Resource> lease;
        std::shared_ptr<const TextureCodec::Image> image;
        uint64_t bytes = 0;
        bool failed = false;
    };
    Graphics& queue;
    GraphicsResources& resources;
    ps2r::render::Backend& backend;
    Limits limits;
    mutable std::mutex mutex;
    std::unordered_map<uint32_t, std::shared_ptr<Entry>> current;
    std::unordered_map<const Graphics::Resource*, std::shared_ptr<Entry>> versions;
    uint64_t next_generation = 1;
    bool restored = false;
    Counters counts;
    TexturePack* pack = nullptr;
    TextureDump* dump = nullptr;
    uint64_t pack_replaced = 0;

    Impl(Graphics& q, GraphicsResources& r, ps2r::render::Backend& b, Limits l)
        : queue(q), resources(r), backend(b), limits(l) {}
    static View ViewOf(const Entry& entry, Graphics::Lease lease) {
        return {entry.token, entry.asset_id, entry.handle, std::move(lease), entry.image};
    }
    void Collect() {
        for (auto it = versions.begin(); it != versions.end();) {
            if (it->second->lease.expired()) {
                counts.retained_bytes -= it->second->bytes;
                it = versions.erase(it);
            } else ++it;
        }
    }
    // The pack's replacement for a converted image (by content hash); the
    // authored levels stay the identity, Pixels() becomes the pack's chain.
    void ApplyPack(TextureCodec::Image& image) {
        if (!pack) return;
        const auto replacement = pack->Lookup(image.content_hash);
        if (!replacement) return;
        if (TextureCodec::ApplyReplacement(image, replacement->width, replacement->height, replacement->rgba)) {
            std::lock_guard<std::mutex> lock(mutex);
            ++pack_replaced;
        }
    }
    static uint64_t BytesOf(const TextureCodec::Image& image) {
        uint64_t bytes = image.palette.size() * 4;
        for (const auto& level : image.levels) bytes += level.rgba.size();
        for (const auto& level : image.pack_levels) bytes += level.rgba.size();
        return bytes;
    }
    Result Failed(const std::shared_ptr<Entry>& entry) {
        std::lock_guard<std::mutex> lock(mutex);
        entry->failed = true;
        ++counts.failed;
        return Result::BackendFailed;
    }
};

TextureAssets::TextureAssets(Graphics& queue, GraphicsResources& resources, ps2r::render::Backend& backend)
    : TextureAssets(queue, resources, backend, Limits{}) {}
TextureAssets::TextureAssets(Graphics& queue, GraphicsResources& resources,
                             ps2r::render::Backend& backend, Limits limits)
    : impl_(new Impl(queue, resources, backend, limits)) {}
TextureAssets::~TextureAssets() = default;
void TextureAssets::SetPack(TexturePack* pack) { impl_->pack = pack; }
void TextureAssets::SetDump(TextureDump* dump) { impl_->dump = dump; }
TextureAssets::PackCounters TextureAssets::PackCounts() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->pack_replaced};
}

TextureAssets::Result TextureAssets::Load(uint32_t object, uint32_t asset_id, const uint8_t* record,
                                         size_t size, View& out, std::string* error) {
    if (!object || (object & 3)) return Result::InvalidObject;
    auto image = std::make_shared<TextureCodec::Image>();
    const auto decoded = TextureCodec::Decode(record, size, *image, error);
    if (decoded != TextureCodec::Result::Ok) {
        if (decoded == TextureCodec::Result::LimitExceeded) return Result::LimitExceeded;
        return decoded == TextureCodec::Result::InvalidAsset ? Result::InvalidAsset : Result::UnsupportedAsset;
    }
    auto& p = *impl_;
    if (p.dump) p.dump->Record(*image, asset_id);
    p.ApplyPack(*image);
    auto entry = std::make_shared<Impl::Entry>();
    entry->asset_id = asset_id;
    entry->image = std::move(image);
    entry->bytes = Impl::BytesOf(*entry->image);
    std::shared_ptr<Impl::Entry> previous;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.Collect();
        if (!p.next_generation || p.versions.size() >= p.limits.versions ||
            entry->bytes > p.limits.bytes || p.counts.retained_bytes > p.limits.bytes - entry->bytes)
            return Result::LimitExceeded;
        entry->token = {object, p.next_generation++};
        const auto at = p.current.find(object);
        if (at != p.current.end()) previous = at->second;
    }
    const auto& base = entry->image->Pixels()[0];
    if (p.resources.Register(p.queue, {base.width, base.height, ps2r::render::SurfaceFormat::Rgba8, false, 1},
                             &entry->handle) != GraphicsResources::Result::Ok) return Result::QueueRejected;
    Graphics::Lease lease;
    if (p.queue.RetainResource(entry->handle, &lease) != Graphics::Result::Ok ||
        (previous && p.queue.ReleaseResource(previous->handle) != Graphics::Result::Ok)) {
        p.queue.ReleaseResource(entry->handle);
        return Result::QueueRejected;
    }
    entry->lease = lease;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.current[object] = entry;
        p.versions.emplace(lease.get(), entry);
        p.counts.retained_bytes += entry->bytes;
        ++p.counts.loaded;
    }
    out = Impl::ViewOf(*entry, std::move(lease));
    if (error) error->clear();
    return Result::Ok;
}
TextureAssets::Result TextureAssets::Find(uint32_t object, View& out) const {
    const auto& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    const auto at = p.current.find(object);
    if (at == p.current.end()) return Result::MissingAsset;
    const auto& entry = *at->second;
    if (entry.failed) return Result::BackendFailed;
    auto lease = entry.lease.lock();
    if (!lease) return Result::StaleGeneration;
    out = Impl::ViewOf(entry, std::move(lease));
    return Result::Ok;
}
TextureAssets::Result TextureAssets::Find(Token token, View& out) const {
    View found;
    const auto result = Find(token.object, found);
    if (result != Result::Ok) return result;
    if (found.token.generation != token.generation) return Result::StaleGeneration;
    out = std::move(found);
    return Result::Ok;
}
TextureAssets::Result TextureAssets::Find(const Graphics::Lease& allocation, View& out) const {
    const auto& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    const auto at = p.versions.find(allocation.get());
    if (at == p.versions.end()) return Result::NotOwned;
    const auto& entry = *at->second;
    const auto lease = entry.lease.lock();
    if (!lease || lease.owner_before(allocation) || allocation.owner_before(lease))
        return Result::StaleGeneration;
    if (entry.failed) return Result::BackendFailed;
    out = Impl::ViewOf(entry, std::move(lease));
    return Result::Ok;
}
TextureAssets::Result TextureAssets::Lease(uint32_t asset_id, uint64_t generation, View& out) const {
    const auto& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    bool known = false;
    for (const auto& [resource, entry] : p.versions) {
        if (entry->asset_id != asset_id) continue;
        known = true;
        if (entry->token.generation != generation) continue;
        if (entry->failed) return Result::BackendFailed;
        auto lease = entry->lease.lock();
        if (!lease) return Result::StaleGeneration;
        out = Impl::ViewOf(*entry, std::move(lease));
        return Result::Ok;
    }
    return known ? Result::StaleGeneration : Result::MissingAsset;
}
TextureAssets::Result TextureAssets::Release(Token token) {
    auto& p = *impl_;
    std::shared_ptr<Impl::Entry> entry;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        const auto at = p.current.find(token.object);
        if (at == p.current.end()) return Result::MissingAsset;
        entry = at->second;
        if (entry->token.generation != token.generation) return Result::StaleGeneration;
    }
    if (p.queue.ReleaseResource(entry->handle) != Graphics::Result::Ok) return Result::QueueRejected;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.current.erase(token.object);
        ++p.counts.released;
    }
    return Result::Ok;
}
TextureAssets::Result TextureAssets::Reset() {
    for (;;) {
        Token token;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->current.empty()) return Result::Ok;
            token = impl_->current.begin()->second->token;
        }
        const auto result = Release(token);
        if (result != Result::Ok) return result;
    }
}
TextureAssets::Result TextureAssets::Record(const Graphics::Anchor& before, Token token) {
    View view;
    const auto found = Find(token, view);
    if (found != Result::Ok) return found;
    const auto& base = view.image->Pixels()[0];
    return impl_->queue.RecordUpload(before, view.handle, {0, 0, int32_t(base.width), int32_t(base.height)},
        size_t(base.width) * 4, base.rgba) == Graphics::Result::Ok ? Result::Ok : Result::QueueRejected;
}
TextureAssets::Result TextureAssets::Upload(const Graphics::Upload& upload, bool synthesize_tail) {
    auto& p = *impl_;
    std::shared_ptr<Impl::Entry> entry;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        const auto at = p.versions.find(upload.resource.get());
        if (at == p.versions.end()) return Result::NotOwned;
        entry = at->second;
        const auto lease = entry->lease.lock();
        if (!lease || lease.owner_before(upload.resource) || upload.resource.owner_before(lease))
            return Result::InvalidUpload;
        if (entry->failed) return Result::BackendFailed;
    }
    const auto& levels = entry->image->Pixels();
    const auto& base = levels[0];
    if (upload.rectangle.x || upload.rectangle.y ||
        upload.rectangle.w != int32_t(base.width) || upload.rectangle.h != int32_t(base.height) ||
        upload.stride != size_t(base.width) * 4 || upload.pixels != base.rgba) return Result::InvalidUpload;
    if (p.resources.Upload(upload) != GraphicsResources::Result::Ok) return p.Failed(entry);
    ps2r::render::SurfaceId surface;
    if (p.resources.Resolve(upload.resource, &surface) != GraphicsResources::Result::Ok) return p.Failed(entry);
    try {
        for (size_t i = 1; i < levels.size(); ++i)
            p.backend.UploadLevel(surface, int(i), int(levels[i].width), int(levels[i].height), levels[i].rgba.data());
        // The backend's `authored_levels` is the chain BEYOND the base (the index
        // of the last uploaded level; texsrc passes `chain`/`*levels` the same
        // way): GL_TEXTURE_MAX_LEVEL lands on it and the synthesized tail starts
        // from it. The count including the base left MAX_LEVEL one past the last
        // defined level -- a mip-incomplete texture that samples BLACK under a
        // mipmapped minification filter (MMIN=4); sprites and
        // text (no mip filter) never saw it.
        if (levels.size() > 1) p.backend.FinishMipChain(surface, int(levels.size() - 1), synthesize_tail);
    } catch (...) { return p.Failed(entry); }
    std::lock_guard<std::mutex> lock(p.mutex);
    ++p.counts.uploaded;
    return Result::Ok;
}
TextureAssets::Result TextureAssets::CollectAfterQuiescence() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->Collect();
    }
    return impl_->resources.CollectAfterQuiescence() == GraphicsResources::Result::Ok ?
        Result::Ok : Result::BackendFailed;
}
TextureAssets::Counters TextureAssets::Counts() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->counts;
    result.current = impl_->current.size();
    result.versions = impl_->versions.size();
    return result;
}

TextureAssets::Result TextureAssets::Capture(Snapshot& out, std::string* error, bool unlinked_recording_open) const {
    const auto& p = *impl_;
    const auto refuse = [&](Result result, const char* why) {
        if (error) *error = std::string("Texture capture: ") + why;
        return result;
    };
    if (p.queue.CurrentState() != Graphics::State::Idle || p.queue.PendingEntries())
        return refuse(Result::PendingWork, "ordered Graphics work has no checkpoint owner");
    Snapshot snapshot;
    std::lock_guard<std::mutex> lock(p.mutex);
    snapshot.next_generation = p.next_generation;
    for (const auto& pair : p.versions) {
        const auto& entry = *pair.second;
        if (entry.lease.expired()) continue;
        const auto current = p.current.find(entry.token.object);
        if (current == p.current.end() || current->second != pair.second)
            return refuse(Result::RetainedConsumer, "a retired generation is still retained");
    }
    for (const auto& pair : p.current) {
        const auto& entry = *pair.second;
        if (entry.failed) return refuse(Result::BackendFailed, "failed allocation cannot be laundered by restore");
        auto lease = entry.lease.lock();
        Graphics::Lease registered;
        if (!lease || p.queue.RetainResource(entry.handle, &registered) != Graphics::Result::Ok ||
            registered != lease || registered.owner_before(lease) || lease.owner_before(registered))
            return refuse(Result::StaleGeneration, "current allocation is not registered");
        registered.reset();
        // One queue registry lease and this local guard. Anything else belongs
        // to a producer/consumer that needs its own exact checkpoint rebinding
        // -- unless the boundary's settle attributed it to the open (unlinked)
        // recording, whose captured draws the image does not carry.
        if (lease.use_count() < 2 || (lease.use_count() != 2 && !unlinked_recording_open))
            return refuse(Result::RetainedConsumer, "external consumer lease requires checkpoint rebinding");
        // The AUTHORED image only: a texture pack is a run-time overlay the
        // restoring session re-applies from its own pack (never serialized).
        snapshot.assets.push_back({entry.token, entry.asset_id, *entry.image});
        snapshot.assets.back().image.pack_levels.clear();
    }
    std::sort(snapshot.assets.begin(), snapshot.assets.end(), [](const SavedAsset& a, const SavedAsset& b) {
        return a.token.generation < b.token.generation;
    });
    if (!ValidateSnapshot(snapshot, p.limits, error)) return Result::InvalidSnapshot;
    out = std::move(snapshot);
    return Result::Ok;
}

TextureAssets::Result TextureAssets::PreflightRestore(const Snapshot& snapshot, std::string* error) const {
    const auto& p = *impl_;
    if (!ValidateSnapshot(snapshot, p.limits, error)) return Result::InvalidSnapshot;
    if (p.queue.CurrentState() != Graphics::State::Idle || p.queue.PendingEntries()) {
        if (error) *error = "Texture restore: shared Graphics queue must be idle";
        return Result::PendingWork;
    }
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.current.empty() || !p.versions.empty() || p.next_generation != 1 || p.counts.loaded || p.restored) {
        if (error) *error = "Texture restore: requires a fresh owner; existing state is not reset";
        return Result::OwnerNotEmpty;
    }
    return Result::Ok;
}

TextureAssets::Result TextureAssets::Restore(const Snapshot& snapshot, std::vector<Token>& uploads,
                                             std::string* error) {
    const auto preflight = PreflightRestore(snapshot, error);
    if (preflight != Result::Ok) return preflight;
    auto& p = *impl_;
    decltype(p.current) current;
    decltype(p.versions) versions;
    std::vector<Graphics::Handle> handles;
    std::vector<Token> required;
    uint64_t bytes = 0;
    const auto rollback = [&] {
        for (const auto& handle : handles) {
            // Queue cannot change state during this serialized owner operation.
            if (p.queue.ReleaseResource(handle) != Graphics::Result::Ok) std::terminate();
        }
    };
    try {
        handles.reserve(snapshot.assets.size());
        required.reserve(snapshot.assets.size());
        for (const auto& saved : snapshot.assets) {
            auto entry = std::make_shared<Impl::Entry>();
            entry->token = saved.token;
            entry->asset_id = saved.asset_id;
            auto restored = std::make_shared<TextureCodec::Image>(saved.image);
            restored->content_hash = TextureCodec::ContentHash(*restored);
            p.ApplyPack(*restored);
            entry->image = std::move(restored);
            entry->bytes = Impl::BytesOf(*entry->image);
            const auto& base = entry->image->Pixels()[0];
            const auto result = p.resources.Register(p.queue,
                {base.width, base.height, ps2r::render::SurfaceFormat::Rgba8, false, 1}, &entry->handle);
            if (result != GraphicsResources::Result::Ok) {
                rollback();
                if (error) *error = "Texture restore: staged resource registration refused";
                return Result::QueueRejected;
            }
            handles.push_back(entry->handle);
            Graphics::Lease lease;
            if (p.queue.RetainResource(entry->handle, &lease) != Graphics::Result::Ok) {
                rollback();
                if (error) *error = "Texture restore: staged resource retain failed";
                return Result::QueueRejected;
            }
            entry->lease = lease;
            current.emplace(saved.token.object, entry);
            versions.emplace(lease.get(), entry);
            bytes += entry->bytes;
            required.push_back(saved.token);
        }
    } catch (...) { rollback(); throw; }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.current.swap(current);
        p.versions.swap(versions);
        p.next_generation = snapshot.next_generation;
        p.restored = true;
        p.counts.retained_bytes = bytes;
    }
    uploads = std::move(required);
    if (error) error->clear();
    return Result::Ok;
}

}  // namespace title
