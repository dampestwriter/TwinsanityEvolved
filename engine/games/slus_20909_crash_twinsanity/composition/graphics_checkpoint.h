#pragma once

#include <functional>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

class StateRestorer;
namespace title { class GraphicsPresentation; class GraphicsDisplay; }
namespace ps2r::app {

// Required shared-surface section on the same fenced backend owner as draws.
// Image preflight stages complete allocations without changing live targets.
class GraphicsCheckpoint {
public:
    using Ordered = std::function<bool(std::function<bool()>)>;
    using Resize = std::function<void(uint32_t,uint32_t)>;
    GraphicsCheckpoint(::title::GraphicsScene&, Ordered,
                       ::title::GraphicsPresentation* = nullptr,::title::GraphicsDisplay* = nullptr,
                       Resize = {});
    // Before Register: see GraphicsOutput::SetLegacyImport.
    void SetLegacyImport(std::function<bool()> prepared) { if (!registered_) legacy_ = std::move(prepared); }
    bool Register(StateRestorer&);
    bool CanCapture(std::string* error);

private:
    ::title::GraphicsScene& scene_;
    Ordered ordered_;
    ::title::GraphicsPresentation* movie_ = nullptr;
    ::title::GraphicsDisplay* display_ = nullptr;
    Resize resize_;
    std::function<bool()> legacy_;
    uint32_t staged_width_ = 0, staged_height_ = 0;
    bool registered_ = false;
    bool RegisterPresentation(StateRestorer&);
};
} // namespace ps2r::app
