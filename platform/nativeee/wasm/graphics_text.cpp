// platform/nativeee (wasm side) -- Font::Begin / Draw / End (include/game/font.h: methods of a game class whose bodies were
// the PS2 renderer's text.cpp: glyph bytes for VU1 to expand into textured quads). The game lays the text out itself
// (RenderText, FontScale: src/game/font.cpp); here the VU1 program's expansion is done in C++ over that layout and the
// quads cross to the host typed (ps2e_gfx2_text_quads with te_gfx_text_quad), one call per font page used.
//
// The expansion (the text VU1 program, as the engine's text HLE pins it -- games/<title>/hle/graphics/text/text_draw.cpp):
// each line starts at (line.x + corner, line.y + corner) where corner = 2048*16 - frame*16/2 (the GS coordinates with the
// screen's middle at 2048, which StartFrame's XYOFFSET takes away again); each glyph is a quad from the cursor to
// (cursor + width*scale, y + height*scale) showing texels (u, v)..(u + width, v - height) of its page, and the cursor
// advances by width*scale whatever the glyph's page. Glyph quadwords: x = u (its page in the low two bits of the float's
// bits), y = v, z = width, w = height, all in 16ths of a pixel.
#include "common.h"
#include "gcc2.h"
#include "game/font.h"
#include "game/graphicstables.h"
#include "game/overlay.h"
#include "game/renderer.h"
#include "platform/graphics.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"

#include <bit>

namespace
{
constexpr s32 MaxQuadsPerCall = 256;
te_gfx_text_quad g_Quads[MaxQuadsPerCall];
// Which pages the frame's texts used (End sends nothing more: the quads went out in Draw, per page)
Material* g_TextMaterial;

u32 GlyphPage(const Vector4& glyph)
{
    return std::bit_cast<u32>(glyph.x) & 3;
}

f32 Corner(s32 frameSize)
{
    return 32768.0f - static_cast<f32>(frameSize * 16) * 0.5f;
}
}

void Font::Begin()
{
    g_TextMaterial = materials[0] != nullptr ? materials[0]->material : nullptr;
}

void Font::Draw(const TextItem* item, TextPackets* packets)
{
    // The host's text-draw census: the string this item draws (its quads follow, a page at a time)
    ps2e_gfx2_text_item(reinterpret_cast<ps2e_u32>(item->text), item->position.x, item->position.y,
                        static_cast<ps2e_s32>(item->alignment.value));
    // WriteTextPackets' layout: the glyph scale, the text laid out, the cursor by the alignment
    Vector2 position;
    position.x = item->position.x * static_cast<f32>(g_RendererWidth) * 16.0f;
    position.y = item->position.y * static_cast<f32>(g_RendererHeight) * 16.0f;
    Vector2 scale;
    FontScale(this, &scale);
    Vector2 size;
    size.x = item->scale.x * scale.x;
    size.y = item->scale.y * scale.y;
    static TextLayout layout;
    layout.pages[0] = layout.pages[1] = layout.pages[2] = layout.pages[3] = 0;
    RenderText(this, item->text, &size, &layout);
    Vector2 cursor = position;
    TextAlignment alignment = item->alignment;
    if (alignment.top == 0)
    {
        cursor.y = alignment.bottom != 0 ? cursor.y - layout.height : cursor.y - layout.height * 0.5f;
    }

    for (u32 page = 0; page < 4; page++)
    {
        if (layout.pages[page] == 1)
        {
            packets->pages[page] = 1;
        }
    }

    // The VU1 expansion, a page at a time (each page's material draws its own glyphs; the cursor advances for every glyph)
    f32 cornerX = Corner(g_RendererWidth);
    f32 cornerY = Corner(g_RendererHeight);
    for (s32 page = 0; page < pageCount; page++)
    {
        if (layout.pages[page + 1] == 0 || materials[page] == nullptr || materials[page]->material == nullptr)
        {
            continue;
        }

        Material* material = materials[page]->material;
        s32 quads = 0;
        s32 taken = 0;
        f32 y = cursor.y;
        for (s32 line = 0; line < layout.lineCount; line++)
        {
            f32 x = cursor.x;
            f32 extent = layout.lines[line].x;
            if (alignment.left == 0)
            {
                x = alignment.right != 0 ? x - extent : x - extent * 0.5f;
            }

            f32 lineY = y + cornerY;
            f32 glyphX = x + cornerX;
            s32 count = layout.counts[line];
            for (s32 glyphIndex = 0; glyphIndex < count; glyphIndex++)
            {
                const Vector4& glyph = glyphs[layout.glyphs[taken + glyphIndex]];
                f32 right = glyphX + glyph.z * size.x;
                f32 bottom = lineY + glyph.w * size.y;
                if (static_cast<s32>(GlyphPage(glyph)) == page + 1)
                {
                    te_gfx_text_quad& quad = g_Quads[quads++];
                    quad.x0 = glyphX;
                    quad.y0 = lineY;
                    quad.x1 = right;
                    quad.y1 = bottom;
                    quad.u0 = glyph.x;
                    quad.v0 = glyph.y;
                    quad.u1 = glyph.x + glyph.z;
                    quad.v1 = glyph.y - glyph.w;
                    quad.colour = item->colour;
                    if (quads == MaxQuadsPerCall)
                    {
                        ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
                        quads = 0;
                    }
                }

                glyphX = right;
            }

            taken += count;
            y = y + layout.lines[line].y;
        }

        if (quads != 0)
        {
            ps2e_gfx2_text_quads(reinterpret_cast<ps2e_u32>(material), reinterpret_cast<ps2e_u32>(g_Quads), quads);
        }
    }

    packets->count = 0;
}

void Font::End(TextPackets*)
{
    g_TextMaterial = nullptr;
}

// the platform's own texts (the fps counter, a mod's timer) over the game's, once a frame, through the GAME's text system
// (QueueText with the game's font: the same quads the menus and the HUD are drawn with). The host decides what to show.
namespace
{
constexpr s32 MaxPlatformTexts = 8;
te_platform_text g_PlatformTexts[MaxPlatformTexts];
}

void Platform::Graphics::QueuePlatformTexts(Renderer* renderer, Font* font)
{
    if (renderer == nullptr || font == nullptr || font->glyphs == nullptr)
    {
        return;
    }

    s32 count = ps2e_gfx2_platform_texts(reinterpret_cast<ps2e_u32>(g_PlatformTexts), MaxPlatformTexts);
    if (count <= 0)
    {
        return;
    }

    if (count > MaxPlatformTexts)
    {
        count = MaxPlatformTexts;
    }

    for (s32 i = 0; i < count; i++)
    {
        const te_platform_text& line = g_PlatformTexts[i];
        if (line.text[0] == '\0')
        {
            continue;
        }

        renderer->font = font;
        renderer->colour = line.colour;
        renderer->textScale.x = line.scale;
        renderer->textScale.y = line.scale;
        renderer->textAlignment.value = line.alignment;
        QueueText(renderer, line.text, line.x, line.y);
    }
}
