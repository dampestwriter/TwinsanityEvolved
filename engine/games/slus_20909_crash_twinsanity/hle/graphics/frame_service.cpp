#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"
#include "engine/ee/timing/tick_profile.h"

#include <stdexcept>

namespace title {
namespace {
constexpr std::array<uint32_t,5> addresses{FrameIntent::kAddress,FrameService::kSend,
    FrameService::kSendInterrupt,FrameClear::kAddress,DisplayLayout::kAddress};
}
FrameService::FrameService(Hooks hooks) : hooks_(std::move(hooks)) {}
bool FrameService::Install(const Capture& capture,const Installer& install) {
    if (installed_ || !capture || !install ||
        (hooks_.enabled && (!hooks_.read || !hooks_.submitted || !hooks_.record))) return false;
    for (size_t i = 0; i < addresses.size(); ++i) {
        originals_[i] = capture(addresses[i]);
        if (!originals_[i]) return false;
    }
    for (size_t i = 0; i < addresses.size(); ++i)
        if (!install(addresses[i],[this,i](auto& context,auto at) { Run(i,context,at); })) {
            while (i) { --i; if (!install(addresses[i],originals_[i]))
                throw std::runtime_error("Graphics frame install rollback failed"); }
            return false;
        }
    installed_ = true;
    return true;
}

void FrameService::Refuse(GraphicsRefusals::Reason reason,uint32_t boundary,uint32_t detail) {
    ++counts_.refused;
    if (hooks_.refuse) hooks_.refuse(reason,boundary,detail);
}

void FrameService::Run(size_t index,CpuContext& context,uint32_t entry) {
    if (failed_) throw std::runtime_error("Graphics frame boundary previously failed");
    // The tick profile's GS-submit site: the frame record handed to the queue
    // (send), the clear and the layout; the setup and the interrupt are cheap.
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::GsSubmit);
    using Reason = GraphicsRefusals::Reason;
    const uint32_t boundary = addresses[index];
    auto& count = index == 0 ? counts_.setup : index == 1 ? counts_.send :
                  index == 2 ? counts_.interrupt : index == 3 ? counts_.clear : counts_.layout;
    const bool full = !entry || entry == boundary;
    const auto pointer = context.cpuRegs.GPR.r[4].UL[0];
    const bool frame_object = (pointer & 0x1fffffff) == FrameIntent::kSourceObject &&
        (pointer >> 29 == 0 || pointer >> 29 == 4 || pointer >> 29 == 5);
    if ((index == 1 || index == 2) && !frame_object) {
        ++counts_.foreign_sends;
        originals_[index](context,entry); // other source clients are not claimed retired
        return;
    }
    // Nothing below commits until a hook accepts: every early exit runs the
    // original boundary (counted as the model call) after attributing why.
    const auto refuse = [&](Reason reason,uint32_t detail) {
        Refuse(reason,boundary,detail);
        ++count.model_calls;
        originals_[index](context,entry);
    };
    if (!full) {
        ++counts_.interior_calls;
        if (hooks_.enabled) { refuse(Reason::Ordering,kInterior); return; }
    }
    ++count.calls;
    if (!hooks_.enabled) {
        ++count.model_calls;
        originals_[index](context,entry);
        return;
    }
    if (index == 4) {
        DisplayLayout::Output layout;
        if (!hooks_.layout || !plan_) { refuse(Reason::Ordering,kLayoutUnsupported); return; }
        if (DisplayLayout::Capture(hooks_.read,entry,layout) != DisplayLayout::Result::Ok ||
            layout.width != plan_->width || layout.height != plan_->height) {
            refuse(Reason::UnsupportedState,kLayoutUnsupported);
            return;
        }
        bool accepted = false;
        try { accepted = hooks_.layout(layout); }
        catch (...) { failed_ = true; ++counts_.failures; if (hooks_.fault) hooks_.fault(Reason::Ordering,boundary,kLayoutRefused); throw; }
        if (!accepted) { refuse(Reason::Ordering,kLayoutRefused); return; }
        ++count.replaced;
        return;
    }
    if (index == 3) {
        FrameEnvironment::SceneFrame frame;
        auto intent = std::make_shared<FrameClear::Output>();
        if (!hooks_.setup || !hooks_.clear || !hooks_.setup(frame)) { refuse(Reason::Ordering,kClearTarget); return; }
        if (FrameClear::Capture(hooks_.read,context,entry,*intent) != FrameClear::Result::Ok ||
            frame.width != intent->width || frame.height != intent->height ||
            frame.color_buffer != intent->draw.raster.frame.fbp ||
            frame.depth_buffer != intent->draw.raster.zbuf.zbp) {
            refuse(Reason::UnsupportedState,kClearTarget);
            return;
        }
        bool accepted = false;
        try { accepted = hooks_.clear(std::move(intent),frame); }
        catch (...) { failed_ = true; ++counts_.failures; if (hooks_.fault) hooks_.fault(Reason::Ordering,boundary,kClearRefused); throw; }
        if (!accepted) { refuse(Reason::Ordering,kClearRefused); return; }
        ++count.replaced;
        return;
    }
    if (index == 0) {
        auto plan = std::make_shared<FrameIntent::Output>();
        // Source display policy: complete scanlines. Alpha blend and fog are
        // disabled by this operation, so their inherited values are inactive.
        if (FrameIntent::Capture(hooks_.read,entry,{},*plan) != FrameIntent::Result::Ok) {
            plan_.reset(); // the sends of this head refuse too: no stale plan
            refuse(Reason::UnsupportedState,kSetupUnsupported);
            return;
        }
        plan_ = std::move(plan);
        ++count.replaced;
        return;
    }
    FrameEnvironment::SceneFrame frame;
    if (!plan_) { refuse(Reason::Ordering,kSendFrame); return; }
    if (!hooks_.submitted(frame)) {
        // TR renderer.cpp:436-440 (PresentFrame): Present — this Send of the
        // frame chain, frame.cpp:81-135 copy drawn->shown — runs BEFORE
        // StartNextFrame's SubmitBuckets (Link). The title's first Send thus
        // shows a drawn buffer no scene has been linked into; the owned display
        // already holds that reset state, so the boundary completes with no
        // copy, no model call and no pending owned work. After the first Link
        // a Send without a submitted scene is still a refusal.
        if (hooks_.linked && !hooks_.linked()) {
            ++count.replaced;
            ++counts_.empty_sends;
            return;
        }
        refuse(Reason::Ordering,kSendFrame);
        return;
    }
    if (frame.width != plan_->width || frame.height != plan_->height) {
        refuse(Reason::Ordering,kSendFrame);
        return;
    }
    bool accepted = false;
    try { accepted = hooks_.record(plan_,frame); }
    catch (...) { failed_ = true; ++counts_.failures; if (hooks_.fault) hooks_.fault(Reason::Ordering,boundary,kSendRefused); throw; }
    if (!accepted) { refuse(Reason::Ordering,kSendRefused); return; }
    ++count.replaced;
}

FrameService::Counters FrameService::Counts() const { return counts_; }
bool FrameService::CapturePlan(uint32_t& width,uint32_t& height) const {
    if (!installed_ || failed_) return false;
    width = plan_ ? plan_->width : 0;
    height = plan_ ? plan_->height : 0;
    return true;
}
bool FrameService::RestorePlan(uint32_t width,uint32_t height) {
    if (!installed_ || failed_ || bool(width) != bool(height)) return false;
    if (!width) { plan_.reset(); return true; }
    auto next = std::make_shared<FrameIntent::Output>();
    if (FrameIntent::Build(width,height,{},*next) != FrameIntent::Result::Ok) return false;
    plan_ = std::move(next);
    return true;
}
} // namespace title
