// render/backends/gl/gl_vertex_stream.cpp -- GlVertexStream implementation.
#include "engine/render/backends/gl/gl_vertex_stream.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER

#include <cstddef>
#include <cstring>

#include "engine/render/backends/gl/gl_state_cache.h"
#include "engine/render/backend.h"

namespace ps2r {
namespace render {

bool GlVertexStream::Init(GlStateCache& st) {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    st.BindVertexArray(vao_);
    st.BindArrayBuffer(vbo_);
    // attribute 0 position xyz @0, 1 texcoord uvq @12, 2 colour rgba8n @24, 3 fog u8n @28
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, fog)));
    st.BindVertexArray(0);
#if PS2R_HOST_VITA
    // the raw layout over the same buffer: S / T on attribute 1 (its Q read from attribute 6), no fog
    glGenVertexArrays(1, &raw_vao_);
    st.BindVertexArray(raw_vao_);
    st.BindArrayBuffer(vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, r)));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(RawVertex), reinterpret_cast<void*>(offsetof(RawVertex, q)));
    st.BindVertexArray(0);
#endif
    ready_ = false;
    offset_ = 0;
    return vao_ != 0 && vbo_ != 0;
}

void GlVertexStream::Shutdown(GlStateCache& st) {
#if !PS2R_HOST_VITA
    if (fence_) { glDeleteSync(static_cast<GLsync>(fence_)); fence_ = nullptr; }
#else
    fence_ = nullptr;
#endif
    ready_ = false;
    offset_ = 0;
    if (vbo_) { st.DeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { st.DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (raw_vao_) { st.DeleteVertexArrays(1, &raw_vao_); raw_vao_ = 0; }
}

void GlVertexStream::BindForDraw(GlStateCache& st) {
    st.BindVertexArray(vao_);
    st.BindArrayBuffer(vbo_);
}

void GlVertexStream::BindRawForDraw(GlStateCache& st) {
    st.BindVertexArray(raw_vao_);
    st.BindArrayBuffer(vbo_);
}

int GlVertexStream::Upload(const GpuVertex* verts, size_t n) {
    return UploadRaw(verts, n * sizeof(GpuVertex), sizeof(GpuVertex));
}

int GlVertexStream::UploadRaw(const void* verts, size_t bytes, size_t stride) {
    if (bytes == 0) return 0;
    if (bytes > kRingSize) {
        // A single draw larger than the ring: plain re-specification; the ring
        // re-initialises on the next upload.
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), verts, GL_STREAM_DRAW);
        ready_ = false;
        offset_ = 0;
        generation_++;
        return 0;
    }
    if (!ready_) {
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
        ready_ = true;
        offset_ = 0;
        generation_++;
    }
    // the next multiple of the stride (32-byte records: the next 32-byte boundary, as before)
    size_t off = (offset_ + stride - 1) / stride * stride;
    if (off + bytes > kRingSize) {
#if !PS2R_HOST_VITA
        if (fence_) {
            glClientWaitSync(static_cast<GLsync>(fence_), GL_SYNC_FLUSH_COMMANDS_BIT,
                             1000000000ull);
            glDeleteSync(static_cast<GLsync>(fence_));
            fence_ = nullptr;
        }
#endif
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kRingSize), nullptr, GL_STREAM_DRAW);
        generation_++;
        fence_ = nullptr;
#if !PS2R_HOST_VITA
        fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
        off = 0;
    }
    void* p = glMapBufferRange(GL_ARRAY_BUFFER, static_cast<GLintptr>(off),
                               static_cast<GLsizeiptr>(bytes),
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                   GL_MAP_FLUSH_EXPLICIT_BIT);
    if (p) {
        std::memcpy(p, verts, bytes);
        glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes));
        glUnmapBuffer(GL_ARRAY_BUFFER);
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(bytes),
                        verts);
    }
    offset_ = off + bytes;
    return static_cast<int>(off / stride);
}

void GlVertexStream::BeginDrawRun() { in_run_ = true; }

void GlVertexStream::EndDrawRun(GlStateCache& st) {
    if (!in_run_) return;
    st.BindVertexArray(0);
    st.UseProgram(0);
    in_run_ = false;
}

}  // namespace render
}  // namespace ps2r
