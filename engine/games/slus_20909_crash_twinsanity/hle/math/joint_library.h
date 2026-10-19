#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"

namespace title {

// Complete source animation operations. Selected entry adapters publish source
// results and scalar ABI effects; they do not reproduce discarded micro state.
class JointLibrary {
public:
    using Body = MathLibrary::Body;
    using CaptureBody = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;
    // Dispatches the installed binding of another source entry (the angle
    // decoder stays its own counted boundary). Defaults to the live table.
    using Call = std::function<void(CpuContext&, uint32_t)>;
    struct Counters {
        uint64_t rotations, generated_body_rotate, generated_interiors, unsupported_inputs;
        // Transform calls split into done returns and actually written matrices.
        // Positive claimed work requires written matrices, never done returns alone.
        uint64_t transforms, matrices_written, done_returns, generated_body_transform;
        // Animate frames published to the animator, those blended with the pose
        // (weight below one), angle decoder delegations and the captured original.
        uint64_t animations, blended_animations, angle_reads, generated_body_animate;
        // Per-entry refusals: the call ran the captured original instead (it is
        // also counted in that entry's generated_body_*), and why the owned
        // operation declined. A post-mutation fault parks the leg (never an abort).
        uint64_t refused_rotate, refused_transform, refused_animate;
        uint64_t refusal_state, refusal_frame, refusal_objects, refusal_channels, faults;
    };
    enum class TransformResult { Invalid, Done, Written };
    enum class AnimateResult { Invalid, Published, Blended };
    // Why the last Invalid/false result declined: entry register invariants
    // (r0, VI0, VF0, binding), the reserved stack frame (alignment, mapping,
    // aliasing a source object), an unmapped source object, or a track sample
    // cursor left unreadable (only reachable after the delegated decoder ran;
    // the entry context is restored before the refusal is reported).
    enum class Refusal { None, State, Frame, Objects, Channels };
    static constexpr uint32_t kRotate = 0x297b40;
    static constexpr uint32_t kTransform = 0x294300;
    static constexpr uint32_t kAnimate = 0x293ec0;
    static constexpr uint32_t kReadJointAngle = 0x297ba8;
    // Source default scale constants consumed when an animator has no scale.
    static constexpr uint32_t kDefaultScale = 0x3234f0;

    explicit JointLibrary(ps2r::ipu::GuestMemory& memory, Call call = {})
        : memory_(memory), call_(std::move(call)) {}
    ~JointLibrary();
    JointLibrary(const JointLibrary&) = delete;
    JointLibrary& operator=(const JointLibrary&) = delete;
    // Atomic three-entry batch: all originals are captured before publication;
    // a rejected or throwing publication restores pristine bodies in reverse.
    // Selected bodies never abort the process: an operation that declines its
    // inputs is counted and the captured original runs on the unchanged entry
    // context (restored anchors relaunch generated callers that reach these
    // entries with objects straddling the scratchpad mirror seam).
    bool Install(const CaptureBody& capture, const Installer& installer, bool selected);
    // The three reviewed callers consume VF1, preserve the ordinary scalar
    // calling convention, and reserve a disjoint aligned 32-byte callee frame.
    bool Rotate(CpuContext& context);
    // Source CreateJointTransform: a0 animator, a1 parent, a2 markDone,
    // a3 output. Done returns touch only the entry scalar ABI. Active calls
    // validate every source span before mutation, then publish the child
    // scale, flags, optional parent composition and four output rows.
    TransformResult Transform(CpuContext& context);
    // Source AnimateJoint: a0 animator, a1 animation data, f12 weight, f13
    // frame share. Reads the joint's track channels, delegates the three angle
    // decodes to the installed ReadJointAngle binding, blends scale, composes
    // the frame rotation (standard 818/a70) and publishes the frame to the
    // animator, blending with its pose when the weight is below one (1c0 plus
    // the source scalar scale/translation blends). Invalid leaves the context
    // and every source object unmodified (only the dead callee frame below the
    // caller's stack pointer may differ); a cursor the delegated decoder left
    // unreadable restores the entry context before declining.
    AnimateResult Animate(CpuContext& context);
    Refusal LastRefusal() const { return refusal_; }
    Counters Counts() const;
    std::string Census() const;

private:
    class CountsState;
    class Installation;
    class TransformSpans;
    class AnimateFrame;
    bool Mapped(uint32_t address, uint64_t size);
    bool Aliases(uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size);
    bool Decline(Refusal refusal);
    bool ValidateTransform(const CpuContext& context, TransformSpans& spans);
    void PublishScale(CpuContext& context, uint64_t flags, bool parent_present);
    bool ValidateAnimate(const CpuContext& context, AnimateFrame& frame);
    bool ReadChannels(CpuContext& context, uint32_t reader, uint32_t now, uint32_t next,
                      unsigned now_register, unsigned next_register, unsigned counter);
    void BlendScale(CpuContext& context);
    void BlendTranslation(CpuContext& context);
    void PublishFrame(CpuContext& context, uint32_t base, const JointMath::Vector& rotation);

    ps2r::ipu::GuestMemory& memory_;
    Call call_;
    Refusal refusal_ = Refusal::None;
    std::shared_ptr<CountsState> counts_;
    std::shared_ptr<Installation> installation_;
};

}  // namespace title
