#pragma once

#include <memory>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

namespace title {

// Backend allocation ownership for the ordered Graphics queue. Fresh Register
// does not call the backend. RegisterExisting/Upload/Resolve/Collect run only on
// its context owner; fresh producer registrations may run concurrently.
//
// TR resources.cpp TextureRead owns an asset until TextureDestroy; textures.cpp
// schedules its upload before the material's draw. Native callers supply owned,
// converted RGBA8 rows at load time. No transfer chain, slot/VRAM identity, pixel
// format decoder, model register or mutable guest pointer enters this interface.
// Source asset readers and producer adapters are separate retirement work.
class GraphicsResources {
public:
    enum class Result { Ok, InvalidDescription, InvalidResource, InvalidUpload,
                        QueueRejected, LimitExceeded, BackendFailed };
    struct Limits {
        uint32_t max_dimension = 8192;
        uint64_t max_pixels = 16 * 1024 * 1024;
        size_t resources = 4096;
    };
    struct Counters {
        uint64_t registered = 0, created = 0, uploads = 0, destroyed = 0;
        uint64_t uploaded_bytes = 0, failures = 0;
        // Includes imported surfaces; created/destroyed count our allocations.
        size_t live_bindings = 0, live_surfaces = 0;
    };

    explicit GraphicsResources(ps2r::render::Backend& backend);
    GraphicsResources(ps2r::render::Backend& backend, Limits limits);
    // Destroy on the backend owner after the queue is quiescent and its leases
    // have been released. Backend must outlive this object.
    ~GraphicsResources();
    GraphicsResources(const GraphicsResources&) = delete;
    GraphicsResources& operator=(const GraphicsResources&) = delete;

    // Register a fresh allocation version with a producer's queue. Reusing a
    // guest object/asset address never reuses old storage by accident. Recorded
    // draws/uploads keep the binding alive after ReleaseResource or scene unload.
    Result Register(Graphics& queue, const ps2r::render::SurfaceDesc& description,
                    Graphics::Handle* handle);

    // Bind an existing surface from this SAME backend, on its context owner.
    // description is checked against that exact allocation. lifetime must prevent
    // resize/destruction/reuse of this exact surface until released; keeping a
    // mutable target cache alive alone is insufficient. No allocation, copy,
    // address lookup or ownership transfer occurs. This registry never destroys an
    // imported surface; its lifetime is released on fenced collection.
    //
    // Reimporting the same surface with the same description and shared owner
    // uses one binding identity, preserving feedback checks across queue handles.
    // A different owner/description, or importing our own allocation, is rejected.
    Result RegisterExisting(Graphics& queue,
                            const ps2r::render::SurfaceDesc& description,
                            ps2r::render::SurfaceId surface,
                            std::shared_ptr<const void> lifetime,
                            Graphics::Handle* handle);

    // Suitable for Graphics::Hooks::upload on the ordered backend owner.
    // Validation is complete before allocation/upload. Backend exceptions return
    // BackendFailed and quarantine that surface until quiescent collection.
    Result Upload(const Graphics::Upload& upload);

    // Resolve an explicit owned binding for DrawParams/TextureSource. Allocation
    // is lazy; texture callers must have submitted their upload first. No target
    // search by legacy address or register-state inference occurs.
    Result Resolve(const Graphics::Lease& resource, ps2r::render::SurfaceId* surface);

    // Call after a consumer fence. Releases ONLY expired bindings; queued or
    // externally held leases still pin their allocations. Destruction remains
    // on the backend owner even if the last producer lease died elsewhere.
    Result CollectAfterQuiescence();
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
