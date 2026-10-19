#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_targets.h"

namespace title {

// Complete DrawIR plus explicit named-target bindings -> Backend. No GsState,
// transfer/parser/VU execution, target-address search or texture mirror.
// Scope: CT32 scene/UI, owned movie pictures/unlit mip textures, rendered sky.
class GraphicsRenderer {
public:
    enum class Result { Ok, InvalidDraw, InvalidBindings, UnsupportedTexture, BackendFailed };
    struct Counters { uint64_t draws = 0, rejected = 0, failures = 0; };
    struct Frame {
        // Supplied by the source frame owner / shared target's existing clear
        // decision, never inferred from a draw or added by this renderer.
        bool clear_depth = false, clear_color = false;
        uint32_t width = 0, height = 0; // logical viewport; zero uses allocation extent
        // Source WriteFrameHead only: explicit display quantization into an
        // owned output attachment. Not a general retained target/alias route.
        bool display_copy = false;
    };
    GraphicsRenderer(ps2r::render::Backend& backend, GraphicsResources& resources);
    // Immutable authored level count, including base, for an owned decoded
    // binding. Root's texture owner also guarantees upload-before-draw order.
    // Empty/zero supports the existing base-only Sky route; mip draws decline.
    void TextureLevels(std::function<uint8_t(const Graphics::Lease&)> levels);
    // Scoped lookup on the backend owner. Constructing/staging a texture
    // composition must never change another live owner's material lookup.
    bool WithTextureLevels(std::function<uint8_t(const Graphics::Lease&)> levels,
                           const std::function<bool()>& draw);
    // Call in Graphics bucket order on the backend owner. Bindings follow
    // GraphicsTargets::Slot. All state is explicit on every draw; no inherited
    // uniform mask, lazy alias inference, frame clear or presentation is added.
    Result Draw(const ps2r::gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings);
    Result Draw(const ps2r::gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings,
                const Frame& frame, ps2r::render::DrawResult* result);
    Counters Counts() const;

private:
    ps2r::render::Backend& backend_;
    GraphicsResources& resources_;
    Counters counts_;
    std::function<uint8_t(const Graphics::Lease&)> texture_levels_;
};

}  // namespace title
