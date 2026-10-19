#pragma once

#include "games/slus_20909_crash_twinsanity/composition/graphics_producers.h"

namespace ps2r::app {

// Per-process execution totals survive resource-owner publication. They are
// evidence of actual calls, never saved/replayed work or completion credit.
class GraphicsCensus {
public:
    void Retire(const GraphicsProducers::Counters&, const ::title::TextureBindings::Counters&,
                const ::title::TextureAssets::Counters&);
    GraphicsProducers::Counters Producers(GraphicsProducers::Counters current) const;
    ::title::TextureBindings::Counters Bindings(::title::TextureBindings::Counters current) const;
    ::title::TextureAssets::Counters Assets(::title::TextureAssets::Counters current) const;

private:
    GraphicsProducers::Counters producers_{};
    ::title::TextureBindings::Counters bindings_{};
    ::title::TextureAssets::Counters assets_{};
};

}  // namespace ps2r::app
