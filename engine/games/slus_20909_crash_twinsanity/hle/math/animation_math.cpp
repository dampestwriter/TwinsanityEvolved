#include "games/slus_20909_crash_twinsanity/hle/math/animation_math.h"

#include <cstring>
#include <memory>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.h"
#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

namespace title {
namespace {

int32_t WrappedCurrent(int32_t current, int32_t next) {
    const int32_t distance = current - next;
    if (distance > 32768) return current - 65536;
    if (distance < -32768) return current + 65536;
    return current;
}

void Advance(GPR_reg& reg, uint32_t bytes) {
    reg.SD[0] = static_cast<int32_t>(reg.UL[0] + bytes);
}

}  // namespace

AnimationMath::AnimationMath(GuestMemory& memory)
    : memory_(memory), generated_body_calls_(std::make_shared<std::atomic<uint64_t>>(0)) {}

uint16_t AnimationMath::Read16(uint32_t address) {
    uint16_t value;
    std::memcpy(&value, memory_.Translate(address, sizeof(value)), sizeof(value));
    return value;
}

void AnimationMath::Write16(uint32_t address, uint16_t value) {
    std::memcpy(memory_.Translate(address, sizeof(value)), &value, sizeof(value));
}

bool AnimationMath::CanReplace(const CpuContext& context) {
    // The 48-byte callee frame is validated per quad: the caller's stack is the
    // mirrored scratchpad and may straddle its 0x70004000 seam.
    using Span = GuestSpan<GuestMemory>;
    const auto& r = context.cpuRegs.GPR.r;
    if (r[0].UD[0] != 0 || !Span::Mapped(memory_, r[4].UL[0], 16) ||
        !memory_.Translate(r[5].UL[0], 4) || !memory_.Translate(r[6].UL[0], 4) ||
        !Span::Mapped(memory_, r[29].UL[0] - 48, 48)) return false;
    const uint32_t reader = r[4].UL[0];
    if ((Read16(reader) & 1) != 0) return memory_.Translate(memory_.Read32(reader + 4), 2) != nullptr;
    return memory_.Translate(memory_.Read32(reader + 8), 2) != nullptr &&
        memory_.Translate(memory_.Read32(reader + 12), 2) != nullptr;
}


AnimationMath::Angles AnimationMath::DecodeAngles(int16_t current, int16_t next, bool is_static) {
    // Multiplication avoids C++'s undefined left shift of a negative sample.
    const int32_t first = static_cast<int32_t>(current) * 16;
    if (is_static) {
        const float value = TrigMath::AngleRadians(first);
        return {value, value};
    }
    const int32_t second = static_cast<int32_t>(next) * 16;
    return {TrigMath::AngleRadians(WrappedCurrent(first, second)), TrigMath::AngleRadians(second)};
}

void AnimationMath::ReadJointAngle(CpuContext& context) {
    calls_.fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[7].UD[0] = r[4].UD[0];
    r[8].UD[0] = r[5].UD[0];
    Advance(r[29], uint32_t(-48));
    const uint32_t reader = r[7].UL[0], stack = r[29].UL[0];
    if ((Read16(reader) & 1) == 0) {
        r[4].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 8));
        r[5].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 12));
        r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(Read16(r[4].UL[0]))) * 16;
        // The source stores this first sample before reading the next sample.
        // If those addresses overlap, the second read observes that store.
        memory_.Write32(stack, r[4].UL[0]);
        const int32_t next = static_cast<int32_t>(static_cast<int16_t>(Read16(r[5].UL[0]))) * 16;
        r[5].SD[0] = r[4].SL[0] - next;
        memory_.Write32(stack + 16, static_cast<uint32_t>(next));
        memory_.Write32(stack + 32, static_cast<uint32_t>(next));
        const int32_t wrapped = WrappedCurrent(r[4].SL[0], next);
        if (wrapped != r[4].SL[0]) memory_.Write32(stack, static_cast<uint32_t>(wrapped));
        r[1].SD[0] = 0x38c90fdb;
        f[2].UL = 0x38c90fdb;
        f[1].f = TrigMath::AngleRadians(static_cast<int32_t>(memory_.Read32(stack)));
        f[0].f = TrigMath::AngleRadians(static_cast<int32_t>(memory_.Read32(stack + 16)));
        memory_.Write32(r[8].UL[0], f[1].UL);
        memory_.Write32(r[6].UL[0], f[0].UL);
        // Re-read cursor fields after output publication; source callers may
        // alias outputs with the reader. Do not hoist these reads.
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 8));
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 12));
        Advance(r[2], 2);
        Advance(r[3], 2);
        memory_.Write32(reader + 8, r[2].UL[0]);
        memory_.Write32(reader + 12, r[3].UL[0]);
    } else {
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 4));
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        const int32_t sample = static_cast<int32_t>(static_cast<int16_t>(Read16(r[3].UL[0]))) * 16;
        memory_.Write32(stack, static_cast<uint32_t>(sample));
        f[0].f = TrigMath::AngleRadians(sample);
        memory_.Write32(r[8].UL[0], f[0].UL);
        memory_.Write32(r[6].UL[0], f[0].UL);
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(reader + 4));
        Advance(r[2], 2);
        memory_.Write32(reader + 4, r[2].UL[0]);
    }
    r[2].UD[0] = Read16(reader) >> 1;
    r[3].UD[0] = Read16(reader + 2) >> 1;
    Write16(reader, static_cast<uint16_t>(r[2].UL[0]));
    Write16(reader + 2, static_cast<uint16_t>(r[3].UL[0]));
    Advance(r[29], 48);
}

AnimationMath::Counters AnimationMath::Counts() const {
    return {calls_.load(std::memory_order_relaxed), continuations_->load(std::memory_order_relaxed),
        unsupported_.load(std::memory_order_relaxed), generated_body_calls_->load(std::memory_order_relaxed)};
}

std::string AnimationMath::Census() const {
    const auto counts = Counts();
    return "angle_pairs=" + std::to_string(counts.angle_pairs) +
        ",generated_continuations=" + std::to_string(counts.generated_continuations) +
        ",unsupported_inputs=" + std::to_string(counts.unsupported_inputs) +
        ",generated_body_angle_pairs=" + std::to_string(counts.generated_body_calls);
}

AnimationMath::~AnimationMath() {
    if (installation_) installation_->store(false, std::memory_order_release);
}

}  // namespace title
