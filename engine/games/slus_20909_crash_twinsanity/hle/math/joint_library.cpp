#include "games/slus_20909_crash_twinsanity/hle/math/joint_library.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.h"

namespace title {

bool JointLibrary::Mapped(uint32_t address, uint64_t size) {
    return GuestSpan<ps2r::ipu::GuestMemory>::Mapped(memory_, address, size);
}

bool JointLibrary::Aliases(uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size) {
    return GuestSpan<ps2r::ipu::GuestMemory>::Aliases(memory_, a, a_size, b, b_size);
}

bool JointLibrary::Decline(Refusal refusal) {
    refusal_ = refusal;
    return false;
}

bool JointLibrary::Rotate(CpuContext& context) {
    // The source entry assumes the established zero integer register. The
    // retained NEW body reads it in its first mask setup before pinning it.
    // Refuse corrupt entry state rather than re-create that model behavior.
    if (context.vuRegs.VI[0] != 0) return Decline(Refusal::State);
    using Span = GuestSpan<ps2r::ipu::GuestMemory>;
    auto& regs = context.cpuRegs.GPR.r;
    const uint64_t zero = regs[0].UD[0];
    const uint64_t pose_value = regs[4].UD[0] + zero;
    const uint32_t pose = static_cast<uint32_t>(pose_value);
    const uint32_t stack = regs[29].UL[0];
    if ((stack & 15u) != 0 || stack < 32 || !Mapped(stack - 32, 32)) return Decline(Refusal::Frame);
    const uint32_t scratch = stack - 32;
    if (!Mapped(pose, 8)) return Decline(Refusal::Objects);
    if (Aliases(pose, 8, scratch, 32)) return Decline(Refusal::Frame);
    uint64_t fields;
    Span::Read(memory_, pose, &fields, sizeof(fields));
    const bool current = (fields & 1) != zero;
    uint32_t from_address;
    if (current) {
        if (pose > UINT32_MAX - 16) return Decline(Refusal::Objects);
        from_address = (pose + 16) & ~15u;
    } else {
        uint32_t joint;
        Span::Read(memory_, pose + 4, &joint, sizeof(joint));
        if (joint > UINT32_MAX - 16) return Decline(Refusal::Objects);
        from_address = (joint + 16) & ~15u;
    }
    if (!Mapped(from_address, 16)) return Decline(Refusal::Objects);
    if (Aliases(from_address, 16, scratch, 32)) return Decline(Refusal::Frame);
    JointMath::Vector from, to;
    Span::Read(memory_, from_address, from.data(), 16);
    std::memcpy(to.data(), context.vuRegs.VF[1].F, 16);
    const uint32_t share = context.fpuRegs.fpr[12].UL;
    const auto interpolated = JointMath::Interpolate(from, to, context.fpuRegs.fpr[12].f);

    // Source flag publication and the scalar NTSC entry ABI. The reserved
    // frame is disjoint from source objects in all three reviewed callers.
    regs[5].UD[0] = pose_value;
    regs[1].SD[0] = 0x3f800000;
    context.fpuRegs.fpr[0].UL = 0x3f800000;
    regs[4].UD[0] = fields;
    const uint32_t parameters[]{share, share, share, 0x3f800000};
    Span::Write(memory_, scratch, parameters, sizeof(parameters));
    if (current) {
        std::memcpy(&regs[2], from.data(), 16);
    } else {
        const uint32_t joint = static_cast<uint32_t>(fields >> 32);
        regs[2].SD[0] = static_cast<int32_t>(joint);
        regs[3].UD[0] = fields |
            static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(zero) + 1)));
        std::memcpy(&regs[4], from.data(), 16);
        Span::Write(memory_, pose, &regs[3].UD[0], sizeof(fields));
    }
    Span::Write(memory_, scratch + 16, from.data(), 16);
    regs[29].SD[0] = static_cast<int32_t>(stack);
    context.vuRegs.VF[0] = {};
    context.vuRegs.VF[0].F[3] = 1;
    std::memcpy(context.vuRegs.VF[31].F, from.data(), 16);
    std::memcpy(context.vuRegs.VF[1].F, interpolated.rotation.data(), 16);
    return true;
}

// Source object spans validated before any CreateJointTransform mutation.
// Scalar fields are read unrounded; quadword fields round down like the
// original loads, so low animator/animation alignments stay covered.
class JointLibrary::TransformSpans {
public:
    uint32_t stack = 0, animator = 0, animation = 0, parent = 0, output = 0;
    bool parent_present = false;
    uint32_t frame = 0;
};

