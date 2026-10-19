#pragma once

#include <atomic>
#include <memory>

#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

// Source point-cloud, transformed point-cloud and triangle bounds. Temporary
// register publication belongs to Invoke's generated-caller ABI only.
class BoundsMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Body = MathLibrary::Body;
    using CaptureBody = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;
    struct Box { Vector minimum, maximum; };
    struct Result {
        Box box;
        Vector last_point;
        Vector accumulator;
    };
    struct Entry { uint32_t address; const char* name; };
    struct Counters {
        std::array<uint64_t, 3> owned_calls;
        std::array<uint64_t, 3> generated_body_calls;
        uint64_t generated_interiors;
        uint64_t unsupported_inputs;
    };

    static const std::array<Entry, 3>& Entries();
    // Nonempty owned point span. Raw minima/maxima preserve operand words,
    // including denormals, signed zero and the control's NaN operand choice.
    static Result Points(const Vector* points, size_t count, const Matrix* transform = nullptr);
    // xyz bound the three corners; the source leaves each output w unchanged.
    static Box Triangle(const Vector& a, const Vector& b, const Vector& c,
                        float minimum_w, float maximum_w);

    explicit BoundsMath(ps2r::ipu::GuestMemory& memory) : memory_(memory) {}
    ~BoundsMath();
    BoundsMath(const BoundsMath&) = delete;
    BoundsMath& operator=(const BoundsMath&) = delete;
    bool Install(const CaptureBody& capture, const Installer& installer, bool selected);
    Counters Counts() const;
    std::string Census() const;
    // Complete entries only. Invalid/wrapped spans and INT_MIN count fail
    // before mutation; generated interior frames retain their original ABI.
    bool Invoke(uint32_t entry, CpuContext& context);

private:
    class CountsState;
    class Installation;
    static void Include(Box& box, const Vector& point);
    bool Range(uint32_t address, uint64_t size);
    Vector Read(uint32_t address);
    void Write(uint32_t address, const Box& box);
    bool Triangle(CpuContext& context);
    bool Points(CpuContext& context, bool transformed);

    ps2r::ipu::GuestMemory& memory_;
    std::shared_ptr<CountsState> counts_;
    std::shared_ptr<Installation> installation_;
};

}  // namespace title
