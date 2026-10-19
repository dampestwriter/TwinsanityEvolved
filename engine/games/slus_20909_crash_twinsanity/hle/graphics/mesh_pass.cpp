#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"

#include "engine/ee/timing/tick_profile.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace title {
namespace {
constexpr std::array<uint32_t, 6> entries{MeshIntent::kRigid, MeshIntent::kPlaced,
    MeshIntent::kDefault, MeshIntent::kClose, MeshIntent::kFlushMaterial, MeshPass::kForget};
template<class T> bool Read(const MaterialState::Read& read, uint32_t address, T& value) {
    return read && address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &value, sizeof(T));
}
template<class T> MeshPass::Write Store(uint32_t address, const T& value) {
    MeshPass::Write result{address, std::vector<uint8_t>(sizeof(T))};
    std::memcpy(result.bytes.data(), &value, sizeof(T));
    return result;
}
}  // namespace
MeshPass::MeshPass(Hooks hooks) : hooks_(std::move(hooks)) {}
bool MeshPass::Install(const Capture& capture, const Installer& installer) {
    if (installed_) return true;
    if (!capture || !installer || !hooks_.read || !hooks_.write || !hooks_.model ||
        !hooks_.texture || !hooks_.environment || !hooks_.owns_material || !hooks_.reserve) return false;
    for (auto entry : entries) {
        auto original = capture(entry);
        if (!original) { originals_.clear(); return false; }
        originals_.emplace(entry, std::move(original));
    }
    size_t done = 0;
    for (auto entry : entries) {
        if (!installer(entry, [this, entry](CpuContext& context, uint32_t at) { Run(entry, context, at); })) {
            while (done) {
                const auto rollback = entries[--done];
                if (!installer(rollback, originals_.at(rollback))) Fault("Mesh installation rollback failed");
            }
            originals_.clear();
            return false;
        }
        ++done;
    }
    installed_ = true;
    return true;
}
[[noreturn]] void MeshPass::Fault(const char* message) {
    faulted_ = true;
    ++counts_.faults;
    throw std::runtime_error(message);
}
bool MeshPass::CaptureMaterial(uint32_t address, const MaterialState::Environment& environment,
                               Request& output) {
    Request request{address, {}};
    if (hooks_.capture_material) {
        if (!hooks_.capture_material(address, environment, request.shader, request.geometry) ||
            !request.geometry) return false;
    } else {
        if (MeshMaterial::Capture(hooks_.read, address, environment, hooks_.texture, request.shader) !=
            MeshMaterial::Result::Ok) return false;
        request.geometry = MeshGeometry::Build;
    }
    output = std::move(request);
    return true;
}
bool MeshPass::Reserve(const std::vector<uint32_t>& addresses) {
    MaterialState::Environment environment;
    std::vector<Request> requests;
    try {
        if (!hooks_.environment(environment)) return false;
        for (auto material : addresses) {
            Request request{material, {}};
            if (!CaptureMaterial(material, environment, request)) return false;
            requests.push_back(std::move(request));
        }
    } catch (...) { return false; }
    std::vector<Record> records;
    try { if (!hooks_.reserve(requests, records)) return false; }
    catch (...) { return false; }
    if (records.size() != requests.size() ||
        std::any_of(records.begin(), records.end(), [](const Record& record) { return !record; }))
        Fault("Mesh reservation returned incomplete records");
    for (size_t i = 0; i < requests.size(); ++i) {
        auto& material = materials_.at(requests[i].material);
        material.requests.push_back(std::move(requests[i]));
        material.records.push_back(std::move(records[i]));
    }
    return true;
}
bool MeshPass::Append(uint32_t boundary, const CpuContext& context) {
    // StartMaterialDirectly does not establish the same complete state as
    // FlushMaterial. Its inherited caller state is a separate source gate.
    if (boundary == MeshIntent::kDefault) return false;
    MeshIntent::Output intent;
    if (MeshIntent::Capture(hooks_.read, boundary, context, intent) != MeshIntent::Result::Ok)
        return false;
    MeshAssets::ModelLease model;
    uint32_t count = 0;
    std::vector<uint32_t> fresh, unlisted;
    std::vector<Write> writes;
    try {
        model = hooks_.model(intent.geometry);
        if (!model || model->parts.size() < intent.materials.size() ||
            !Read(hooks_.read, kMaterialCount, count) || count > 500) return false;
        MaterialState::Environment environment;
        if (!hooks_.environment(environment)) return false;
        std::set<uint32_t> visited;
        for (auto address : intent.materials) {
            if (!visited.insert(address).second) continue;
            uint32_t writer = 0;
            uint8_t listed = 0, direct = 0;
            Request captured{address, {}};
            if (!hooks_.owns_material(address) || address > 0xffffff99 ||
                !Read(hooks_.read, address + 0x28, writer) || writer ||
                !Read(hooks_.read, address + 0x64, listed) ||
                !Read(hooks_.read, address + 0x66, direct) ||
                !CaptureMaterial(address, environment, captured)) return false;
            const auto found = materials_.find(address);
            if (direct) return false;
            if (found == materials_.end()) {
                if (listed || direct) return false; // another source producer already owns it
                fresh.push_back(address);
                unlisted.push_back(address);
            }
        }
        if (unlisted.size() > 500 - count) return false;
        for (auto address : unlisted) {
            writes.push_back(Store(address + 0x64, uint8_t(1)));
            writes.push_back(Store(kMaterials + count++ * 4, address));
        }
        if (!writes.empty()) writes.push_back(Store(kMaterialCount, count));
    } catch (...) { return false; }
    if (const auto selected = selected_models_.find(intent.object);
        selected != selected_models_.end() && selected->second != model)
        Fault("Mesh source object reloaded inside an active pass");
    const auto active = active_.find(intent.object);
    const bool opened = active == active_.end();
    if (!opened && active->second->materials != intent.materials)
        Fault("Mesh material references changed inside a source block");
    for (auto address : fresh) materials_.try_emplace(address);
    // Once a root reservation or software list has been published, there is
    // no model fallback. All remaining errors fault this unsubmitted frame.
    if (!writes.empty() && !hooks_.write(writes)) Fault("Mesh list write failed");
    selected_models_[intent.object] = model;
    std::shared_ptr<Block> block;
    if (opened) {
        block = std::make_shared<Block>();
        block->model = std::move(model);
        block->materials = intent.materials;
        active_[intent.object] = block;
        for (size_t part = 0; part < intent.materials.size(); ++part)
            materials_.at(intent.materials[part]).parts.push_back({block, part});
        ++counts_.blocks;
    } else block = active->second;
    block->instances.push_back(intent.view);
    block->size += intent.size;
    ++counts_.instances;
    if (!opened && (intent.kind == MeshIntent::Kind::Rigid
        ? block->size + 22 > 100 : block->size + 14 >= 100)) {
        block->closed = true;
        active_.erase(intent.object);
    }
    return true;
}
void MeshPass::Deliver(Material& material) {
    if (std::any_of(material.parts.begin(), material.parts.end(),
        [](const Part& part) { return !part.block->closed; })) return;
    for (; material.delivered < material.records.size(); ++material.delivered) {
        const auto& request = material.requests[material.delivered];
        const auto& shader = request.shader;
        std::vector<ps2r::gs::DrawIr> draws;
        for (const auto& part : material.parts) {
            auto instances = part.block->instances;
            for (auto& instance : instances) instance.scroll = shader.scroll;
            std::vector<ps2r::gs::DrawIr> batch;
            if (request.geometry(part.block->model->parts[part.index], instances, shader.state, batch) !=
                MeshGeometry::Result::Ok) Fault("Mesh geometry failed after selection");
            if (draws.size() + batch.size() > 65536) Fault("Mesh material draw limit");
            draws.insert(draws.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        }
        if (!material.records[material.delivered](draws)) Fault("Mesh ordered recording failed");
    }
}
void MeshPass::Close() {
    for (auto& [object, block] : active_) { (void)object; block->closed = true; }
    active_.clear();
    for (auto& [address, material] : materials_) { (void)address; Deliver(material); }
}
void MeshPass::Run(uint32_t boundary, CpuContext& context, uint32_t entry) try {
    if (faulted_) Fault("Mesh pass requires abandoned-frame reset");
    if (!installed_ || !originals_.count(boundary)) Fault("Mesh entry was not installed");
    Count* counter = boundary == MeshIntent::kRigid ? &counts_.rigid :
        boundary == MeshIntent::kPlaced ? &counts_.placed :
        boundary == MeshIntent::kDefault ? &counts_.direct :
        boundary == MeshIntent::kFlushMaterial ? &counts_.flush : nullptr;
    if (counter) ++counter->calls;
    const bool whole = !entry || entry == boundary;
    if (!whole) {
        if (counter) ++counter->interior;
        if (!selected_models_.empty()) Fault("Mesh interior entry after selection");
    }
    if (hooks_.enabled && whole) {
        // The pass's own host work (capture, delivery) is the tick's mesh site;
        // the title's original body below is not.
        ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Mesh);
        // The frame's admission, decided once at its first model boundary.
        // A frame refused whole runs every boundary below on the model path
        // and collects nothing: no owned material can share it with a writer.
        if (!frame_decided_ && boundary != MeshIntent::kClose && boundary != kForget) {
            frame_decided_ = true;
            frame_admitted_ = !hooks_.frame_admits || hooks_.frame_admits();
            ++(frame_admitted_ ? counts_.frames_admitted : counts_.frames_whole_model);
        }
        if (!frame_admitted_ && boundary != MeshIntent::kClose && boundary != kForget) {
            if (!materials_.empty() || !selected_models_.empty())
                Fault("Mesh whole-model frame holds owned collection");
        } else if (boundary == MeshIntent::kClose) {
            Close();
        } else if (boundary == kForget) {
            for (const auto& [address, material] : materials_) {
                (void)address;
                if (material.records.empty() || material.delivered != material.records.size())
                    Fault("Mesh material forgotten before source submission");
            }
            Reset();
        } else if (boundary == MeshIntent::kFlushMaterial) {
            const uint32_t address = context.cpuRegs.GPR.r[4].UL[0];
            if (const auto found = materials_.find(address); found != materials_.end()) {
                if (std::any_of(found->second.parts.begin(), found->second.parts.end(),
                    [](const Part& part) { return !part.block->closed; }))
                    Fault("Mesh flush before source block closure");
                uint32_t writer = 0;
                if (!Read(hooks_.read, address + 0x28, writer) || writer || !Reserve({address}))
                    Fault("Mesh material ownership or reservation changed");
                Deliver(found->second);
                ++counter->replaced;
                return;
            }
        } else {
            const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
            if (Append(boundary, context)) { ++counter->replaced; return; }
            if (selected_models_.count(object)) Fault("Selected mesh declined a later instance");
            // The root's ownership certificate is binding. A raw producer must
            // never silently join a material already collected by this pass.
            MeshIntent::Output candidate;
            if (MeshIntent::Capture(hooks_.read, boundary, context, candidate) == MeshIntent::Result::Ok)
                for (auto address : candidate.materials)
                    if (materials_.count(address)) Fault("Mixed model/native material producer");
        }
    }
    if (counter) ++counter->model_calls;
    originals_.at(boundary)(context, entry);
} catch (...) {
    if (!faulted_) { faulted_ = true; ++counts_.faults; }
    throw;
}
void MeshPass::Reset() {
    active_.clear();
    materials_.clear();
    selected_models_.clear();
    faulted_ = false;
    frame_decided_ = false;
    frame_admitted_ = true;
}
}  // namespace title