namespace {
using Span = GuestSpan<ps2r::ipu::GuestMemory>;
// Field accesses walk guest quads (validated spans never fail here).
template<class T> T Load(ps2r::ipu::GuestMemory& memory, uint32_t address) {
    T value{};
    Span::Read(memory, address, &value, sizeof(value));
    return value;
}
template<class T> void Store(ps2r::ipu::GuestMemory& memory, uint32_t address, const T& value) {
    Span::Write(memory, address, &value, sizeof(value));
}
JointMath::Vector LoadQuad(ps2r::ipu::GuestMemory& memory, uint32_t address) {
    return Load<JointMath::Vector>(memory, address & ~15u);
}
void StoreQuad(ps2r::ipu::GuestMemory& memory, uint32_t address, const JointMath::Vector& value) {
    Store(memory, address & ~15u, value);
}
void SetVector(GPR_reg& reg, const JointMath::Vector& value) { std::memcpy(&reg, value.data(), 16); }
// Object pointers reloaded after mutation must still map; the source would
// fault on an unmapped read and no model is substituted.
void Require(ps2r::ipu::GuestMemory& memory, uint32_t address, size_t size) {
    if (!Span::Mapped(memory, address, size)) throw std::runtime_error("JointLibrary: source object left unmapped");
}
void LoadRegister(GPR_reg& reg, ps2r::ipu::GuestMemory& memory, uint32_t address) {
    Require(memory, address & ~15u, 16);
    Span::Read(memory, address & ~15u, &reg, 16);
}
void SetVector(VR_reg& reg, const JointMath::Vector& value) { std::memcpy(reg.F, value.data(), 16); }
}  // namespace

bool JointLibrary::ValidateTransform(const CpuContext& context, TransformSpans& spans) {
    auto& regs = context.cpuRegs.GPR.r;
    spans.stack = regs[29].UL[0];
    spans.animator = regs[4].UL[0];
    spans.parent = regs[5].UL[0];
    spans.parent_present = regs[5].UD[0] != 0;  // The source compares the whole register.
    spans.output = regs[7].UL[0];
    if ((spans.stack & 15u) != 0 || spans.stack < 16) return Decline(Refusal::Frame);
    spans.frame = spans.stack - 16;
    if (!Mapped(spans.frame, 16)) return Decline(Refusal::Frame);
    const uint32_t pose_base = spans.animator & ~15u;
    if (!Mapped(pose_base, 0x40)) return Decline(Refusal::Objects);
    spans.animation = Load<uint32_t>(memory_, spans.animator + 8);
    const uint32_t animation_base = spans.animation & ~15u;
    const uint64_t animation_size = 0x58u + (spans.animation & 15u);
    const uint32_t output_base = spans.output & ~15u;
    if (!Mapped(animation_base, animation_size) || !Mapped(output_base, 0x40)) return Decline(Refusal::Objects);
    if (spans.parent_present) {
        const uint32_t parent_base = spans.parent & ~15u;
        if (!Mapped(parent_base, 0x50)) return Decline(Refusal::Objects);
        if (Aliases(spans.frame, 16, parent_base, 0x50)) return Decline(Refusal::Frame);
    }
    // The reserved callee frame belongs to the source caller's stack; it never
    // aliases a source object or the output. Objects may alias each other.
    if (Aliases(spans.frame, 16, pose_base, 0x40) || Aliases(spans.frame, 16, animation_base, animation_size) ||
        Aliases(spans.frame, 16, output_base, 0x40)) return Decline(Refusal::Frame);
    return true;
}

void JointLibrary::PublishScale(CpuContext& context, uint64_t flags, bool parent_present) {
    // The scale children inherit: own, else parent's, else four ones built in
    // the reserved frame. The source orders the bits read, scale write and
    // bits write differently on the ones branch; aliases must observe that.
    auto& regs = context.cpuRegs.GPR.r;
    const uint32_t animator = regs[8].UL[0], parent = regs[9].UL[0], stack = regs[29].UL[0] - 16;
    regs[5].SD[0] = 1;
    const uint32_t animation = Load<uint32_t>(memory_, animator + 8);
    regs[4].SD[0] = static_cast<int32_t>(animation);
    JointMath::Vector scale;
    if ((flags & 4) != 0 || parent_present) {
        scale = LoadQuad(memory_, (flags & 4) != 0 ? animator + 0x30 : parent + 0x40);
        uint64_t bits = Load<uint64_t>(memory_, animation + 0x50);
        StoreQuad(memory_, animation + 0x40, scale);
        bits |= 1;
        Store(memory_, animation + 0x50, bits);
    } else {
        regs[1].SD[0] = 0x3f800000;
        context.fpuRegs.fpr[0].UL = 0x3f800000u;
        for (uint32_t offset : {0xcu, 0x0u, 0x4u, 0x8u}) Store<uint32_t>(memory_, stack + offset, 0x3f800000u);
        uint64_t bits = Load<uint64_t>(memory_, animation + 0x50);
        scale = LoadQuad(memory_, stack);
        bits |= 1;
        Store(memory_, animation + 0x50, bits);
        StoreQuad(memory_, animation + 0x40, scale);
    }
    SetVector(regs[3], scale);
}

