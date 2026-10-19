#pragma once

#include "games/slus_20909_crash_twinsanity/composition/graphics_producers.h"

class StateRestorer;
namespace ps2r::verify { class RunMeta; }
namespace ps2r::app {
class GraphicsImport;

// Application owner for selected resources and producers. Dispatch wrappers
// refer to this stable owner, so a validated restore can publish a fresh,
// matching texture/binding/producer composition without recapturing wrappers.
class GraphicsRuntime {
public:
    using Options = GraphicsProducers::Options;
    using Body = GraphicsProducers::Body;
    using Capture = GraphicsProducers::Capture;
    using Installer = GraphicsProducers::Installer;
    // `scratchpad`: the title's stack window (GraphicsProducers::Window); the
    // texture owner reads the Stream a TextureRead receives there. `arena`:
    // the root's extra physical region at GraphicsProducers::kArenaBase (the
    // META arena) where the DiskManager nodes the sky owner's load-boundary
    // conversion walks live under hle.meta_arena.
    using Window = GraphicsProducers::Window;
    GraphicsRuntime(MainMemory&, GraphicsOutput&, VideoStack&, Options, Window scratchpad = {},
                    Window arena = {});
    GraphicsRuntime(MainMemory&, ::title::GraphicsResources&, ::title::GraphicsRenderer&,
                    ::title::GraphicsScene&, render::Backend&, Options, GraphicsProducers::Output,
                    Window scratchpad = {}, Window arena = {});
    ~GraphicsRuntime();
    GraphicsRuntime(const GraphicsRuntime&) = delete;
    GraphicsRuntime& operator=(const GraphicsRuntime&) = delete;
    bool Install(const Capture&, const Installer&, ::title::TextureBindings::Deallocate);
    // Host leg frame clock for refusal attribution (VerifyHost boundaries);
    // the source Link ordinal clock comes from GraphicsOutput. Optional.
    void SetFrameClock(::title::GraphicsRefusals::Clock host_frame);
    // Process-lifetime refusal/fault ledger of the Textures/Sprites/Text/Sky
    // families (survives restore owner swaps; published by Record).
    const ::title::GraphicsRefusals& Refusals() const;
    // Before RegisterState: the explicit legacy import. With it attached the
    // Graphics sections are optional ONLY for an image the import admits
    // (frozen schema + inventoried signature); its derived TXAS and the
    // active selection publish at the GXSL position, after RAM0.
    void SetImport(GraphicsImport* import);
    bool RegisterState(StateRestorer&);
    bool CanCheckpoint(std::string* reason = nullptr) const;
    ::title::MovieGraphics::OutputServices MovieOutput();
    void Record(verify::RunMeta&) const;
    ::title::TextureAssets::Counters TextureCounts() const;
    GraphicsProducers::Counters ProducerCounts() const;
    uint64_t Restores() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
