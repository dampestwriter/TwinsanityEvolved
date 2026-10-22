// platform/vita/gl_vita.h -- PS2R_GL_HEADER on the PlayStation Vita: vitaGL, plus the one thing the GL backend needs
// that vitaGL cannot answer. The Vita's fragment profile has no texture size query (textureSize / tex2Dsize), so the
// backend passes each sampler's size as a uniform; glTexImage2D is routed through ps2r_vita_TexImage2D, which records
// the size of every level-0 image it defines, and ps2r_vita_TexSize answers it for a texture name.
#pragma once

#include <vitaGL.h>

#include <cstdint>

void ps2r_vita_TexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void* data);
// The size of texture `tex`'s level 0 as last defined; false when it was never defined through glTexImage2D.
bool ps2r_vita_TexSize(uint32_t tex, int* width, int* height);
// Changes whenever a level-0 image is defined: a size answered under the same generation still holds.
uint32_t ps2r_vita_TexGeneration();

#ifndef PS2R_GL_VITA_NO_WRAP
#define glTexImage2D ps2r_vita_TexImage2D
#endif