JointLibrary::TransformResult JointLibrary::Transform(CpuContext& context) {
    using Vector = JointMath::Vector;
    auto& regs = context.cpuRegs.GPR.r;
    const auto& constant = context.vuRegs.VF[0];
    // Canonical zero register, VI0 and VF0 are source-entry invariants; the
    // original reads them raw, so corrupt state is refused, never modeled.
    if (regs[0].UD[0] != 0 || context.vuRegs.VI[0] != 0 || constant.I[0] != 0 ||
        constant.I[1] != 0 || constant.I[2] != 0 || constant.I[3] != 0x3f800000u) {
        Decline(Refusal::State);
        return TransformResult::Invalid;
    }
    const uint32_t animator = regs[4].UL[0];
    if (!Mapped(animator, 8)) {
        Decline(Refusal::Objects);
        return TransformResult::Invalid;
    }
    const uint64_t flags = Load<uint64_t>(memory_, animator);
    TransformSpans spans;
    const bool done = (flags & 0x20) != 0;
    if (!done && !ValidateTransform(context, spans)) return TransformResult::Invalid;
    // Entry scalar ABI shared by both outcomes.
    regs[8].UD[0] = regs[4].UD[0];
    regs[9].UD[0] = regs[5].UD[0];
    regs[3].UD[0] = flags;
    regs[2].UD[0] = flags & 0x20;
    regs[29].SD[0] = static_cast<int32_t>(regs[29].UL[0]);
    if (done) return TransformResult::Done;

    JointMath::Matrix matrix;
    if ((flags & 1) != 0) {
        Vector parent_scale{1, 1, 1, 1};
        if ((flags & 8) != 0 && spans.parent_present) parent_scale = LoadQuad(memory_, spans.parent + 0x40);
        const Vector rotation = LoadQuad(memory_, animator + 0x10);
        const bool scaled = (Load<uint64_t>(memory_, animator) & 4) != 0;
        const Vector own = scaled ? LoadQuad(memory_, animator + 0x30) : Vector{};
        const auto basis = JointMath::RotationBasis(rotation, parent_scale, scaled ? &own : nullptr);
        std::copy(basis.begin(), basis.end(), matrix.begin());
    } else {
        // The source identity comes from macro self-subtraction in the caller's
        // rounding mode; row 2 copies the constant register's raw positive zeros.
        volatile float one = 1.0f;
        const float zero = one - one;
        matrix[0] = {1, zero, zero, zero};
        matrix[1] = {zero, 1, zero, zero};
        matrix[2] = {0, 0, 1, 0};
    }
    PublishScale(context, Load<uint64_t>(memory_, animator), spans.parent_present);
    const uint64_t reloaded = Load<uint64_t>(memory_, animator);
    regs[10].UD[0] = reloaded;
    matrix[3] = (reloaded & 2) != 0 ? LoadQuad(memory_, animator + 0x20) : Vector{0, 0, 0, 1};
    if (spans.parent_present && (reloaded & 0x10) == 0) {
        JointMath::Matrix parent;
        parent[3] = LoadQuad(memory_, spans.parent + 0x30);
        parent[1] = LoadQuad(memory_, spans.parent + 0x10);
        parent[2] = LoadQuad(memory_, spans.parent + 0x20);
        parent[0] = LoadQuad(memory_, spans.parent);
        for (unsigned row = 0; row < 4; ++row) {
            SetVector(regs[2 + row], parent[row]);
            SetVector(context.vuRegs.VF[5 + row], parent[row]);
        }
        matrix = JointMath::MultiplyByParent(matrix, parent);
    }
    regs[2].SD[0] = 0x20;
    if (regs[6].UD[0] != 0) {
        regs[2].UD[0] = reloaded | 0x20;
        Store(memory_, animator, reloaded | 0x20);
    }
    for (unsigned row = 0; row < 4; ++row) {
        StoreQuad(memory_, spans.output + row * 16, matrix[row]);
        SetVector(context.vuRegs.VF[1 + row], matrix[row]);
    }
    return TransformResult::Written;
}

// AnimateJoint's reserved frame and the source objects it reads before any
// mutation. The frame belongs to the source caller's stack and is disjoint
// from every object; the delegated decoder owns the 0x30 bytes below it.
class JointLibrary::AnimateFrame {
public:
    static constexpr uint32_t kSize = 0xf0, kDecoderFrame = 0x30;
    uint32_t base = 0, animator = 0, joint = 0, data = 0, settings = 0;
    uint16_t statics = 0, mask = 0;
    uint32_t static_values = 0, current = 0, next = 0;
};

