#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_pass.h"

#include <cstring>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"

namespace title {
namespace {
constexpr std::array<uint32_t, 2> entries{DistortionPass::kDraw, DistortionPass::kForget};
// Source DistortionAcross/DistortionDown scale the strengths before VU1.
constexpr float kAcross = 0x1p-14f, kDown = -0x1p-12f;
template<class T> bool Read(const MaterialState::Read& read, uint32_t address, T& out) {
    return read && address <= 0x02000000 - sizeof(T) && read(address, &out, sizeof(T));
}
float Bits(uint32_t value) { float result; std::memcpy(&result, &value, 4); return result; }
}  // namespace

bool DistortionPass::Batch::Ready() const { return state_.load(std::memory_order_acquire) == State::Ready; }
bool DistortionPass::Batch::Complete() const { return state_.load(std::memory_order_acquire) == State::Complete; }
bool DistortionPass::Batch::Failed() const { return state_.load(std::memory_order_acquire) == State::Failed; }
const std::vector<ps2r::gs::DrawIr>& DistortionPass::Batch::Draws() const {
    if (!Ready()) throw std::logic_error("Distortion batch is not ready");
    return draws_;
}
bool DistortionPass::Batch::Finish(bool success) const {
    State expected = State::Ready;
    if (state_.compare_exchange_strong(expected, success ? State::Complete : State::Failed,
                                       std::memory_order_acq_rel)) return success;
    state_.store(State::Failed, std::memory_order_release);
    return false;
}

DistortionPass::DistortionPass(DistortionBlocks& blocks, Hooks hooks)
    : blocks_(blocks), hooks_(std::move(hooks)) {}

bool DistortionPass::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || (hooks_.enabled && (!hooks_.read || !hooks_.environment ||
        !hooks_.frame || !hooks_.reserve || !hooks_.settings))) return false;
    for (uint32_t address : entries) {
        auto body = capture(address);
        if (!body) { originals_.clear(); return false; }
        originals_[address] = std::move(body);
    }
    const auto active = std::make_shared<std::atomic<bool>>(false);
    const auto rollback = [&](size_t last) {
        bool restored = true;
        for (size_t count = last + 1; count != 0; --count) {
            const auto address = entries[count - 1];
            try { if (!install(address, originals_.at(address))) restored = false; }
            catch (...) { restored = false; }
        }
        originals_.clear();
        if (!restored) throw std::runtime_error("DistortionPass dispatch rollback failed");
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto address = entries[i];
        const auto original = originals_.at(address);
        bool accepted = false;
        try {
            accepted = install(address, [this, active, original, address](CpuContext& c, uint32_t e) {
                if (!active->load(std::memory_order_acquire)) { original(c, e); return; }
                Run(address, c, e);
            });
        } catch (...) { rollback(i); throw; }
        if (!accepted) { rollback(i); return false; }
    }
    installed_ = true;
    active->store(true, std::memory_order_release);
    return true;
}

