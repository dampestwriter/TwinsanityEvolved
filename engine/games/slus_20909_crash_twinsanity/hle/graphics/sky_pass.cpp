#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_pass.h"
#include "engine/ee/timing/tick_profile.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace title {
namespace {
template<class T> bool Read(const MaterialState::Read& read, uint32_t address, T& value) {
    return read && address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &value, sizeof(T));
}
template<class T> SkyPass::Write Store(uint32_t address, const T& value) {
    SkyPass::Write result{address, std::vector<uint8_t>(sizeof(T))};
    std::memcpy(result.bytes.data(), &value, sizeof(T));
    return result;
}
constexpr std::array<uint32_t, 7> entries{SkySetup::kAddress, SkyPass::kDrawModel, SkyIntent::kAddress,
    SkyAssets::kModelRead, SkyAssets::kModelDestroy, SkyAssets::kTextureRead, SkyAssets::kTextureDestroy};
}

SkyPass::SkyPass(Hooks hooks) : hooks_(std::move(hooks)) {}
bool SkyPass::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || !hooks_.read || !hooks_.write || !hooks_.environment ||
        !hooks_.begin || !hooks_.record) return false;
    std::map<uint32_t, Body> originals;
    for (uint32_t entry : entries) {
        auto body = capture(entry);
        if (!body) return false;
        originals.emplace(entry, std::move(body));
    }
    originals_ = std::move(originals);
    size_t done = 0;
    for (uint32_t entry : entries) {
        if (!install(entry, [this, entry](CpuContext& context, uint32_t at) { Run(entry, context, at); })) {
            while (done) {
                const uint32_t rollback = entries[--done];
                if (!install(rollback, originals_.at(rollback))) Fault("Sky installation rollback failed");
            }
            originals_.clear();
            return false;
        }
        ++done;
    }
    installed_ = true;
    return true;
}
GraphicsRefusals::Reason SkyPass::ReasonOf(Refusal refusal) {
    using Reason = GraphicsRefusals::Reason;
    switch (refusal) {
        case Refusal::Environment: case Refusal::Begin: case Refusal::Nested:
        case Refusal::Interior: case Refusal::Unopened:
            return Reason::Ordering;
        case Refusal::ModelMissing: case Refusal::Material:
            return Reason::MissingResource;
        case Refusal::None: case Refusal::Setup: case Refusal::Source: case Refusal::ModelShape:
        case Refusal::Intent: case Refusal::Exception: case Refusal::Programs:
            break;
    }
    return Reason::UnsupportedState;
}
void SkyPass::Fault(const char* message, uint32_t boundary) {
    faulted_ = true;
    ++counts_.faults;
    if (hooks_.fault) hooks_.fault(GraphicsRefusals::Reason::Ordering, boundary, 0);
    throw std::runtime_error(message);
}
void SkyPass::Refuse(Refusal refusal, uint32_t boundary) {
    ++counts_.refused;
    counts_.last_refusal = refusal;
    if (hooks_.refuse) hooks_.refuse(ReasonOf(refusal), boundary, uint32_t(refusal));
}
SkyPass::Refusal SkyPass::Prepare(uint32_t sky) {
    MaterialState::Environment environment;
    Output candidate;
    std::vector<Model> models;
    std::set<uint32_t> unlisted;
    std::array<uint32_t, 4> source{};
    SkyGeometry::Matrix writable{};
    uint32_t listed = 0;
    try {
        if (!hooks_.environment(environment)) return Refusal::Environment;
        if (SkySetup::Capture(hooks_.read, environment, candidate.setup) != SkySetup::Result::Ok)
            return Refusal::Setup;
        if (!Read(hooks_.read, sky, source) || sky > 0xffffffbf ||
            !Read(hooks_.read, sky + 0x10, writable) ||
            source[2] > 256 || source[3] > 0xfffffbff ||
            !Read(hooks_.read, kMaterialCount, listed) || listed > 500)
            return Refusal::Source;
        for (uint32_t i = 0; i < source[2]; ++i) {
            Model model{};
            std::array<uint32_t, 8> rigid{};
            if (!Read(hooks_.read, source[3] + i * 4, model.address) ||
                !Read(hooks_.read, model.address, rigid)) return Refusal::Source;
            model.geometry = assets_.FindModel(rigid[3]);
            if (!model.geometry) return Refusal::ModelMissing;
            if (rigid[6] > 64 || rigid[2] > 0xffffff00) return Refusal::ModelShape;
            const size_t count = std::min(size_t(rigid[6]), model.geometry->parts.size());
            for (size_t part = 0; part < count; ++part) {
                uint32_t resource = 0;
                Part item{};
                uint8_t flag = 0;
                if (!Read(hooks_.read, rigid[2] + uint32_t(part) * 4, resource) || resource > 0xfffffff7 ||
                    !Read(hooks_.read, resource + 8, item.material) || item.material > 0xffffff98)
                    return Refusal::Source;
                if (SkyMaterial::Capture(hooks_.read, item.material, candidate.setup, assets_, item.shader) !=
                        SkyMaterial::Result::Ok) return Refusal::Material;
                if (!Read(hooks_.read, item.material + 0x64, flag)) return Refusal::Source;
                if (!flag) unlisted.insert(item.material);
                model.parts.push_back(std::move(item));
            }
            models.push_back(std::move(model));
        }
        if (unlisted.size() > 500 - listed) return Refusal::Source;
        if (SkyIntent::Build(candidate.setup.width, candidate.setup.height, environment, candidate.finish) !=
                SkyIntent::Result::Ok) return Refusal::Intent;
        // The VU1 program residency the model's RenderSkyMaterial would leave
        // (SkyPrograms): planned here, in the pass's material order, from the
        // live tables; emitted per model with its bookkeeping. Unreadable or
        // out-of-shape state refuses before anything is committed.
        SkyPrograms residency;
        if (!residency.Capture(hooks_.read)) return Refusal::Programs;
        for (auto& model : models) {
            SkyPrograms::Plan plan;
            for (const auto& part : model.parts)
                if (residency.Step(hooks_.read, part.material, plan) != SkyPrograms::Result::Ok)
                    return Refusal::Programs;
            model.programs = std::move(plan.writes);
            model.uploads = plan.uploads;
        }
    } catch (...) { return Refusal::Exception; }
    if (!hooks_.begin(candidate.setup.width, candidate.setup.height)) return Refusal::Begin;
    // Selection is now final. Any later error faults rather than mixing arms.
    active_ = true;
    output_ = std::move(candidate);
    models_ = std::move(models);
    next_model_ = vertices_ = 0;
    if (!hooks_.write({Store(sky + 0x10, output_.setup.to_screen)})) Fault("Sky matrix write failed", SkySetup::kAddress);
    return Refusal::None;
}

