#include "games/slus_20909_crash_twinsanity/hle/graphics/screen_aspect.h"

#include <cstring>

#include "engine/ee/cpu/ps2_float.h"
#include "engine/gs/hle/present/aspect_selection.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {
constexpr std::array<uint32_t, 4> kEntries{
    ScreenAspect::kProjection, ScreenAspect::kFrustum, ScreenAspect::kFitPlace, ScreenAspect::kFitSize};
constexpr std::array<const char*, 4> kNames{"projection", "frustum", "fit_place", "fit_size"};

uint32_t Gpr(const CpuContext& context, int index) { return context.cpuRegs.GPR.r[index].UL[0]; }
float ReadF32(ps2r::ipu::GuestMemory& memory, uint32_t address) {
    const uint32_t word = memory.Read32(address);
    float value;
    std::memcpy(&value, &word, sizeof value);
    return value;
}
void WriteF32(ps2r::ipu::GuestMemory& memory, uint32_t address, float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof word);
    memory.Write32(address, word);
}
// The s16 the generated bodies read with lh (a 32-bit word read, the half selected).
int16_t ReadS16(ps2r::ipu::GuestMemory& memory, uint32_t address) {
    const uint32_t word = memory.Read32(address & ~3u);
    return static_cast<int16_t>((address & 2) ? (word >> 16) : (word & 0xFFFF));
}
float Mul(float a, float b) { return ps2_float_clamp(ps2_float_clamp(a) * ps2_float_clamp(b)); }
float Sub(float a, float b) { return ps2_float_clamp(ps2_float_clamp(a) - ps2_float_clamp(b)); }
float Add(float a, float b) { return ps2_float_clamp(ps2_float_clamp(a) + ps2_float_clamp(b)); }
}  // namespace

ScreenAspect::ScreenAspect(ps2r::ipu::GuestMemory& memory, RatioSource ratio)
    : memory_(memory), ratio_(std::move(ratio)) {}
ScreenAspect::~ScreenAspect() = default;

bool ScreenAspect::Install(const Capture& capture, const Installer& install) {
    if (installed_ || !capture || !install || !ratio_) return false;
    for (size_t i = 0; i < kEntries.size(); ++i) {
        originals_[i] = capture(kEntries[i]);
        if (!originals_[i]) return false;
    }
    for (size_t i = 0; i < kEntries.size(); ++i) {
        if (!install(kEntries[i], [this, i](CpuContext& context, uint32_t entry) { Dispatch(i, context, entry); })) {
            for (size_t j = 0; j < i; ++j) install(kEntries[j], originals_[j]);
            return false;
        }
    }
    installed_ = true;
    return true;
}

void ScreenAspect::Dispatch(size_t index, CpuContext& context, uint32_t entry) {
    if (entry != 0 && entry != kEntries[index]) {
        // A restored continuation inside the body: the original finishes it;
        // the aspect argument was consumed before the recorded boundary.
        interior_.fetch_add(1, std::memory_order_relaxed);
        originals_[index](context, entry);
        return;
    }
    const double ratio = ratio_();
    const uint32_t view = index == 0 ? Gpr(context, 4) : 0;   // RenderView* (a0) before the body clobbers it
    if (ps2r::gs::AspectSelection::IsNarrow(ratio)) {
        // The title's narrow term: the generated body, untouched.
        passed_[index].fetch_add(1, std::memory_order_relaxed);
        originals_[index](context, entry);
        if (index == 0) PublishCamera(view);
        return;
    }
    const float aspect = Term(ratio);
    owned_[index].fetch_add(1, std::memory_order_relaxed);
    switch (index) {
        case 0:  // RenderView::Update(f32 aspect): $f12
            context.fpuRegs.fpr[12].f = aspect;
            originals_[index](context, entry);
            PublishCamera(view);
            return;
        case 1:  // SetViewFrustum(near $f12, far $f13, aspect $f14, &fov $a0)
            context.fpuRegs.fpr[14].f = aspect;
            originals_[index](context, entry);
            return;
        case 2: OwnFitPlace(context, aspect); return;
        default: OwnFitSize(context, aspect); return;
    }
}

