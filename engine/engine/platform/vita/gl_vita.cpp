// platform/vita/gl_vita.cpp -- see gl_vita.h.
#define PS2R_GL_VITA_NO_WRAP 1
#include "engine/platform/vita/gl_vita.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {
std::mutex g_mutex;
std::unordered_map<uint32_t, std::pair<int, int>> g_sizes;   // texture name -> level-0 width, height
std::atomic<uint32_t> g_generation{1};
}  // namespace

void ps2r_vita_TexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void* data) {
    glTexImage2D(target, level, internal_format, width, height, border, format, type, data);
    if (target != GL_TEXTURE_2D || level != 0) return;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sizes[uint32_t(bound)] = {int(width), int(height)};
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

uint32_t ps2r_vita_TexGeneration() { return g_generation.load(std::memory_order_relaxed); }

bool ps2r_vita_TexSize(uint32_t tex, int* width, int* height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_sizes.find(tex);
    if (it == g_sizes.end()) return false;
    *width = it->second.first;
    *height = it->second.second;
    return true;
}
