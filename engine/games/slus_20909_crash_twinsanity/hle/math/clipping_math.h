#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"

namespace title {

// Eight-corner box clipping, including the source guard scale. Pure results
// have twelve-bit plane masks; generated register packing stays in the adapter.
class ClippingMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Box = BoundsMath::Box;
    using Corners = std::array<Vector, 8>;
    using Body = MathLibrary::Body;
    using CaptureBody = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;
    struct Result {
        Matrix matrix;
        Corners clipped, guarded;
        Vector accumulator;
        std::array<uint32_t, 8> masks;
        uint32_t any, common;
    };
    struct Entry { uint32_t address; const char* name; };
    struct Counters {
        uint64_t box_at, box, corners;
        std::array<uint64_t, 2> generated_body_calls;
        uint64_t unsupported_entries, unsupported_inputs;
    };
    static constexpr uint32_t kCorners = 0x2015d0;
    static const std::array<Entry, 2>& Entries();
    static Corners BoxCorners(const Box& box);
    static Result ClipCorners(const Corners& corners, const Matrix& view, const Vector& guard,
                              float translation_scale = 1);
    static Result ClipBox(const Box& box, const Matrix& view, const Vector& guard,
                          const Matrix* model = nullptr, float translation_scale = 1);

    explicit ClippingMath(ps2r::ipu::GuestMemory& memory) : memory_(memory) {}
    ~ClippingMath();
    ClippingMath(const ClippingMath&) = delete;
    ClippingMath& operator=(const ClippingMath&) = delete;
    bool Install(const CaptureBody& capture, const Installer& installer, bool selected);
    // 2015d0 is the source ClipCorners entry contained by generated 20157c.
    // Other interior requests are not supported selected entries.
    bool Invoke(uint32_t source_entry, CpuContext& context);
    Counters Counts() const;
    std::string Census() const;

private:
    class CountsState;
    class Installation;
    static uint32_t Outcode(const Vector& point);
    bool Range(uint32_t address, uint64_t size);
    void Publish(CpuContext& context, const Result& result);

    ps2r::ipu::GuestMemory& memory_;
    std::shared_ptr<CountsState> counts_;
    std::shared_ptr<Installation> installation_;
};

}  // namespace title
