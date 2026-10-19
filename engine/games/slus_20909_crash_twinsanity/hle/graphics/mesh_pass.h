#pragma once

#include <map>

#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_material.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_service.h"

namespace title {

// Source instance/material collection, with no guest rendering producer in the
// selected deferred rigid/placed path. Direct/default meshes remain unselected.
// Root owns resource loading, source anchors, and submission.
class MeshPass {
public:
    using Body = SkyService::Body;
    using Capture = SkyService::Capture;
    using Installer = SkyService::Installer;
    static constexpr uint32_t kForget = 0x001c0410;
    static constexpr uint32_t kMaterialCount = 0x0030a810, kMaterials = 0x003d5cf0;
    struct Write { uint32_t address; std::vector<uint8_t> bytes; };
    using Geometry = std::function<MeshGeometry::Result(
        const MeshGeometry::Mesh&, const std::vector<MeshGeometry::View>&,
        const ps2r::gs::DrawIr&, std::vector<ps2r::gs::DrawIr>&)>;
    struct Request {
        uint32_t material;
        MeshMaterial::Output shader;
        Geometry geometry{};
    };
    using Record = std::function<bool(const std::vector<ps2r::gs::DrawIr>&)>;
    struct Hooks {
        bool enabled = false;
        MaterialState::Read read;
        std::function<bool(const std::vector<Write>&)> write;
        std::function<MeshAssets::ModelLease(uint32_t)> model;
        MeshMaterial::Lookup texture;
        std::function<bool(MaterialState::Environment&)> environment;
        // Certificate from whole-source consumer selection, not "seen so far".
        // A material shared with an unselected producer cannot be claimed.
        std::function<bool(uint32_t material)> owns_material;
        // Optional complete material owner. Captures at the source flush,
        // with an earlier eligibility check during collection. Geometry
        // must own all deferred inputs and must not reread source memory.
        // False/throw refuses selection; after collection it faults the pass.
        std::function<bool(uint32_t, const MaterialState::Environment&,
                           MeshMaterial::Output&, Geometry&)> capture_material;
        // Reserve each request's actual bucket/source position and its target
        // leases atomically. False/throw leaves root unchanged. Records retain
        // those exact bindings and enqueue upload-before-draw work atomically.
        // Positions are reserved at FlushMaterial, after source block closure.
        std::function<bool(const std::vector<Request>&, std::vector<Record>&)> reserve;
        // Optional whole-frame admission, asked ONCE per source frame at its
        // first model boundary (the frame is the ForgetMaterials-delimited
        // block). False = this frame runs every boundary on the model path
        // with NO collection (whole-bucket fallback: the model and the owned
        // producer never share one frame's material list). Root answers false
        // while a live material has no receipt (a restored image), so an
        // uncertifiable material never falls to the writer mid-frame next to
        // an owned one. Absent = every frame admitted.
        std::function<bool()> frame_admits;
    };
    struct Count { uint64_t calls = 0, replaced = 0, model_calls = 0, interior = 0; };
    struct Counters {
        Count rigid, placed, direct, flush;
        uint64_t blocks = 0, instances = 0, faults = 0;
        // Source frames the admission hook decided: admitted (owned collection
        // ran) / whole_model (every boundary of the frame took the model path).
        uint64_t frames_admitted = 0, frames_whole_model = 0;
    };
    explicit MeshPass(Hooks hooks);
    bool Install(const Capture& capture, const Installer& installer);
    void Run(uint32_t boundary, CpuContext& context, uint32_t entry);
    Counters Stats() const { return counts_; }
    // Root may cancel only an abandoned, unsubmitted frame. Reservations must
    // be cancelled in the root queue too. Normal source ForgetMaterials resets.
    void Reset();
private:
    struct Block {
        MeshAssets::ModelLease model;
        std::vector<MeshGeometry::View> instances;
        std::vector<uint32_t> materials;
        uint32_t size = 0;
        bool closed = false;
    };
    struct Part { std::shared_ptr<Block> block; size_t index; };
    struct Material {
        std::vector<Part> parts;
        std::vector<Request> requests;
        std::vector<Record> records;
        size_t delivered = 0;
    };
    bool Append(uint32_t boundary, const CpuContext& context);
    bool Reserve(const std::vector<uint32_t>& materials);
    bool CaptureMaterial(uint32_t, const MaterialState::Environment&, Request&);
    void Deliver(Material& material);
    void Close();
    [[noreturn]] void Fault(const char* message);
    Hooks hooks_;
    std::map<uint32_t, Body> originals_;
    std::map<uint32_t, std::shared_ptr<Block>> active_;
    std::map<uint32_t, Material> materials_;
    std::map<uint32_t, MeshAssets::ModelLease> selected_models_;
    Counters counts_;
    bool installed_ = false, faulted_ = false;
    // The frame's admission: decided at its first model boundary, cleared at
    // ForgetMaterials / Reset.
    bool frame_decided_ = false, frame_admitted_ = true;
};
}  // namespace title
