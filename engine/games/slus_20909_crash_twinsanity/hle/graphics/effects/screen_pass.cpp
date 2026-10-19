#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_pass.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace title {
namespace {
template<class T> bool ReadAt(const MaterialState::Read& read, uint32_t address, T& out) {
    return read && address <= 0x02000000 - sizeof(T) && read(address, &out, sizeof(T));
}
}
ScreenPass::Result ScreenPass::ReadParameters(const Read& read, const CpuContext& context, Parameters& out) {
    if (context.cpuRegs.GPR.r[28].UL[0] != kGlobalPointer) return Result::UnsupportedSource;
    Parameters value;
    uint8_t blur = 0, filter = 0, wave = 0, blends = 0;
    uint32_t amount = 0;
    if (!ReadAt(read, kBlurSwitch, blur) || !ReadAt(read, kFilterSwitch, filter) || !ReadAt(read, kWaveSwitch, wave) ||
        !ReadAt(read, kScreenWidth, value.width) || !ReadAt(read, kScreenHeight, value.height) ||
        !ReadAt(read, kFilterPalette, value.palette) || !ReadAt(read, kFilterSecond, value.second) ||
        !ReadAt(read, kFilterBlends, blends) || !ReadAt(read, kFilterAmount, amount))
        return Result::InvalidMemory;
    value.blur = blur != 0; value.filter = filter != 0; value.wave = wave != 0;
    value.blends = blends != 0;
    std::memcpy(&value.amount, &amount, sizeof(amount));
    if (value.Any()) {
        // The source strips span 512 pixels at a fixed stride (effects.cpp:62);
        // other extents have no source-backed drawing.
        if (value.width != 512 || !value.height || value.height > 1024) return Result::UnsupportedSource;
        value.base = uint32_t(int32_t(value.width) * int32_t(value.height) * 10) >> 8;
    }
    out = value;
    return Result::Ok;
}
ScreenPass::Result ScreenPass::ReadWaves(const Read& read, WaveGeometry::Waves& out) {
    uint32_t rows = 0;
    std::array<std::array<float, 4>, 16> quadwords{};
    WaveGeometry::Waves value;
    if (!ReadAt(read, kWaveShader + kWaveRows, rows) || (rows & 15) || !ReadAt(read, rows, quadwords) ||
        !ReadAt(read, kWaveShader + kWaveAmplitude, value.amplitude))
        return Result::InvalidMemory;
    if (!std::isfinite(value.amplitude)) return Result::UnsupportedSource;
    for (size_t i = 0; i < quadwords.size(); ++i)
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(quadwords[i][axis])) return Result::UnsupportedSource;
            value.rows[i][axis] = quadwords[i][axis];
        }
    out = value;
    return Result::Ok;
}
bool ScreenPass::Batch::Finish(bool success) const {
    auto expected = State::Ready;
    if (!state_.compare_exchange_strong(expected, success ? State::Complete : State::Failed,
                                        std::memory_order_acq_rel))
        return false;
    if (!success && failed_) failed_->fetch_add(1, std::memory_order_acq_rel);
    return true;
}
bool ScreenPass::Batch::Cancel() const {
    auto expected = State::Ready;
    return state_.compare_exchange_strong(expected, State::Cancelled, std::memory_order_acq_rel);
}
const char* ScreenPass::Name(Refusal refusal) {
    switch (refusal) {
    case Refusal::None: return "none";
    case Refusal::Unreadable: return "unreadable";
    case Refusal::Palette: return "palette";
    case Refusal::Waves: return "waves";
    case Refusal::Frame: return "frame";
    case Refusal::Extent: return "extent";
    case Refusal::Reserve: return "reserve";
    case Refusal::Faulted: return "faulted";
    }
    return "?";
}
ScreenPass::ScreenPass(Read read) : read_(std::move(read)) {}
ScreenPass::ScreenPass(Read read, Hooks hooks) : read_(std::move(read)), hooks_(std::move(hooks)) {
    if (hooks_.enabled && (!hooks_.frame || !hooks_.reserve))
        throw std::invalid_argument("A selected screen pass requires the frame and reserve services");
}
bool ScreenPass::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || !read_) return false;
    auto original = capture(kDraw);
    auto free_slots = capture(kFreeTextureSlots);
    if (!original || !free_slots) return false;
    original_ = original;
    free_slots_ = free_slots;
    const auto active = std::make_shared<std::atomic<bool>>(false);
    bool accepted = false;
    try {
        accepted = install(kDraw, [this, active, original](CpuContext& c, uint32_t e) {
            if (!active->load(std::memory_order_acquire)) { original(c, e); return; }
            Run(c, e);
        });
    } catch (...) { original_ = {}; free_slots_ = {}; throw; }
    if (!accepted) { original_ = {}; free_slots_ = {}; return false; }
    active->store(true, std::memory_order_release);
    installed_ = true;
    return true;
}
void ScreenPass::Refuse(CpuContext& context, uint32_t entry, Refusal refusal) {
    ++counts_.refused;
    counts_.last_refusal = refusal;
    ++counts_.model_calls;
    ++active_;
    try { original_(context, entry); --active_; }
    catch (...) { --active_; throw; }
}
void ScreenPass::Run(CpuContext& context, uint32_t entry) {
    if (!installed_) throw std::logic_error("ScreenPass is not installed");
    // The body cannot yield (it only writes packets); a resumed interior has
    // no proved parameter set and is counted, never attributed or owned.
    if (entry && entry != kDraw) {
        ++counts_.interior;
        ++counts_.model_calls;
        ++active_;
        try { original_(context, entry); --active_; }
        catch (...) { --active_; throw; }
        return;
    }
    ++counts_.calls;
    Parameters parameters;
    const auto read = ReadParameters(read_, context, parameters);
    if (read != Result::Ok) ++counts_.unreadable;
    else if (!parameters.Any()) ++counts_.empty;
    else {
        ++counts_.effects;
        counts_.blur += parameters.blur; counts_.filter += parameters.filter; counts_.wave += parameters.wave;
    }
    if (!hooks_.enabled) {
        ++counts_.model_calls;
        ++active_;
        try { original_(context, entry); --active_; }
        catch (...) { --active_; throw; }
        return;
    }
    if (read != Result::Ok) return Refuse(context, entry, Refusal::Unreadable);
    // No switch: the source returns before any side effect.
    if (!parameters.Any()) return;
    Collect();
    if (failed_->load(std::memory_order_acquire)) return Refuse(context, entry, Refusal::Faulted);
    auto batch = std::make_shared<Batch>();
    auto& work = batch->work;
    work.screen.height = parameters.height;
    work.screen.blur = parameters.blur;
    if (parameters.filter) {
        const auto palette = ScreenPalettes::Resolve(read_, parameters.palette, parameters.second,
                                                     parameters.blends, parameters.amount, work.screen.palette);
        if (palette == ScreenPalettes::Result::Ok) work.screen.filter = true;
        else if (palette == ScreenPalettes::Result::Disabled) ++counts_.filter_off; // FilterColours returns
        else return Refuse(context, entry, Refusal::Palette);
    }
    if (parameters.wave) {
        if (ReadWaves(read_, work.waves) != Result::Ok) return Refuse(context, entry, Refusal::Waves);
        work.wave = true;
    }
    const bool draws = work.screen.blur || work.screen.filter || work.wave;
    if (draws) {
        if (!hooks_.frame(batch->frame)) return Refuse(context, entry, Refusal::Frame);
        if (batch->frame.width != parameters.width || batch->frame.height != parameters.height)
            return Refuse(context, entry, Refusal::Extent);
        batch->failed_ = failed_;
        // Allocate the producer's tracking before the root queue is touched.
        pending_.reserve(pending_.size() + 1);
        // RecordOwned false = "stay unselected for this call" (INTERFACES
        // section 8): the output queue is unchanged, so the call refuses.
        if (!hooks_.reserve(batch)) return Refuse(context, entry, Refusal::Reserve);
        pending_.push_back(batch);
    }
    counts_.last_palette = parameters.palette;
    counts_.last_blends = parameters.blends;
    counts_.last_height = parameters.height;
    // Committed: the source's keepTime-1 FreeTextureSlots(&g_TextureUploadContext, 1)
    // precedes its callees (effects.cpp:275); FinishScene frees again with
    // keepTime 0 after the materials, so the slot bookkeeping must see this call.
    ++counts_.replaced;
    context.cpuRegs.GPR.r[4].UD[0] = kTextureContext;
    context.cpuRegs.GPR.r[5].UD[0] = 1;
    ++active_;
    try { free_slots_(context, kFreeTextureSlots); --active_; }
    catch (...) { --active_; throw; }
}
void ScreenPass::Collect() {
    for (auto at = pending_.begin(); at != pending_.end();)
        at = (*at)->Complete() ? pending_.erase(at) : at + 1;
}
bool ScreenPass::CanCheckpoint(bool unlinked_recording_open) const {
    if (active_) return false;
    // A settled boundary with an open recording: this frame's reserved slot is
    // one of the recording's unlinked items, delivered when the pass completes
    // -- admissible, like a shadow bracket's Ready batches. A closed boundary
    // waits for every reservation to complete (delivered, failed or cancelled).
    for (const auto& batch : pending_)
        if (!batch->Complete() && !(unlinked_recording_open && batch->Ready())) return false;
    return true;
}
void ScreenPass::ResetAfterQuiescence() { pending_.clear(); }
ScreenPass::Counters ScreenPass::Counts() const {
    auto counts = counts_;
    counts.faults = failed_->load(std::memory_order_acquire);
    return counts;
}
}  // namespace title
