// render/backends/gl/gl_uniform_cache.h -- GlUniformCache: per-location value shadow
// for the PS2 program's scalar uniforms.
//
// The draw path sets ~30 scalar uniforms per draw and consecutive draws share
// almost all of them; each setter records the last value sent per location and
// skips the glUniform* call when unchanged. Uniform values are per-program state,
// so the shadow is cleared on every relink. A negative location is a silent no-op,
// as glUniform* defines it. The shadow is a small open-addressed table hashed by a
// multiply (a location can be a large number -- vitaGL's are addresses -- and a
// hashed container's modulo is a software divide on the Vita's CPU).
#pragma once

#include <cstdint>

namespace ps2r {
namespace render {

class GlUniformCache {
public:
    void Clear();
    void Set1i(int loc, int v);
    void Set1f(int loc, float v);
    void Set2f(int loc, float a, float b);
    void Set2i(int loc, int a, int b);
    void Set3f(int loc, float a, float b, float c);
    void Set4f(int loc, float a, float b, float c, float d);
    // a vec4 array of `count` (at most kArrayFloats / 4) from `v`
    void Set4fv(int loc, int count, const float* v);
    uint64_t Skips() const { return skips_; }
    uint64_t Mutations() const { return mutations_; }   // the glUniform* calls made

private:
    struct Shadow {
        uint8_t kind = 0;   // 1=1i 2=1f 3=2f 4=3f 5=2i 6=4f
        int i[2] = {0, 0};
        float f[4] = {0.f, 0.f, 0.f, 0.f};
    };
    struct Entry {
        int loc = -1;   // -1: free
        Shadow shadow;
    };
    static constexpr int kBits = 9;
    static constexpr int kSize = 1 << kBits;
    // the location's entry (claimed when new); a full table answers a scratch entry that never matches
    Shadow& Slot(int loc);
    Entry table_[kSize];
    int used_ = 0;
    Shadow overflow_;
    static constexpr int kArrayFloats = 32;
    struct ArrayShadow {
        int loc = -1;
        int count = 0;
        float v[kArrayFloats] = {};
    };
    ArrayShadow arrays_[4];
    uint64_t skips_ = 0;
    uint64_t mutations_ = 0;
};

}  // namespace render
}  // namespace ps2r
