#pragma once

#include <atomic>

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"

class MainMemory;
class StateRestorer;
namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {
class GraphicsOutput;
class VideoStack;

// Application composition of source display plans and ordered owned output.
// Ordinary/interrupt Present retain their existing pacing and display setup;
// their frame-copy sends reach FrameService instead of the transport model.
class GraphicsFrame {
public:
    explicit GraphicsFrame(::title::FrameService::Hooks,
                           std::function<uint64_t()> completed_copies = {},
                           std::function<uint64_t()> completed_setups = {},
                           std::function<uint64_t()> completed_layouts = {});
    GraphicsFrame(MainMemory&,GraphicsOutput&,VideoStack&,bool selected);
    bool Install(const ::title::FrameService::Capture&,const ::title::FrameService::Installer&);
    bool RegisterState(StateRestorer&);
    bool CanCheckpoint(std::string* error) const;
    void Record(verify::RunMeta&) const;
    void SetFrameClock(::title::GraphicsRefusals::Clock host_frame);
    // Before RegisterState: see GraphicsOutput::SetLegacyImport (the display
    // plan of an admitted frozen image is none until the next source head).
    void SetLegacyImport(std::function<bool()> prepared) { legacy_ = std::move(prepared); }
    const ::title::GraphicsRefusals& Refusals() const { return *refusals_; }
    // Detail of a SetUpFrame clear the owned scene refused AT ITS ORDERED SLOT
    // (GraphicsScene::StartFrame non-Ok before drawing): kClearRetained |
    // GraphicsScene::Result. The same captured clear draw then takes the
    // retained route at that slot (GraphicsOutput::RecordOwnedRetainable).
    static constexpr uint32_t kClearRetained = 0x200;

private:
    static ::title::FrameService::Hooks Attributed(::title::FrameService::Hooks, ::title::GraphicsRefusals&);
    bool selected_ = false;
    std::function<bool()> legacy_;
    std::unique_ptr<::title::GraphicsRefusals> refusals_; // constructed before service_ (hooks sink into it)
    ::title::GraphicsRefusals::Clock source_clock_;
    ::title::FrameService service_;
    std::function<uint64_t()> completed_copies_;
    std::function<uint64_t()> completed_setups_;
    std::function<uint64_t()> completed_layouts_;
    std::atomic<uint64_t> retained_clears_{0};
};
} // namespace ps2r::app