namespace {
float Bits(uint32_t value) { float f; std::memcpy(&f, &value, 4); return f; }
int64_t Extend(uint32_t value) { return static_cast<int32_t>(value); }
// cvt.s.w of a sign-extended halfword sample scaled by the channel unit.
float Channel(int16_t sample) {
    return title::JointMath::ScalarProduct(static_cast<float>(sample), Bits(0x39800000u));
}
}  // namespace

bool JointLibrary::ValidateAnimate(const CpuContext& context, AnimateFrame& frame) {
    auto& regs = context.cpuRegs.GPR.r;
    const uint32_t stack = regs[29].UL[0];
    frame.base = stack - AnimateFrame::kSize;
    if ((stack & 15u) != 0 || stack < AnimateFrame::kSize + AnimateFrame::kDecoderFrame) return Decline(Refusal::Frame);
    // The owned frame plus the delegated decoder's frame below it.
    const uint32_t frame_base = frame.base - AnimateFrame::kDecoderFrame;
    const uint64_t frame_size = AnimateFrame::kSize + AnimateFrame::kDecoderFrame;
    if (!Mapped(frame_base, frame_size)) return Decline(Refusal::Frame);
    const auto object = [&](uint32_t address, uint64_t size) {
        if (!Mapped(address, size)) return Decline(Refusal::Objects);
        if (Aliases(frame_base, frame_size, address, size)) return Decline(Refusal::Frame);
        return true;
    };
    frame.animator = regs[4].UL[0];
    frame.data = regs[5].UL[0];
    if (!object(frame.animator & ~15u, 0x50) || !object(frame.data, 0x14)) return false;
    frame.joint = Load<uint32_t>(memory_, frame.animator + 4);
    if (!object(frame.joint & ~15u, 0x70)) return false;
    const uint32_t index = Load<uint8_t>(memory_, frame.joint + 0x51);
    frame.settings = Load<uint32_t>(memory_, frame.data + 4) + index * 8;
    if (!object(frame.settings, 8)) return false;
    frame.statics = Load<uint16_t>(memory_, frame.settings + 2);
    const unsigned channels = (Load<uint16_t>(memory_, frame.settings) >> 8) & 0xf;
    frame.mask = static_cast<uint16_t>((1u << channels) - 1u);
    frame.static_values = Load<uint32_t>(memory_, frame.data + 8) + Load<uint16_t>(memory_, frame.settings + 4) * 2u;
    const uint32_t advance = Load<uint16_t>(memory_, frame.settings + 6) * 2u;
    frame.current = Load<uint32_t>(memory_, frame.data + 0xc) + advance;
    frame.next = Load<uint32_t>(memory_, frame.data + 0x10) + advance;
    // The first three channels are decided by the entry statics; the later
    // cursors depend on the delegated decoder and are checked when consumed.
    uint32_t static_cursor = frame.static_values, current = frame.current, next = frame.next;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const bool is_static = ((frame.statics >> channel) & 1u) != 0;
        uint32_t* const static_cursors[] = {&static_cursor};
        uint32_t* const dynamic_cursors[] = {&current, &next};
        const size_t cursor_count = is_static ? 1 : 2;
        uint32_t* const* cursors = is_static ? static_cursors : dynamic_cursors;
        for (size_t i = 0; i < cursor_count; ++i) {
            uint32_t* cursor = cursors[i];
            if (!object(*cursor, 2)) return false;
            *cursor += 2;
        }
    }
    if (context.fpuRegs.fpr[12].f < 1.0f && (Load<uint64_t>(memory_, frame.animator) & 4) == 0 &&
        !object(kDefaultScale, 12)) return false;
    return true;
}

