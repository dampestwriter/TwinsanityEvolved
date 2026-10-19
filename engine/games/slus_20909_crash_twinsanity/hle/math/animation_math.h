#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

// Source animation math. This stage owns compressed angle decoding only;
// Euler interpolation, slerp and joint composition still have other consumers.
class AnimationMath {
public:
    using GuestMemory = MathLibrary::GuestMemory;
    using Body = MathLibrary::Body;
    using Capture = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;

    struct Angles {
        float current;
        float next;
    };
    struct Counters {
        uint64_t angle_pairs;
        uint64_t generated_continuations;
        uint64_t unsupported_inputs;
        uint64_t generated_body_calls;
    };

    static constexpr uint32_t kReadJointAngleAddress = 0x00297ba8;
    explicit AnimationMath(GuestMemory& memory);
    ~AnimationMath();
    // false executes/counts the pristine original; true replaces supported
    // full entries. Counter increments immediately before actual original
    // execution. Capture must not return an installed wrapper.
    // One successful batch per owner; teardown requires quiescent dispatch.
    // Retained callbacks forward to originals after destruction.
    bool Install(const Capture& capture, const Installer& installer, bool replace_helpers = true);

    // Signed compressed samples represent 16 angle units each. Wrap the first
    // sample by exactly one revolution only beyond the strict half-turn ties.
    // A static channel consumes only current and repeats its decoded value.
    static Angles DecodeAngles(int16_t current, int16_t next, bool is_static = false);

    // Complete NTSC entry: a0 TrackReader, a1 current output, a2 next output.
    // Source: game/animation.cpp ReadJointAngle; generated shard 027 is ABI.
    // Preserves halfword flag shifts, per-channel cursor advancement and
    // ordered stack/output writes, including aliased reader/output data.
    void ReadJointAngle(CpuContext& context);
    Counters Counts() const;
    std::string Census() const;

private:
    bool CanReplace(const CpuContext& context);
    uint16_t Read16(uint32_t address);
    void Write16(uint32_t address, uint16_t value);
    GuestMemory& memory_;
    std::atomic<uint64_t> calls_{0};
    std::shared_ptr<std::atomic<uint64_t>> continuations_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> installation_;
    std::atomic<uint64_t> unsupported_{0};
    std::shared_ptr<std::atomic<uint64_t>> generated_body_calls_;
};

}  // namespace title
