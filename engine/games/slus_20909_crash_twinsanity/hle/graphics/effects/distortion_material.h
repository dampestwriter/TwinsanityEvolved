#pragma once

#include <array>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

class DistortionMaterial {
public:
    static constexpr uint32_t kVtable = 0x2fa348, kSettings = 0x1d5f90;
    struct Settings {
        uint64_t attributes = 0;
        int16_t lod_k = 0, lod_l = 0;
    };
    enum class Result { Ok, InvalidMemory, UnsupportedSettings, UnsupportedTarget };
    // Capture at the SOURCE SETTINGS CALL, not the later drawing call. The
    // procedural constructor changes linear/K/L after making its settings.
    // The material owner must retain this value with its object generation.
    static Result CaptureSettings(const MaterialState::Read&, uint32_t shader, Settings&);
    // The proved procedural attribute family (first-context Gouraud, no fog).
    static bool Supported(const Settings&);
    // Explicit frame/target values and the captured settings version. The
    // ordered copy precedes this draw, so its global TEXA and dither writes
    // are included. No original settings/packet allocation is read.
    static Result Build(const Settings&, const MaterialState::Frame&,
                        const std::array<uint32_t, 8>& target, uint32_t source_frame,
                        ps2r::gs::DrawIr& out);
};

}  // namespace title