bool JointLibrary::ReadChannels(CpuContext& context, uint32_t reader, uint32_t now, uint32_t next,
                                unsigned now_register, unsigned next_register, unsigned counter) {
    // Source ReadChannels, inlined twice by AnimateJoint: a static channel
    // repeats one sample into both outputs, a live channel reads its current
    // and next samples; the halfword flags shift after every channel.
    auto& regs = context.cpuRegs.GPR.r;
    auto& fpu = context.fpuRegs.fpr;
    regs[counter].SD[0] = 2;
    for (;;) {
        const bool is_static = (Load<uint16_t>(memory_, reader) & 1u) != 0;
        regs[2].SD[0] = Extend(Load<uint32_t>(memory_, reader + 4));
        if (is_static) {
            if (!Mapped(regs[2].UL[0], 2)) return false;
            regs[3].SD[0] = static_cast<int16_t>(Load<uint16_t>(memory_, regs[2].UL[0]));
            fpu[0].f = Channel(static_cast<int16_t>(regs[3].UL[0]));
            Store(memory_, regs[now_register].UL[0], fpu[0].UL);
            Store(memory_, regs[next_register].UL[0], fpu[0].UL);
            regs[2].SD[0] = Extend(Load<uint32_t>(memory_, reader + 4) + 2);
            Store(memory_, reader + 4, regs[2].UL[0]);
        } else {
            regs[2].SD[0] = Extend(Load<uint32_t>(memory_, reader + 8));
            if (!Mapped(regs[2].UL[0], 2)) return false;
            regs[3].SD[0] = static_cast<int16_t>(Load<uint16_t>(memory_, regs[2].UL[0]));
            fpu[0].f = Channel(static_cast<int16_t>(regs[3].UL[0]));
            Store(memory_, regs[now_register].UL[0], fpu[0].UL);
            regs[2].SD[0] = Extend(Load<uint32_t>(memory_, reader + 0xc));
            if (!Mapped(regs[2].UL[0], 2)) return false;
            regs[3].SD[0] = static_cast<int16_t>(Load<uint16_t>(memory_, regs[2].UL[0]));
            fpu[0].f = Channel(static_cast<int16_t>(regs[3].UL[0]));
            Store(memory_, regs[next_register].UL[0], fpu[0].UL);
            regs[2].SD[0] = Extend(Load<uint32_t>(memory_, reader + 8) + 2);
            regs[3].SD[0] = Extend(Load<uint32_t>(memory_, reader + 0xc) + 2);
            Store(memory_, reader + 8, regs[2].UL[0]);
            Store(memory_, reader + 0xc, regs[3].UL[0]);
        }
        regs[3].UD[0] = Load<uint16_t>(memory_, reader);
        regs[next_register].SD[0] = Extend(regs[next_register].UL[0] + 4);
        regs[2].UD[0] = Load<uint16_t>(memory_, reader + 2);
        regs[now_register].SD[0] = Extend(regs[now_register].UL[0] + 4);
        regs[3].SD[0] = Extend(regs[3].UL[0] >> 1);
        regs[counter].SD[0] = Extend(regs[counter].UL[0] - 1);
        regs[2].SD[0] = Extend(regs[2].UL[0] >> 1);
        Store(memory_, reader, static_cast<uint16_t>(regs[3].UL[0]));
        Store(memory_, reader + 2, static_cast<uint16_t>(regs[2].UL[0]));
        if (regs[counter].SD[0] < 0) break;
    }
    (void)now; (void)next;
    return true;
}

void JointLibrary::BlendScale(CpuContext& context) {
    // Source ScaleJoint (297d88): blend the animator scale toward the frame
    // scale by the weight; without a scale flag start from the default scale
    // constants and set the flag. Register and write order follow the source.
    using Math = JointMath;
    auto& regs = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t animator = regs[4].UL[0], frame = regs[5].UL[0];
    const uint64_t flags = Load<uint64_t>(memory_, animator);
    regs[2].UD[0] = flags & 4;
    regs[2].SD[0] = 0x320000;
    const float weight = f[12].f;
    if ((flags & 4) != 0) {
        f[1].UL = Load<uint32_t>(memory_, animator + 0x30);
        regs[2].SD[0] = Extend(animator + 0x30);
        f[0].UL = Load<uint32_t>(memory_, frame);
        regs[1].SD[0] = 0x3f800000;
        f[3].UL = 0x3f800000u;
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
        f[1].f = Math::ScalarSum(f[1].f, f[0].f);
        Store(memory_, animator + 0x30, f[1].UL);
        f[0].UL = Load<uint32_t>(memory_, frame + 4);
        f[1].UL = Load<uint32_t>(memory_, regs[2].UL[0] + 4);
        f[2].UL = Load<uint32_t>(memory_, regs[2].UL[0] + 8);
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
        f[1].f = Math::ScalarSum(f[1].f, f[0].f);
        Store(memory_, regs[2].UL[0] + 4, f[1].UL);
        f[0].UL = Load<uint32_t>(memory_, frame + 8);
        Store(memory_, regs[2].UL[0] + 0xc, f[3].UL);
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[2].f), weight);
        f[2].f = Math::ScalarSum(f[2].f, f[0].f);
        Store(memory_, regs[2].UL[0] + 8, f[2].UL);
        return;
    }
    f[0].UL = Load<uint32_t>(memory_, frame);
    f[2].UL = Load<uint32_t>(memory_, kDefaultScale);
    regs[3].SD[0] = Extend(animator + 0x30);
    regs[2].SD[0] = static_cast<int32_t>(kDefaultScale);
    regs[1].SD[0] = 0x3f800000;
    f[3].UL = 0x3f800000u;
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[2].f), weight);
    f[2].f = Math::ScalarSum(f[2].f, f[0].f);
    Store(memory_, animator + 0x30, f[2].UL);
    f[1].UL = Load<uint32_t>(memory_, kDefaultScale + 4);
    f[0].UL = Load<uint32_t>(memory_, frame + 4);
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
    f[1].f = Math::ScalarSum(f[1].f, f[0].f);
    Store(memory_, regs[3].UL[0] + 4, f[1].UL);
    f[2].UL = Load<uint32_t>(memory_, kDefaultScale + 8);
    f[0].UL = Load<uint32_t>(memory_, frame + 8);
    Store(memory_, regs[3].UL[0] + 0xc, f[3].UL);
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[2].f), weight);
    f[2].f = Math::ScalarSum(f[2].f, f[0].f);
    Store(memory_, regs[3].UL[0] + 8, f[2].UL);
    regs[3].SD[0] = 4;
    regs[2].UD[0] = Load<uint64_t>(memory_, animator) | 4;
    Store(memory_, animator, regs[2].UD[0]);
}