[[noreturn]] void DistortionPass::Fault(const char* text) {
    if (!faulted_) ++counts_.faults;
    faulted_ = true;
    throw std::runtime_error(text);
}
void DistortionPass::Collect() {
    for (auto at = pending_.begin(); at != pending_.end();) {
        if ((*at)->Failed()) Fault("Distortion ordered consumer failed");
        if ((*at)->Complete()) { ++counts_.completed; at = pending_.erase(at); }
        else ++at;
    }
}
bool DistortionPass::Open(uint32_t material) {
    // The source would start the material writer here: StartMaterialWriter
    // inserts the material into bucket 23 at its first draw of the frame.
    uint32_t shader = 0, writer = 0, target = 0;
    if (!Read(hooks_.read, material, shader) || !Read(hooks_.read, material + kWriterFirst, writer) ||
        !Read(hooks_.read, MaterialState::kRenderTarget, target) || !Read(hooks_.read, target, target_))
        Fault("Distortion material object unavailable");
    uint32_t vtable = 0;
    if (!Read(hooks_.read, shader + 0x6c, vtable) || vtable != DistortionMaterial::kVtable)
        Fault("Distortion material shader is not the retail type18 object");
    // Everything below is a pure read; nothing is reserved or written yet, so
    // an unprovable frame refuses to the model instead of faulting.
    if (writer) return false; // the model already owns this frame's writer
    MaterialState::Environment environment;
    MaterialState::Frame values;
    Frame frame;
    if (!hooks_.environment(environment) || !hooks_.frame(frame)) return false;
    if (MaterialState::ReadFrame(hooks_.read, environment, values) != MaterialState::Result::Ok) return false;
    if (values.display_width != frame.width || values.display_height != frame.height ||
        target_[3] != frame.width || target_[4] != frame.height)
        return false;
    DistortionMaterial::Settings settings;
    if (!hooks_.settings(shader, settings)) return false; // no observed settings version yet
    if (DistortionMaterial::Build(settings, values, target_, frame.source_frame, material_) !=
        DistortionMaterial::Result::Ok)
        return false;
    auto batch = std::make_shared<Batch>(frame, environment);
    // RecordOwned false = "stay unselected for this call" (INTERFACES section 8).
    if (!hooks_.reserve(batch)) return false;
    pending_.push_back(batch);
    open_ = std::move(batch);
    ++counts_.batches;
    return true;
}
bool DistortionPass::Draw(const CpuContext& context) {
    const auto& gpr = context.cpuRegs.GPR.r;
    const uint32_t block = gpr[4].UL[0], table = gpr[5].UL[0], matrix = gpr[6].UL[0], material = gpr[7].UL[0];
    if (material != kMaterial) Fault("Distortion draw with a foreign material");
    if (!open_ && !Open(material)) return false;
    std::vector<DistortionGeometry::Block> blocks;
    if (blocks_.Capture(block, blocks) != DistortionBlocks::Result::Ok)
        Fault("Distortion block chain is not owned");
    DistortionGeometry::Look look;
    std::array<float, 12> corners{};
    std::array<std::array<uint32_t, 8>, 64> steps{};
    std::array<float, 16> screen{};
    if (!Read(hooks_.read, table + 0x14, look.gravity) || !Read(hooks_.read, table + kTableCorners, corners) ||
        !Read(hooks_.read, table + kTableSteps, steps) || !Read(hooks_.read, matrix, screen))
        Fault("Distortion render table unavailable");
    for (unsigned corner = 0; corner < 6; ++corner)
        look.corners[corner] = {corners[corner * 2], corners[corner * 2 + 1]};
    for (unsigned step = 0; step < 64; ++step)
        for (unsigned value = 0; value < 6; ++value) look.shapes[step][value] = Bits(steps[step][value]);
    DistortionGeometry::View view;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) view.screen[row][column] = screen[row * 4 + column];
    view.time = context.fpuRegs.fpr[12].f;
    view.distortion = {context.fpuRegs.fpr[13].f * kAcross, context.fpuRegs.fpr[14].f * kDown};
    view.width = target_[3]; view.height = target_[4];
    std::vector<ps2r::gs::DrawIr> draws;
    if (DistortionGeometry::Build(blocks, look, view, material_, draws) != DistortionGeometry::Result::Ok)
        Fault("Distortion geometry rejected the source values");
    counts_.blocks += blocks.size();
    counts_.draws += draws.size();
    open_->draws_.insert(open_->draws_.end(), draws.begin(), draws.end());
    return true;
}
void DistortionPass::Forget() {
    ++counts_.forgets;
    if (!open_) return;
    open_->state_.store(Batch::State::Ready, std::memory_order_release);
    open_.reset();
}
void DistortionPass::Run(uint32_t boundary, CpuContext& context, uint32_t entry) try {
    if (!installed_ || !originals_.count(boundary) || faulted_)
        Fault("Distortion producer has no valid continuation");
    const bool draw = boundary == kDraw;
    if (draw) ++counts_.calls;
    const bool whole = !entry || entry == boundary;
    if (draw && !whole) ++counts_.interior;
    if (!hooks_.enabled) {
        if (draw) ++counts_.model_calls;
        originals_.at(boundary)(context, entry);
        return;
    }
    if (!whole) Fault("Selected distortion producer cannot import an interior continuation");
    Collect();
    if (draw) {
        // A refused frame stays with the model until the source forgets it.
        if (refusing_ || !Draw(context)) {
            if (!refusing_) { refusing_ = true; ++counts_.refused; }
            ++counts_.model_calls;
            originals_.at(boundary)(context, entry);
            return;
        }
        ++counts_.replaced;
    } else { Forget(); refusing_ = false; originals_.at(boundary)(context, entry); }
} catch (...) {
    if (!faulted_) { faulted_ = true; ++counts_.faults; }
    throw;
}
bool DistortionPass::CanCheckpoint(bool unlinked_recording_open) const {
    if (faulted_ || open_ || refusing_ || blocks_.Busy()) return false;
    for (const auto& batch : pending_)
        if (!batch->Complete() && !(unlinked_recording_open && batch->Ready())) return false;
    return true;
}
DistortionPass::Counters DistortionPass::Counts() const {
    auto result = counts_;
    for (const auto& batch : pending_) if (batch->Complete()) ++result.completed;
    return result;
}
void DistortionPass::ResetAfterQuiescence() {
    open_.reset();
    pending_.clear();
    faulted_ = refusing_ = false;
}
}  // namespace title
