#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_material.h"

namespace title {

// Owner of the distortion material's settings version. The source makes the
// type18 material once per particle-system initialisation
// (InitParticleSystemsForLevel 0x1b3a70 inlines MakeDistortionMaterial): it
// allocates a fresh shader object, sets its attributes, calls the type18
// settings virtual (NTSC 0x1d5f90) and only THEN writes the linear filter and
// LOD K/L fields. The GS settings the original packet carried are therefore
// the object's state at the settings call, not at any later draw. This
// observer captures that state at the call and versions it by the shader
// object, so DistortionPass can refuse a material whose live shader was never
// observed (a missed constructor, a restored checkpoint without this payload).
// It replaces no game operation: the original settings body still runs.
class DistortionSettings {
public:
    using Read = MaterialState::Read;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    static constexpr uint32_t kCall = DistortionMaterial::kSettings;
    struct Snapshot {
        uint32_t version = 1, shader = 0;
        uint64_t generation = 0;
        DistortionMaterial::Settings settings;
    };
    struct Counters {
        uint64_t calls = 0, captured = 0, rejected = 0, interior = 0;
    };
    explicit DistortionSettings(Read);
    bool Install(const Capture&, const Installer&);
    // The newest captured version, only if it belongs to this shader object.
    bool Current(uint32_t shader, DistortionMaterial::Settings& out, uint64_t* generation = nullptr) const;
    // Value state for the checkpoint owner. Restore replaces the whole version.
    bool CaptureState(Snapshot&) const;
    static bool Validate(const Snapshot&);
    bool Restore(const Snapshot&);
    bool CanCheckpoint() const { return active_ == 0 && !counts_.interior; }
    Counters Counts() const { return counts_; }
private:
    Read read_;
    Body original_;
    Snapshot state_;
    Counters counts_;
    uint32_t active_ = 0;
    bool installed_ = false;
    void Run(CpuContext&, uint32_t entry);
};

}  // namespace title