void ScreenAspect::PublishCamera(uint32_t view) {
    if (!camera_ || view == 0) return;
    // The world->camera matrix the body just took from the camera (view.cpp:
    // Update copies the camera's inverted place): 16 f32, row-major.
    float m[16], p[16];
    for (uint32_t i = 0; i < 16; ++i) m[i] = ReadF32(memory_, view + kViewToCamera + i * 4);
    for (uint32_t i = 0; i < 16; ++i) p[i] = ReadF32(memory_, view + kViewCameraToScreen + i * 4);
    camera_(m, p);
}

void ScreenAspect::FitPlace(float aspect, bool in_pixels, float anchor_x, int16_t width, int16_t height,
                            float& place_x, float& place_y) {
    // TR renderer.cpp FitPlaceToScreen, in the generated body's operation order.
    const float offset = Sub(place_x, anchor_x);
    const float squeeze = Mul(aspect, kInverseNarrow);
    place_x = Add(ps2_div_s(offset, squeeze), anchor_x);
    if (!in_pixels) return;
    place_x = Mul(place_x, static_cast<float>(width));
    place_y = Mul(place_y, static_cast<float>(height));
}

void ScreenAspect::FitSize(float aspect, bool in_pixels, int16_t width, int16_t height,
                           float& size_x, float& size_y) {
    // TR renderer.cpp FitSizeToScreen, in the generated body's operation order.
    const float squeeze = Mul(aspect, kInverseNarrow);
    size_x = ps2_div_s(size_x, squeeze);
    if (!in_pixels) return;
    size_x = Mul(size_x, static_cast<float>(width));
    size_y = Mul(size_y, static_cast<float>(height));
}

void ScreenAspect::OwnFitPlace(CpuContext& context, float aspect) {
    const bool in_pixels = Gpr(context, 4) != 0;
    const uint32_t anchor = Gpr(context, 5), place = Gpr(context, 6);
    float place_x = ReadF32(memory_, place), place_y = ReadF32(memory_, place + 4);
    FitPlace(aspect, in_pixels, ReadF32(memory_, anchor), ReadS16(memory_, kRendererWidth),
             ReadS16(memory_, kRendererHeight), place_x, place_y);
    WriteF32(memory_, place, place_x);
    if (in_pixels) WriteF32(memory_, place + 4, place_y);
}

void ScreenAspect::OwnFitSize(CpuContext& context, float aspect) {
    const bool in_pixels = Gpr(context, 4) != 0;
    const uint32_t size = Gpr(context, 5);
    float size_x = ReadF32(memory_, size), size_y = ReadF32(memory_, size + 4);
    FitSize(aspect, in_pixels, ReadS16(memory_, kRendererWidth), ReadS16(memory_, kRendererHeight), size_x, size_y);
    WriteF32(memory_, size, size_x);
    if (in_pixels) WriteF32(memory_, size + 4, size_y);
}

ScreenAspect::Counters ScreenAspect::Counts() const {
    Counters result;
    for (size_t i = 0; i < kEntries.size(); ++i) {
        result.owned[i] = owned_[i].load(std::memory_order_relaxed);
        result.passed[i] = passed_[i].load(std::memory_order_relaxed);
    }
    result.interior = interior_.load(std::memory_order_relaxed);
    return result;
}

std::string ScreenAspect::Census() const {
    const auto counts = Counts();
    std::string result = std::string("ratio=") + ps2r::gs::AspectSelection::Name(ratio_ ? ratio_() : 0.0);
    for (size_t i = 0; i < kEntries.size(); ++i)
        result += "," + std::string(kNames[i]) + "=" + std::to_string(counts.owned[i]) +
                  ",passed_" + kNames[i] + "=" + std::to_string(counts.passed[i]);
    result += ",interior=" + std::to_string(counts.interior);
    return result;
}

}  // namespace title
