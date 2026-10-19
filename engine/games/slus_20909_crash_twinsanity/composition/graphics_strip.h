#pragma once

#include <atomic>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_intent.h"

namespace ps2r::app {

// Producer-time source capture and immutable All-mode Strip delivery. The
// composition root supplies its existing bucket queue and backend owner.
//
// Delivery outcome at the ordered output slot: Completed = the owned scene
// drew the strip. Retained = the scene REFUSED before drawing anything (a
// retained-pipeline state readable only on the consumer at that slot: target
// growth, aliased/CPU-dirty colour pages, a pending RT-sync entry, an
// unfinished stencil sequence, a Zsrc route); the delivering owner then
// submits the SAME captured draw through the retained route at that slot
// (pixels identical to the Plain control) and runs `attribute` on its own
// thread. Refused = the scene refused before drawing and there is no retained
// draw to submit: the slot is released, the caller attributes it (never a
// Strip outcome: a strip always has its captured draw). Failed = the renderer
// failed after commitment: a fault.
class GraphicsStrip {
public:
    using Frame = ::title::FrameEnvironment::SceneFrame;
    enum class Outcome { Completed, Retained, Refused, Failed };
    struct Retained {
        ps2r::gs::DrawIr draw;
        std::function<void()> attribute;
    };
    using Work = std::function<Outcome(::title::GraphicsScene&,Retained&)>;
    using Capture = std::function<bool(Frame&)>;
    using Record = std::function<bool(uint32_t,Work)>;
    // Attribution of a retained delivery: (producer entry, kRetained | Result).
    using Refuse = std::function<void(uint32_t producer,uint32_t detail)>;
    static constexpr uint32_t kRetained = 0x200;
    struct Counters {
        uint64_t captured = 0, completed = 0, retained = 0, failures = 0;
        uint32_t first_refusal = 0; // kRetained | the first refusing GraphicsScene::Result, or 0
    };
    GraphicsStrip(Capture,Record,Refuse refuse = {});
    bool Submit(::title::StripIntent::Output);
    Counters Counts() const;

private:
    Capture capture_;
    Record record_;
    Refuse refuse_;
    std::atomic<uint64_t> captured_{0}, completed_{0}, retained_{0}, failures_{0};
    std::atomic<uint32_t> first_refusal_{0};
};
} // namespace ps2r::app