void JointLibrary::BlendTranslation(CpuContext& context) {
    // Source TranslateJoint (297e68): like the scale blend, starting from the
    // joint's own translation when the animator has no translation yet.
    using Math = JointMath;
    auto& regs = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t animator = regs[4].UL[0], frame = regs[5].UL[0];
    const uint64_t flags = Load<uint64_t>(memory_, animator);
    regs[2].UD[0] = flags & 2;
    const float weight = f[12].f;
    if ((flags & 2) != 0) {
        f[1].UL = Load<uint32_t>(memory_, animator + 0x20);
        regs[2].SD[0] = Extend(animator + 0x20);
        f[0].UL = Load<uint32_t>(memory_, frame);
        regs[1].SD[0] = 0x3f800000;
        f[3].UL = 0x3f800000u;
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
        f[1].f = Math::ScalarSum(f[1].f, f[0].f);
        Store(memory_, animator + 0x20, f[1].UL);
        f[0].UL = Load<uint32_t>(memory_, frame + 4);
        f[1].UL = Load<uint32_t>(memory_, regs[2].UL[0] + 4);
        f[2].UL = Load<uint32_t>(memory_, regs[2].UL[0] + 8);
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
        f[1].f = Math::ScalarSum(f[1].f, f[0].f);
        Store(memory_, regs[2].UL[0] + 4, f[1].UL);
        f[0].UL = Load<uint32_t>(memory_, frame + 8);
        Store(memory_, regs[2].UL[0] + 0xc, f[3].UL);
        f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[2].f), weight);
        f[2].f = Math::ScalarSum(f[2].f, f[0].f);
        Store(memory_, regs[2].UL[0] + 8, f[2].UL);
        return;
    }
    regs[2].SD[0] = Extend(Load<uint32_t>(memory_, animator + 4));
    const uint32_t joint = regs[2].UL[0];
    Require(memory_, joint, 12);
    regs[3].SD[0] = Extend(animator + 0x20);
    f[0].UL = Load<uint32_t>(memory_, frame);
    f[1].UL = Load<uint32_t>(memory_, joint);
    regs[1].SD[0] = 0x3f800000;
    f[3].UL = 0x3f800000u;
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
    f[1].f = Math::ScalarSum(f[1].f, f[0].f);
    Store(memory_, animator + 0x20, f[1].UL);
    f[2].UL = Load<uint32_t>(memory_, joint + 4);
    f[0].UL = Load<uint32_t>(memory_, frame + 4);
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[2].f), weight);
    f[2].f = Math::ScalarSum(f[2].f, f[0].f);
    Store(memory_, regs[3].UL[0] + 4, f[2].UL);
    f[1].UL = Load<uint32_t>(memory_, joint + 8);
    f[0].UL = Load<uint32_t>(memory_, frame + 8);
    Store(memory_, regs[3].UL[0] + 0xc, f[3].UL);
    f[0].f = Math::ScalarProduct(Math::ScalarDifference(f[0].f, f[1].f), weight);
    f[1].f = Math::ScalarSum(f[1].f, f[0].f);
    Store(memory_, regs[3].UL[0] + 8, f[1].UL);
    regs[3].SD[0] = 2;
    regs[2].UD[0] = Load<uint64_t>(memory_, animator) | 2;
    Store(memory_, animator, regs[2].UD[0]);
}

