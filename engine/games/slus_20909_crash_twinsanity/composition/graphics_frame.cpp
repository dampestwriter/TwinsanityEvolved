#include "games/slus_20909_crash_twinsanity/composition/graphics_frame.h"

#include <cstring>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/composition/graphics_output.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_report.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

namespace ps2r::app {
namespace {
constexpr uint32_t kTag = 0x4e4c5046; // FPLN
uint32_t Word(const std::vector<uint8_t>& b,size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at+1])<<8 | uint32_t(b[at+2])<<16 | uint32_t(b[at+3])<<24;
}
bool Validate(const savestate::Blob& blob,std::string* error) {
    if (blob.version == 1 && blob.payload.size() == 8) {
        const auto w = Word(blob.payload,0),h = Word(blob.payload,4);
        ::title::FrameIntent::Output plan;
        if ((!w && !h) || ::title::FrameIntent::Build(w,h,{},plan) == ::title::FrameIntent::Result::Ok)
            return true;
    }
    if (error) *error = "Graphics display plan is malformed";
    return false;
}
}

GraphicsFrame::GraphicsFrame(::title::FrameService::Hooks hooks,
                             std::function<uint64_t()> completed_copies,
                             std::function<uint64_t()> completed_setups,
                             std::function<uint64_t()> completed_layouts)
    : selected_(hooks.enabled),refusals_(std::make_unique<::title::GraphicsRefusals>()),
      service_(Attributed(std::move(hooks),*refusals_)),
      completed_copies_(std::move(completed_copies)), completed_setups_(std::move(completed_setups)),
      completed_layouts_(std::move(completed_layouts)) {}

::title::FrameService::Hooks GraphicsFrame::Attributed(::title::FrameService::Hooks hooks,
                                                        ::title::GraphicsRefusals& ledger) {
    // The composition's attribution is the owner's: an explicit test sink is kept.
    if (!hooks.refuse) hooks.refuse = ledger.Refuser(::title::GraphicsRefusals::Family::Frame);
    if (!hooks.fault) hooks.fault = ledger.Faulter(::title::GraphicsRefusals::Family::Frame);
    return hooks;
}
void GraphicsFrame::SetFrameClock(::title::GraphicsRefusals::Clock host_frame) {
    refusals_->SetClocks(std::move(host_frame),source_clock_);
}

GraphicsFrame::GraphicsFrame(MainMemory& memory,GraphicsOutput& output,VideoStack& video,bool selected)
    : GraphicsFrame({selected,
        [&memory](uint32_t at,void* bytes,size_t size) {
            const auto* source = memory.Pointer(at,size);
            if (!source || !bytes) return false;
            std::memcpy(bytes,source,size);
            return true;
        },
        [&output](auto& frame) { return output.CaptureSubmittedScene(frame); },
        [&output,&video](auto intent,const auto& frame) {
            if (!video.Active()) return false;
            return output.EnqueueOwned([intent=std::move(intent),frame](VideoStack& target) {
                // Refused at the slot (the scene declined before the copy):
                // false hands the Send back to the source's own copy. Failed
                // after the copy began is the owned output's fault.
                using Resolution = ::title::GraphicsDisplay::Resolution;
                const auto resolution = target.ResolveDisplay(*intent,frame);
                if (resolution == Resolution::Failed)
                    throw std::runtime_error("Graphics display copy failed at its ordered slot");
                return resolution == Resolution::Ok;
            });
        },
        [&output](auto& frame) { return output.CaptureSetup(frame); },
        [this,&output,&video](auto intent,const auto& frame) {
            if (!video.Active() || !video.Scene()) return false;
            // StartFrame may refuse at the ordered slot (a retained-pipeline
            // state readable only there); the same captured clear draw then
            // takes the retained route at that slot instead of faulting.
            return output.RecordOwnedRetainable(0,[this,intent=std::move(intent),frame](
                    VideoStack& target,GraphicsOutput::OwnedRetained& retained) {
                using Result = ::title::GraphicsScene::Result;
                using Outcome = GraphicsOutput::OwnedOutcome;
                if (!target.Scene()) return Outcome::Failed;
                const auto result = target.Scene()->StartFrame(*intent,frame);
                if (result == Result::Ok) return Outcome::Completed;
                if (result == Result::DrawFailure) return Outcome::Failed;
                retained.draw = intent->draw;
                retained.draw.src_frame = frame.source_frame;
                retained.attribute = [this,result] {
                    ++retained_clears_;
                    refusals_->Refuse(::title::GraphicsRefusals::Family::Frame,
                        ::title::GraphicsRefusals::Reason::Ordering,::title::FrameClear::kAddress,
                        kClearRetained | uint32_t(result));
                };
                return Outcome::Retained;
            });
        },
        [&output](const auto& layout) {
            return output.EnqueueOwned([layout](VideoStack& video) { return video.ConfigureDisplay(layout); });
        },
        [&output] { return output.SceneLinked(); }},[&video] { return video.DisplayCopies(); },
        [&video] { return video.Scene() ? video.Scene()->FrameSetups() : 0; },
        [&video] { return video.Scene() ? video.Scene()->DisplayLayouts() : 0; }) {
    source_clock_ = [&output] { return output.SourceFrame(); };
    refusals_->SetClocks({},source_clock_);
}

