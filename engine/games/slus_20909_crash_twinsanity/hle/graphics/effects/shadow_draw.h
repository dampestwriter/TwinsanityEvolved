#pragma once

#include <cstdint>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// Source StartShadows -> type0B volume strips -> ApplyShadows, as explicit
// ordered draws. Coordinates are logical pixels; sources/targets are named
// by the pass renderer, never looked up by an address or register snapshot.
class ShadowDraw {
public:
    enum class Step { Clear, SeedAlpha, CopyDepth, InvertDepth, Volume, Quantize, Composite };
    enum class Result { Ok, InvalidFrame, InvalidVolume, LimitExceeded };
    struct Frame { uint32_t number = 0, width = 0, height = 0; };
    struct Command {
        Step step;
        ps2r::gs::DrawIr draw;
    };
    static constexpr size_t kMaxVertices = 1024 * 1024;

    // Each volume is ONE original source strip or clipped fan, expanded to
    // triangles by the type0B geometry owner. Preserve that grouping: the
    // shadow stencil counts once per touched pixel per strip, not per batch.
    // The only live geometry inputs are projected xyz and the source +/-8
    // winding colours. Retained DrawIr state is ignored and rebuilt here.
    static Result Build(const Frame&, const std::vector<ps2r::gs::DrawIr>& volumes,
                        std::vector<Command>& out);

    // Source byte arithmetic; the 16-bit accumulator has a 5-bit counter.
    static uint8_t CounterByte(int32_t winding);
    static uint8_t QuantizedByte(int32_t winding);
    static uint8_t Darken(uint8_t scene, uint8_t mask);
};

}  // namespace title