void JointLibrary::PublishFrame(CpuContext& context, uint32_t base, const JointMath::Vector& rotation) {
    // Weight at or above one: the frame's scale, movement and rotation replace
    // the animator pose outright and the three pose flags are set.
    auto& regs = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t animator = regs[19].UL[0];
    regs[3].UD[0] = Load<uint64_t>(memory_, animator);
    regs[5].SD[0] = 4;
    LoadRegister(regs[4], memory_, base + 0x50);
    regs[6].SD[0] = 2;
    LoadRegister(regs[2], memory_, base + 0x10);
    regs[3].UD[0] |= 6;
    Store(memory_, (animator + 0x30) & ~15u, regs[4]);
    Store(memory_, (animator + 0x20) & ~15u, regs[2]);
    Store(memory_, animator, regs[3].UD[0]);
    StoreQuad(memory_, base + 0x70, rotation);
    f[0].UL = Load<uint32_t>(memory_, base + 0x70);
    regs[2].SD[0] = 1;
    f[1].UL = Load<uint32_t>(memory_, base + 0x74);
    regs[3].UD[0] |= 1;
    Store(memory_, animator + 0x10, f[0].UL);
    regs[2].SD[0] = Extend(animator + 0x10);
    Store(memory_, animator, regs[3].UD[0]);
    Store(memory_, regs[2].UL[0] + 4, f[1].UL);
    f[0].UL = Load<uint32_t>(memory_, base + 0x78);
    Store(memory_, regs[2].UL[0] + 8, f[0].UL);
    f[1].UL = Load<uint32_t>(memory_, base + 0x7c);
    Store(memory_, regs[2].UL[0] + 0xc, f[1].UL);
}

