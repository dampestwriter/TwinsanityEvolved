#pragma once

#include <map>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_material.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_programs.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_service.h"

namespace title {

// Complete source sky pass. DrawSky (camera/game orchestration) is retained;
// only StartSkyBuffer, DrawSkyModel, FinishSky and resource load notifications
// cross this boundary. Do not install the older FinishSky-only SkyService too.
class SkyPass {
public:
    using Body = SkyService::Body;
    using Capture = SkyService::Capture;
    using Installer = SkyService::Installer;
    static constexpr uint32_t kDrawModel = 0x001bec10;
    static constexpr uint32_t kMaterialCount = 0x0030a810, kMaterials = 0x003d5cf0;
    struct Draw {
        ps2r::gs::DrawIr intent;
        SkyAssets::TextureLease texture;
    };
    struct Output {
        SkySetup::Output setup;
        std::vector<Draw> draws;
        SkyIntent::Output finish;
    };
    using Write = SkyPrograms::Write;
    // Why a selected StartSkyBuffer REFUSED (detail code of the Sky refusal row).
    // Refusal happens before the owned arm commits anything (no matrix write,
    // no begin): the ORIGINAL StartSkyBuffer runs and the pass continues as a
    // model pass (DrawSkyModel/FinishSky originals, counted as model_calls)
    // until the source FinishSky closes it.
    enum class Refusal : uint32_t {
        None = 0,
        Environment,     // no completed source frame/environment for bucket 0 (ordering)
        Setup,           // SkySetup capture: frame/matrix shape unsupported
        Source,          // sky object/model list/material pointers unreadable or out of range
        ModelMissing,    // a sky model this owner never converted (missing resource)
        ModelShape,      // model part count/resource shape unsupported
        Material,        // SkyMaterial capture failed (texture not converted -> missing resource)
        Intent,          // FinishSky intent shape unsupported
        Begin,           // root could not pin the scene/half-size targets (ordering)
        Exception,       // a read threw
        Nested,          // StartSkyBuffer while a model pass is still open (ordering)
        Interior,        // interior entry of a boundary with no active owned pass (ordering)
        Unopened,        // DrawSkyModel/FinishSky with no StartSkyBuffer seen (ordering)
        Programs         // VU1 program residency (SkyPrograms) unreadable or out of shape
    };
    static GraphicsRefusals::Reason ReasonOf(Refusal refusal);
    struct Hooks {
        // Enabled selects all three producer boundaries. Missing source assets,
        // target/environment or complete entry REFUSES (counted, attributed,
        // original pass) before selection; faults after selection throw and
        // are never replayed by the model.
        bool enabled = false;
        MaterialState::Read read;
        // The texture owner's converted image of a texture object (textures at
        // load: TextureBindings::Converted). Consulted by the sky's load-
        // boundary conversion when the object carries the ownership word
        // instead of a DiskManager handle. Optional: without it every such
        // texture is declined (declines_converted) and its materials refuse.
        SkyAssets::Converted converted;
        // All writes or none. These are software matrix/material bookkeeping,
        // never renderer packets or device state. False after selection faults.
        std::function<bool(const std::vector<Write>&)> write;
        std::function<bool(MaterialState::Environment&)> environment;
        // Pins root's scene + half-size attachments for this whole pass.
        // False must leave the root unchanged. No clear or copy from the model.
        std::function<bool(uint32_t width, uint32_t height)> begin;
        // Atomically records uploads, sky geometry, then FinishSky at the
        // captured bucket-0 source position. Must retain owned resource leases.
        // Failure/throw after selection faults; never replay model setup.
        std::function<bool(const Output&)> record;
        // Attribution: refuse(reason, boundary, Refusal) once per refused pass
        // (or per unopened/interior model call); fault before every throw.
        GraphicsRefusals::Sink refuse, fault;
    };
    struct Counts {
        uint64_t calls = 0, replaced = 0, model_calls = 0, interior_calls = 0;
    };
    struct Counters {
        Counts start, model, finish;
        uint64_t models_loaded = 0, textures_loaded = 0, load_declines = 0, faults = 0;
        // load_declines by SkyAssets::Result: a node/bytes read that failed
        // (the DiskManager nodes live in the META arena under hle.meta_arena --
        // a reader without that window declines EVERY load), an asset shape
        // the converter does not take, the conversion cache limit.
        uint64_t declines_memory = 0, declines_asset = 0, declines_limit = 0;
        // a texture the texture owner published whose image the sky could not
        // take from it (no lookup, or released since)
        uint64_t declines_converted = 0;
        // textures taken from the texture owner's converted asset (the
        // ownership word at +0x60) rather than the DiskManager block
        uint64_t textures_converted = 0;
        uint64_t refused = 0;          // refused selected passes (model passes)
        Refusal last_refusal = Refusal::None;
        // MPG uploads the owned pass appended to the sky bucket's chain: the
        // model arm's VU1 program residency (SkyPrograms), kept so the frame's
        // later kicks run under the micro image the title's VU1 library knows.
        uint64_t programs_uploaded = 0;
    };
    explicit SkyPass(Hooks hooks);
    // Captures all seven entries before mutation; failed installation rolls
    // back earlier entries. Install before FrameEnvironment's FinishSky wrapper.
    bool Install(const Capture& capture, const Installer& installer);
    void Run(uint32_t boundary, CpuContext& context, uint32_t entry);
    Counters Stats() const;
    // Reset only at an abandoned/unsubmitted frame or load-state boundary.
    // Root must separately release/cancel its reservation and queued work.
    void Reset();
    const SkyAssets& Assets() const { return assets_; }
    // `unlinked_recording_open`: see SkyAssets::Capture (the settle admitted
    // this frame's recorded pass; its leases are the open recording's).
    bool CaptureAssets(SkyAssets::Snapshot&, std::string* error = nullptr, bool unlinked_recording_open = false) const;
    bool RestoreAssets(const SkyAssets::Snapshot&, std::string* error = nullptr);

private:
    struct Part { uint32_t material; SkyMaterial::Output shader; };
    struct Model {
        uint32_t address; SkyAssets::ModelLease geometry; std::vector<Part> parts;
        std::vector<Write> programs;   // the model's program residency writes (SkyPrograms)
        uint32_t uploads = 0;
    };
    Refusal Prepare(uint32_t sky);
    void DrawModel(CpuContext& context);
    void Finish();
    void Refuse(Refusal refusal, uint32_t boundary);
    [[noreturn]] void Fault(const char* message, uint32_t boundary = 0);
    Hooks hooks_;
    SkyAssets assets_;
    std::map<uint32_t, Body> originals_;
    Counters counts_;
    Output output_;
    std::vector<Model> models_;
    size_t next_model_ = 0, vertices_ = 0;
    bool installed_ = false, active_ = false, faulted_ = false, model_pass_ = false;
};

}  // namespace title
