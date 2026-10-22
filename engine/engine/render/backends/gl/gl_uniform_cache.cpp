// render/backends/gl/gl_uniform_cache.cpp -- GlUniformCache implementation.
#include "engine/render/backends/gl/gl_uniform_cache.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstring>
#include <cstdlib>   // XBISECT

namespace ps2r {
namespace render {

void GlUniformCache::Clear() {
    for (Entry& e : table_) e = Entry{};
    used_ = 0;
    for (ArrayShadow& a : arrays_) a = ArrayShadow{};
}

void GlUniformCache::Set4fv(int loc, int count, const float* v) {
    if (loc < 0 || count <= 0) return;
    const size_t bytes = size_t(count) * 4 * sizeof(float);
    ArrayShadow* slot = nullptr;
    for (ArrayShadow& a : arrays_) {
        if (a.loc == loc) { slot = &a; break; }
        if (a.loc < 0 && !slot) slot = &a;
    }
    if (slot && slot->loc == loc && slot->count == count && std::memcmp(slot->v, v, bytes) == 0) { skips_++; return; }
    if (slot && count * 4 <= kArrayFloats) {
        slot->loc = loc;
        slot->count = count;
        std::memcpy(slot->v, v, bytes);
    }
    ++mutations_;
    glUniform4fv(loc, count, v);
}

GlUniformCache::Shadow& GlUniformCache::Slot(int loc) {
    static const bool xNoCache = std::getenv("TE_X_NO_UCACHE") != nullptr;   // XBISECT
    if (xNoCache) { overflow_ = Shadow{}; return overflow_; }
    uint32_t at = (static_cast<uint32_t>(loc) * 0x9E3779B1u) >> (32 - kBits);
    for (;;) {
        Entry& e = table_[at];
        if (e.loc == loc) return e.shadow;
        if (e.loc < 0) {
            if (used_ >= kSize * 3 / 4) break;
            e.loc = loc;
            e.shadow = Shadow{};
            used_++;
            return e.shadow;
        }
        at = (at + 1) & (kSize - 1);
    }
    overflow_ = Shadow{};
    return overflow_;
}

void GlUniformCache::Set1i(int loc, int v) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 1 && s.i[0] == v) { skips_++; return; }
    s.kind = 1; s.i[0] = v;
    ++mutations_;
    glUniform1i(loc, v);
}

void GlUniformCache::Set1f(int loc, float v) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 2 && s.f[0] == v) { skips_++; return; }
    s.kind = 2; s.f[0] = v;
    ++mutations_;
    glUniform1f(loc, v);
}

void GlUniformCache::Set2f(int loc, float a, float b) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 3 && s.f[0] == a && s.f[1] == b) { skips_++; return; }
    s.kind = 3; s.f[0] = a; s.f[1] = b;
    ++mutations_;
    glUniform2f(loc, a, b);
}

void GlUniformCache::Set2i(int loc, int a, int b) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 5 && s.i[0] == a && s.i[1] == b) { skips_++; return; }
    s.kind = 5; s.i[0] = a; s.i[1] = b;
    ++mutations_;
    glUniform2i(loc, a, b);
}

void GlUniformCache::Set3f(int loc, float a, float b, float c) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 4 && s.f[0] == a && s.f[1] == b && s.f[2] == c) { skips_++; return; }
    s.kind = 4; s.f[0] = a; s.f[1] = b; s.f[2] = c;
    ++mutations_;
    glUniform3f(loc, a, b, c);
}

void GlUniformCache::Set4f(int loc, float a, float b, float c, float d) {
    if (loc < 0) return;
    Shadow& s = Slot(loc);
    if (s.kind == 6 && s.f[0] == a && s.f[1] == b && s.f[2] == c && s.f[3] == d) {
        skips_++;
        return;
    }
    s.kind = 6; s.f[0] = a; s.f[1] = b; s.f[2] = c; s.f[3] = d;
    ++mutations_;
    glUniform4f(loc, a, b, c, d);
}

}  // namespace render
}  // namespace ps2r