JointLibrary::AnimateResult JointLibrary::Animate(CpuContext& context) {
    using Vector = JointMath::Vector;
    auto& regs = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const auto& constant = context.vuRegs.VF[0];
    if (regs[0].UD[0] != 0 || context.vuRegs.VI[0] != 0 || constant.I[0] != 0 ||
        constant.I[1] != 0 || constant.I[2] != 0 || constant.I[3] != 0x3f800000u || !call_) {
        Decline(Refusal::State);
        return AnimateResult::Invalid;
    }
    AnimateFrame frame;
    if (!ValidateAnimate(context, frame)) return AnimateResult::Invalid;
    const uint32_t base = frame.base;
    // A track cursor the entry statics cannot decide may prove unreadable only
    // after the delegated decoder advanced it; every write so far lands in the
    // dead callee frame, so the entry context is restored and the call declines.
    const CpuContext entry = context;
    const auto decline = [&](Refusal refusal) {
        context = entry;
        Decline(refusal);
        return AnimateResult::Invalid;
    };
    // Prologue: frame, callee saves and the track reader, in source order.
    regs[29].SD[0] = Extend(base);
    regs[1].SD[0] = 0x39800000;
    f[1].UL = 0x39800000u;
    const std::array<uint64_t, 9> saved{regs[16].UD[0], regs[17].UD[0], regs[18].UD[0], regs[19].UD[0],
        regs[20].UD[0], regs[21].UD[0], regs[22].UD[0], regs[23].UD[0], regs[31].UD[0]};
    for (unsigned i = 0; i < 8; ++i) Store(memory_, base + 0x90 + i * 8, saved[i]);
    Store(memory_, base + 0xd0, saved[8]);
    Store(memory_, base + 0xe8, f[21].UL);
    Store(memory_, base + 0xe0, f[20].UL);
    regs[12].SD[0] = Extend(base + 0x40);
    regs[19].UD[0] = regs[4].UD[0];
    regs[21].SD[0] = Extend(base + 0x60);
    regs[22].SD[0] = Extend(base + 0x50);
    regs[23].SD[0] = Extend(base + 0x10);
    f[21].f = f[13].f;
    f[20].f = f[12].f;
    regs[9].UD[0] = regs[29].UD[0];
    regs[10].UD[0] = regs[23].UD[0];
    regs[20].SD[0] = Extend(frame.settings);
    Store(memory_, base, frame.statics);
    Store(memory_, base + 2, frame.mask);
    Store(memory_, base + 4, frame.static_values);
    Store(memory_, base + 8, frame.current);
    Store(memory_, base + 0xc, frame.next);
    regs[8].SD[0] = Extend(base + 0x20);
    if (!ReadChannels(context, base, base + 0x10, base + 0x20, 10, 8, 11)) return decline(Refusal::Channels);
    const Vector move_now = LoadQuad(memory_, base + 0x10), move_next = LoadQuad(memory_, base + 0x20);
    // Three angle channels through the installed decoder binding, which owns
    // its own counting and its 0x30-byte frame below this one.
    regs[17].UD[0] = regs[12].UD[0];
    regs[16].SD[0] = Extend(base + 0x30);
    for (unsigned channel = 0; channel < 3; ++channel) {
        regs[5].UD[0] = regs[16].UD[0];
        regs[6].UD[0] = regs[17].UD[0];
        regs[4].UD[0] = regs[29].UD[0];
        regs[17].SD[0] = Extend(regs[17].UL[0] + 4);
        regs[31].UL[0] = 0x29407cu;
        call_(context, kReadJointAngle);
        regs[18].SD[0] = 1 - static_cast<int32_t>(channel);
        regs[16].SD[0] = Extend(regs[16].UL[0] + 4);
    }
    Store(memory_, base + 0x4c, f[21].UL);
    const Vector angles_now = LoadQuad(memory_, base + 0x30), angles_next = LoadQuad(memory_, base + 0x40);
    const auto euler = JointMath::EulerRotation(angles_now, angles_next, move_now, move_next);
    regs[1].SD[0] = 0x39800000;
    f[1].UL = 0x39800000u;
    regs[6].UD[0] = regs[29].UD[0];
    regs[5].UD[0] = regs[21].UD[0];
    regs[4].UD[0] = regs[22].UD[0];
    if (!ReadChannels(context, base, base + 0x50, base + 0x60, 4, 5, 7)) return decline(Refusal::Channels);
    const uint16_t settings = Load<uint16_t>(memory_, frame.settings);
    regs[2].UD[0] = (settings >> 13) & 1u;
    regs[3].SD[0] = 8;
    if (regs[2].UD[0] != 0) {
        regs[2].UD[0] = Load<uint64_t>(memory_, frame.animator) | 8;
        Store(memory_, frame.animator, regs[2].UD[0]);
    }
    const Vector share = LoadQuad(memory_, base + 0x40);
    const Vector scale = JointMath::Lerp(LoadQuad(memory_, base + 0x50), LoadQuad(memory_, base + 0x60), share[3]);
    StoreQuad(memory_, base + 0x10, euler.move);
    StoreQuad(memory_, base + 0x50, scale);
    Vector rotation = euler.rotation;
    regs[2].UD[0] = (Load<uint16_t>(memory_, frame.settings) >> 12) & 1u;
    if (regs[2].UD[0] != 0) {
        regs[2].SD[0] = Extend(Load<uint32_t>(memory_, frame.animator + 4));
        Require(memory_, (regs[2].UL[0] + 0x40) & ~15u, 16);
        rotation = JointMath::Turn(LoadQuad(memory_, regs[2].UL[0] + 0x40), rotation);
    }
    regs[1].SD[0] = 0x3f800000;
    f[0].UL = 0x3f800000u;
    const bool blend = f[20].f < f[0].f;
    if (blend) context.fpuRegs.fprc[31] |= 0x800000u;
    else context.fpuRegs.fprc[31] &= ~0x800000u;
    SetVector(context.vuRegs.VF[2], euler.move);
    const uint32_t animator = regs[19].UL[0];
    AnimateResult result;
    if (blend) {
        regs[4].UD[0] = Load<uint64_t>(memory_, animator);
        Store(memory_, base + 0x7c, f[0].UL);
        Store(memory_, base + 0x70, f[20].UL);
        regs[2].UD[0] = regs[4].UD[0] & 1;
        Store(memory_, base + 0x74, f[20].UL);
        Store(memory_, base + 0x78, f[20].UL);
        if (regs[2].UD[0] != 0) {
            LoadRegister(regs[2], memory_, animator + 0x10);
            Store(memory_, base + 0x80, regs[2]);
        } else {
            regs[2].SD[0] = Extend(Load<uint32_t>(memory_, animator + 4));
            regs[3].UD[0] = regs[4].UD[0] | 1;
            LoadRegister(regs[4], memory_, regs[2].UL[0] + 0x10);
            Store(memory_, animator, regs[3].UD[0]);
            Store(memory_, base + 0x80, regs[4]);
        }
        rotation = JointMath::Interpolate(LoadQuad(memory_, base + 0x80), rotation, f[20].f).rotation;
        regs[5].UD[0] = regs[22].UD[0];
        regs[4].UD[0] = regs[19].UD[0];
        f[12].f = f[20].f;
        regs[31].UL[0] = 0x29424cu;
        BlendScale(context);
        regs[5].UD[0] = regs[23].UD[0];
        f[12].f = f[20].f;
        regs[4].UD[0] = regs[19].UD[0];
        regs[31].UL[0] = 0x29425cu;
        BlendTranslation(context);
        StoreQuad(memory_, animator + 0x10, rotation);
        result = AnimateResult::Blended;
    } else {
        PublishFrame(context, base, rotation);
        result = AnimateResult::Published;
    }
    SetVector(context.vuRegs.VF[1], rotation);
    for (unsigned i = 0; i < 8; ++i) regs[16 + i].UD[0] = saved[i];
    regs[31].UD[0] = saved[8];
    f[21].UL = Load<uint32_t>(memory_, base + 0xe8);
    f[20].UL = Load<uint32_t>(memory_, base + 0xe0);
    regs[29].SD[0] = Extend(regs[29].UL[0] + 0xf0);
    return result;
}

}  // namespace title