bool GraphicsFrame::Install(const ::title::FrameService::Capture& capture,
                            const ::title::FrameService::Installer& install) {
    return service_.Install(capture,install);
}
bool GraphicsFrame::CanCheckpoint(std::string* error) const {
    uint32_t width,height;
    if (service_.CapturePlan(width,height)) return true;
    if (error) *error = "Graphics display producer has failed";
    return false;
}
bool GraphicsFrame::RegisterState(StateRestorer& restorer) {
    if (!selected_) return true;
    savestate::SectionSpec spec;
    spec.tag = kTag; spec.version = 1; spec.owner = "Graphics display plan";
    spec.required = !legacy_;
    spec.capture_required = true;
    if (legacy_) {
        spec.absent = [this] {
            if (!legacy_()) throw std::runtime_error("Graphics display plan section is missing without an admitted legacy import");
            if (!service_.RestorePlan(0,0)) throw std::runtime_error("Graphics display plan legacy reset failed");
        };
    }
    spec.validate = Validate;
    spec.serialize = [this](auto& bytes) {
        uint32_t w,h;
        if (!service_.CapturePlan(w,h)) throw std::runtime_error("Graphics display plan capture failed");
        bytes.clear();
        for (auto n : {w,h}) for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(n>>(8*i)));
    };
    spec.restore = [this](const savestate::Blob& blob,auto* error) {
        if (Validate(blob,error) && service_.RestorePlan(Word(blob.payload,0),Word(blob.payload,4))) return true;
        if (error) *error = "Graphics display plan publication failed";
        return false;
    };
    if (!restorer.Registry().Register(spec)) return false;
    restorer.AddImagePreflight([this](const auto& sections,auto* error) {
        const savestate::Blob* ram = nullptr;
        const savestate::Blob* plan = nullptr;
        const savestate::Blob* source = nullptr;
        for (const auto& blob : sections) {
            if (blob.tag == kTag) plan = &blob;
            if (blob.tag == savestate::TAG_RAM) ram = &blob;
            if (blob.tag == ::title::FrameState::kTag) source = &blob;
        }
        if (!plan && !source && legacy_ && legacy_()) return true;   // the admitted frozen image
        ::title::FrameEnvironment::Snapshot frame;
        if (!plan || !ram || !source || !Validate(*plan,error) ||
            !::title::FrameState::Decode(source->payload,frame,error)) return false;
        const auto w = Word(plan->payload,0), h = Word(plan->payload,4);
        // A completed source head cannot be restored with an omitted plan.
        if (!w) {
            if (!frame.dither_known && !frame.scene_valid && !frame.submitted_valid) return true;
            if (error) *error = "Graphics completed source frame requires its display plan";
            return false;
        }
        constexpr auto dimensions = ::title::MaterialState::kDisplayWidth;
        constexpr auto formats = ::title::MaterialState::kDisplayPixelBytes;
        if (ram->payload.size() >= dimensions+8 && ram->payload.size() >= formats+8 &&
            Word(ram->payload,dimensions) == w && Word(ram->payload,dimensions+4) == h &&
            Word(ram->payload,formats) == 2 && Word(ram->payload,formats+4) == 4) return true;
        if (error) *error = "Graphics display plan differs from incoming source layout";
        return false;
    });
    return true;
}
void GraphicsFrame::Record(verify::RunMeta& meta) const {
    const auto c = service_.Counts();
    ::title::GraphicsReport::Frame(meta, c);
    meta.Set("graphics_frame_other","foreign_sends=" + std::to_string(c.foreign_sends) +
        ",interior_calls=" + std::to_string(c.interior_calls) + ",failed=" + std::to_string(c.failures) +
        ",refused=" + std::to_string(c.refused) + ",empty_sends=" + std::to_string(c.empty_sends));
    refusals_->Record(meta,{::title::GraphicsRefusals::Family::Frame});
    meta.Set("graphics_frame_output","completed=" +
        std::to_string(completed_copies_ ? completed_copies_() : 0) + ",setups=" +
        std::to_string(completed_setups_ ? completed_setups_() : 0) + ",layouts=" +
        std::to_string(completed_layouts_ ? completed_layouts_() : 0) + ",retained_clears=" +
        std::to_string(retained_clears_.load()));
}
} // namespace ps2r::app