void SkyPass::DrawModel(CpuContext& context) {
    if (next_model_ >= models_.size()) Fault("Unexpected sky model");
    const Model& model = models_[next_model_];
    if (context.cpuRegs.GPR.r[4].UL[0] != model.address) Fault("Sky model order changed");
    const uint32_t view_address = context.cpuRegs.GPR.r[5].UL[0];
    uint32_t reference = 0, camera = 0;
    if (view_address > 0xfffffe9f || !Read(hooks_.read, view_address + 0x160, reference) ||
        (reference && !Read(hooks_.read, reference, camera))) Fault("Sky camera read failed");
    ++next_model_;
    if (!camera) return; // source's complete no-camera early return
    SkyGeometry::View view;
    uint32_t render_view = 0, listed = 0;
    if (!Read(hooks_.read, context.cpuRegs.GPR.r[6].UL[0], view.screen) ||
        !Read(hooks_.read, context.cpuRegs.GPR.r[7].UL[0], view.clip) ||
        !Read(hooks_.read, SkySetup::kRenderView, render_view) || render_view > 0xfffffebf ||
        !Read(hooks_.read, render_view + 0x140, view.guard) ||
        !Read(hooks_.read, kMaterialCount, listed) || listed > 500)
        Fault("Sky matrix/material read failed");
    std::vector<Write> writes;
    std::set<uint32_t> added;
    for (size_t part = 0; part < model.parts.size(); ++part) {
        const auto& material = model.parts[part];
        uint8_t flag = 0;
        if (!Read(hooks_.read, material.material + 0x64, flag)) Fault("Sky material flag read failed");
        if (!flag && added.insert(material.material).second) {
            if (listed == 500) Fault("Sky material list overflow");
            writes.push_back(Store(kMaterials + listed++ * 4, material.material));
            writes.push_back(Store(material.material + 0x64, uint8_t(1)));
        }
        writes.push_back(Store(material.material + 0x66, uint8_t(1)));
        view.scroll = material.shader.scroll;
        std::vector<ps2r::gs::DrawIr> draws;
        if (SkyGeometry::Build(model.geometry->parts[part], view, material.shader.state, draws) !=
            SkyGeometry::Result::Ok) Fault("Sky geometry rejected after selection");
        for (auto& draw : draws) {
            vertices_ += draw.verts.size();
            if (vertices_ > SkyGeometry::kMaxOutputVertices) Fault("Sky pass vertex limit");
            output_.draws.push_back({std::move(draw), material.shader.texture});
        }
    }
    writes.push_back(Store(kMaterialCount, listed));
    writes.insert(writes.end(), model.programs.begin(), model.programs.end());
    if (!hooks_.write(writes)) Fault("Sky material bookkeeping write failed");
    counts_.programs_uploaded += model.uploads;
}
void SkyPass::Finish() {
    if (next_model_ != models_.size()) Fault("Sky pass ended before its models");
    if (!hooks_.record(output_)) Fault("Sky pass recording failed");
    active_ = false;
    models_.clear();
    output_ = {};
}
void SkyPass::Run(uint32_t boundary, CpuContext& context, uint32_t entry) {
    if (faulted_ || !originals_.count(boundary)) throw std::logic_error("Sky continuation unavailable");
    const bool full = entry == 0 || entry == boundary;
    Counts* counts = boundary == SkySetup::kAddress ? &counts_.start :
        boundary == kDrawModel ? &counts_.model : boundary == SkyIntent::kAddress ? &counts_.finish : nullptr;
    if (counts) {
        if (full) ++counts->calls;
        else ++counts->interior_calls;
    }
    try {
        if (hooks_.enabled && counts && !full) {
            // Inside an owned pass the source has already taken this owner's
            // matrix: an interior continuation cannot be mixed in. Outside one
            // nothing is committed: refuse to the original continuation.
            if (active_) Fault("Sky producer entered at an interior label", boundary);
            Refuse(Refusal::Interior, boundary);
        } else if (full && boundary == SkySetup::kAddress) {
            if (active_) Fault("Nested sky pass", boundary);
            if (hooks_.enabled) {
                if (model_pass_) {
                    Refuse(Refusal::Nested, boundary); // the open model pass continues
                } else {
                    const auto refusal = Prepare(context.cpuRegs.GPR.r[4].UL[0]);
                    if (refusal == Refusal::None) { ++counts->replaced; return; }
                    // Nothing committed: the ORIGINAL StartSkyBuffer runs and
                    // this pass stays a model pass through its FinishSky.
                    Refuse(refusal, boundary);
                    model_pass_ = true;
                }
            }
        } else if (active_ && boundary == kDrawModel) {
            DrawModel(context);
            ++counts->replaced;
            return;
        } else if (active_ && boundary == SkyIntent::kAddress) {
            Finish();
            ++counts->replaced;
            return;
        } else if (hooks_.enabled && counts && full) {
            // DrawSkyModel/FinishSky of a model pass run their originals; with
            // no StartSkyBuffer seen at all they refuse the same way (ordering).
            if (!model_pass_) Refuse(Refusal::Unopened, boundary);
            if (boundary == SkyIntent::kAddress) model_pass_ = false;
        }
        const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
        // Forget BEFORE source destruction; leases held by a selected pass or
        // recorded output keep the previous owned version alive.
        if (full && boundary == SkyAssets::kModelDestroy) assets_.ForgetModel(object);
        if (full && boundary == SkyAssets::kTextureDestroy) assets_.ForgetTexture(object);
        if (counts) ++counts->model_calls;
        originals_.at(boundary)(context, entry);
        const auto declined = [this](SkyAssets::Result result) {
            ++counts_.load_declines;
            if (result == SkyAssets::Result::InvalidMemory) ++counts_.declines_memory;
            else if (result == SkyAssets::Result::UnsupportedAsset) ++counts_.declines_asset;
            else if (result == SkyAssets::Result::NotConverted) ++counts_.declines_converted;
            else ++counts_.declines_limit;
        };
        if (full && hooks_.enabled && boundary == SkyAssets::kModelRead) {
            ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Sky);
            const auto result = assets_.ModelLoaded(hooks_.read, object);
            if (result == SkyAssets::Result::Ok) ++counts_.models_loaded;
            else declined(result);
        }
        if (full && hooks_.enabled && boundary == SkyAssets::kTextureRead) {
            ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Sky);
            uint32_t transfer = 0;
            const bool owned_word = hooks_.read && hooks_.read(object + 0x60, &transfer, sizeof(transfer)) &&
                transfer == 0xffffffff;
            const auto result = assets_.TextureLoaded(hooks_.read, object, hooks_.converted);
            if (result == SkyAssets::Result::Ok) {
                ++counts_.textures_loaded;
                if (owned_word) ++counts_.textures_converted;
            } else declined(result);
        }
    } catch (...) {
        if (active_ && !faulted_) { faulted_ = true; ++counts_.faults; }
        throw;
    }
}
SkyPass::Counters SkyPass::Stats() const { return counts_; }
bool SkyPass::CaptureAssets(SkyAssets::Snapshot& snapshot, std::string* error, bool unlinked_recording_open) const {
    if (active_ || faulted_) {
        if (error) *error = "Sky state requires a completed, nonfaulted source boundary";
        return false;
    }
    return assets_.Capture(snapshot, error, unlinked_recording_open);
}
bool SkyPass::RestoreAssets(const SkyAssets::Snapshot& snapshot, std::string* error) {
    if (active_ || faulted_) {
        if (error) *error = "Sky assets cannot be restored during selected work";
        return false;
    }
    return assets_.Restore(snapshot, error);
}
void SkyPass::Reset() {
    active_ = faulted_ = model_pass_ = false;
    output_ = {};
    models_.clear();
    assets_.Reset();
    next_model_ = vertices_ = 0;
}
}  // namespace title
