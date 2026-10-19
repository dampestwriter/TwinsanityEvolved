#include "games/slus_20909_crash_twinsanity/composition/graphics_strip.h"

namespace ps2r::app {
GraphicsStrip::GraphicsStrip(Capture capture,Record record,Refuse refuse)
    : capture_(std::move(capture)),record_(std::move(record)),refuse_(std::move(refuse)) {}

bool GraphicsStrip::Submit(::title::StripIntent::Output intent) {
    Frame frame;
    if (!capture_ || !record_ || !capture_(frame) ||
        !::title::GraphicsScene::CanDrawStrip(intent,frame)) return false;
    // Later source setup, Link or restore cannot replace this captured tuple.
    intent.draw.src_frame = frame.source_frame;
    const bool recorded = record_(intent.bucket,[this,intent=std::move(intent),frame](auto& scene,auto& retained) {
        using Result = ::title::GraphicsScene::Result;
        Result result;
        try { result = scene.DrawStrip(intent,frame); }
        catch (...) { ++failures_; throw; }
        if (result == Result::Ok) { ++completed_; return Outcome::Completed; }
        // The renderer ran and failed: committed, a fault. Every other result
        // is a pre-draw refusal of this slot's retained state.
        if (result == Result::DrawFailure) { ++failures_; return Outcome::Failed; }
        ++retained_;
        const uint32_t detail = kRetained | uint32_t(result);
        uint32_t none = 0;
        first_refusal_.compare_exchange_strong(none,detail);
        retained.draw = intent.draw;
        retained.attribute = [this,producer=intent.producer,detail] {
            if (refuse_) refuse_(producer,detail);
        };
        return Outcome::Retained;
    });
    if (recorded) ++captured_;
    return recorded;
}
GraphicsStrip::Counters GraphicsStrip::Counts() const {
    return {captured_.load(),completed_.load(),retained_.load(),failures_.load(),first_refusal_.load()};
}
} // namespace ps2r::app
