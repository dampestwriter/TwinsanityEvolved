// platform/nativeee/te_gfx_layout.h -- the GUEST-MEMORY layout of the nativeee arm's graphics objects, shared by the wasm
// side (platform/nativeee/wasm/graphics_*.cpp, compiled into the game module) and the host side
// (platform/nativeee/host/graphics.cpp). Every field is fixed-width; every "pointer" is a u32 guest address, so both
// sides read the same bytes (wasm address == guest address == MainMemory offset).
//
// The game holds these objects (Platform::Graphics::Material* / GameTexture* / MaterialResource::material, OLEG's
// MaterialStorage = 0x70 bytes of inline storage): they live on the game's heap or inside game structs and travel to
// the host as addresses, so the host reads a material's state from guest memory when it draws. The PS2 renderer's
// Material/Shader/Texture (src/platform/ps2/renderer/renderer.h) are NOT these: only their meaning is kept.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TE_GFX_MATERIAL_MAGIC = 0x544D4554u /* 'TEMT' */, TE_GFX_SHADER_MAGIC = 0x48534554u /* 'TESH' */,
       TE_GFX_MAX_SHADERS = 4 };

// What a material is for (TeMaterial::kind)
enum te_gfx_material_kind {
    TE_GFX_MATERIAL_RESOURCE = 0,   // read from a chunk (ReadMaterial): its shaders from the file
    TE_GFX_MATERIAL_FLAT = 1,       // MakeFlatMaterial: untextured UI shapes in their own colours, bucket 24
    TE_GFX_MATERIAL_PARTICLE = 2,   // MakeParticleMaterial: UI 2D particles, additive, the first particle page's texture
    TE_GFX_MATERIAL_DISTORTION = 3, // MakeDistortionMaterial
    TE_GFX_MATERIAL_SKID = 4,       // MakeSkidMaterial
};

// A shader of a material: the game's ShaderSettings bits (the PS2 renderer's union layout, bit for bit -- the file
// stores them as one byte per field and the arm packs them the same way), its texture, colour, scroll and type.
typedef struct te_gfx_shader {
    uint32_t magic;            // TE_GFX_SHADER_MAGIC
    uint32_t type;             // the shader type 0..0x20
    uint64_t settings;         // ShaderSettings::value (see te_gfx_settings_* below)
    uint32_t textureId;        // 0 = none
    uint32_t texture;          // GameTexture* (guest) taken from the texture table, or 0
    uint8_t colour[4];         // the colour bytes the shader draws with (RGBA, 0x80 alpha = 1)
    uint8_t fontPage;          // a font material's first shader: its page (SetFontPage)
    uint8_t pad0[3];
    int16_t lodK, lodL;        // TEX1's K and L
    float scroll[2];           // the UV payload this frame (shadertypes.cpp UpdateShader: the scroll modes / the animation's
                               // offset), written by StepAnimations (wasm side), added to every vertex's ST by the host
                               // builds. Shares the file's 16 leftover bytes (the tools', never read) so the struct keeps its
                               // 120 bytes: a shader is a game-heap allocation and its size class is part of the frozen heap
                               // layout
    float leftover[2];         // the rest of the tools' bytes
    float shaderColour[4];     // the colour as fractions (the file's)
    float scrollPhases[2];
    float scrollSpeeds[2];
    uint32_t animation;        // ShaderAnimation* (guest) or 0
    float extra[6];            // type-specific: 0x10/0x11/0x18 cornerValue; 0x17/0x1A speed, amplitudes; 0x1C speed, amplitude
    int32_t mode;              // cloth mode (0x17 / 0x1A)
} te_gfx_shader;
#ifdef __cplusplus
static_assert(sizeof(te_gfx_shader) == 120, "te_gfx_shader is a game-heap allocation: its size class is part of the frozen heap layout");
#endif

// A material: 0x70 bytes at most (Platform::Graphics::MaterialStorage).
typedef struct te_gfx_material {
    uint32_t magic;            // TE_GFX_MATERIAL_MAGIC
    uint32_t kind;             // te_gfx_material_kind
    uint32_t bucket;           // the render bucket (0..27) its draws go in
    uint32_t shaderCount;
    uint32_t shaders[TE_GFX_MAX_SHADERS];   // te_gfx_shader* (guest)
    uint64_t activatedShaders; // the file's key of the shaders' programs
    uint32_t ownsShaders;      // 1 = DestroyMaterial frees the shaders
    uint32_t reserved[13];
} te_gfx_material;

// A texture (Platform::Graphics::GameTexture): the game's resource header, the file's size word, the PS2 renderer's 0x60-
// byte Texture header AS THE FILE STORES IT (widthPower/heightPower, levels, formats, level offsets/widths, palette
// offset) and the arm's own words: the disk-manager handle of the image bytes and the host's texture handle.
typedef struct te_gfx_texture_header {   // the first 0x4C bytes of the file's Texture, then the tools' leftover
    uint16_t unused00;
    uint16_t unused02;
    uint16_t widthPower;
    uint16_t heightPower;
    uint8_t levels;
    uint8_t pixelFormat;       // TEX0.PSM of the texels
    uint8_t uploadFormat;      // BITBLTBUF.DPSM of the upload
    uint8_t hasAlpha;          // TEX0.TCC
    uint8_t unused0C;
    uint8_t function;          // TEX0.TFX
    uint8_t unused0E[2];
    uint32_t levelOffsets[7];  // 64-word units from the slot base
    uint32_t levelWidths[7];   // 64-pixel units
    uint32_t paletteOffset;    // 64-word units from the slot base
    uint8_t unused4C[0x54 - 0x4C];
    int32_t transfer;          // the arm's: the disk manager's handle of the image bytes (-1 none)
    uint32_t hostHandle;       // the arm's: ps2e_draw texture id (0 none)
    uint16_t unused5C;
    uint16_t secondContext;
} te_gfx_texture_header;       // 0x60 bytes, the PS2 Texture's size

typedef struct te_gfx_texture {
    uint32_t headerBits;       // ResourceHeader::bits (the game's)
    uint32_t id;               // ResourceHeader::id
    uint32_t size;             // the file's: header + leftover + image bytes
    te_gfx_texture_header texture;
} te_gfx_texture;              // 0x6C bytes, the PS2 GameTexture's size

// ---- the model resources (renderer.h RigidModelData / RigidModel / Skin / BlendSkin / Lod / Sky), the PS2's sizes:
// the game sizes two of them itself (shadows.cpp: ModelSize 0x1C, MeshSize 0x20) and the tables hold them by pointer. The
// submodel PACKETS stay in the disk manager exactly as on the PS2 (pool-region parity); the host decodes each packet
// once when it is read (ps2e_gfx2_model_loaded) and keeps the typed geometry by the model's guest address.
typedef struct te_gfx_model {         // RigidModelData, 0x1C: a model's submodels
    uint32_t headerBits, id;          // ResourceHeader
    uint32_t subModelCount;
    uint32_t vertexCounts;            // u32* (guest)
    uint32_t sizes;                   // u32* packet bytes
    uint32_t extraSizes;              // u32*
    uint32_t subModels;               // s32* disk-manager handles (array new: a cookie precedes it)
} te_gfx_model;

typedef struct te_gfx_rigid_model {   // RigidModel (an OGI's model or a scenery mesh), 0x20
    uint32_t headerBits, id;
    uint32_t materials;               // MaterialResource** (guest)
    uint32_t model;                   // RigidModelData* (guest) -- the host's geometry key
    uint32_t instanceBlock;           // InstanceBlockOwner::block: unused here (no instance blocks), kept 0
    uint8_t needsEye;                 // a shader of its materials needs the eye (types 0B 0C 0F 10 11 16 18 1B)
    uint8_t billboard;                // a shader turns it to face the camera (type 1B)
    uint8_t pad[2];
    uint32_t count;                   // its materials (submodels)
    uint32_t modelId;
} te_gfx_rigid_model;

typedef struct te_gfx_skin {          // Skin, 0x24 (read so the chunk stays in step; drawn by the skin path)
    uint32_t headerBits, id;
    uint32_t materials;               // MaterialResource**
    uint32_t vertexCounts;            // u32*
    uint32_t subModels;               // s32* disk handles (array new)
    uint32_t count;
    uint32_t sizes;                   // u32*
    uint32_t vertexTotal;
    uint32_t unused20;
} te_gfx_skin;

typedef struct te_gfx_blend_part {    // BlendPart, 0x28
    uint32_t shapeAddresses;          // u32*
    uint32_t shapeSizes;              // u32*
    uint32_t shapeCounts;             // u32*
    uint32_t packet;                  // u8* (heap)
    uint8_t shapeCount;
    uint8_t pad[3];
    float shapeFactors[3];
    uint32_t unused20;
    uint32_t packetSize;
} te_gfx_blend_part;

typedef struct te_gfx_blend_sub_model {   // BlendSubModel, 8 bytes
    uint32_t parts;                   // BlendPart**
    uint32_t count;
} te_gfx_blend_sub_model;

typedef struct te_gfx_blend_skin {    // BlendSkin, 0x18
    uint32_t headerBits, id;
    uint32_t materials;               // MaterialResource**
    uint32_t subModels;               // BlendSubModel**
    uint32_t count;
    uint32_t shapeCount;
} te_gfx_blend_skin;

enum { TE_GFX_LOD_SWITCHES = 3 };
typedef struct te_gfx_lod {           // Lod, 0x24: the squares of its distances
    uint32_t headerBits, id;
    uint32_t meshes;                  // RigidModel** (guest)
    uint8_t count;
    uint8_t pad[3];
    uint32_t nearest;
    uint32_t farthest;                // 0xFFFFFFFF = no limit
    uint32_t switchDistances[TE_GFX_LOD_SWITCHES];
} te_gfx_lod;

typedef struct te_gfx_sky {           // Sky, 0x50
    uint32_t headerBits, id;
    uint32_t count;
    uint32_t models;                  // RigidModel** (guest)
    float toScreen[4][4];             // the PS2 kept the half-size buffer's matrix here; unused here
} te_gfx_sky;

// ShaderSettings bit fields (src/platform/ps2/renderer/renderer.h ShaderSettings), as shifts and widths.
#define TE_GFX_SETTING(value, shift, width) ((uint32_t)(((value) >> (shift)) & ((1ull << (width)) - 1)))
#define TE_GFX_S_BLENDS(v) TE_GFX_SETTING(v, 0, 1)
#define TE_GFX_S_PRESET(v) TE_GFX_SETTING(v, 1, 4)
#define TE_GFX_S_ALPHA_TEST(v) TE_GFX_SETTING(v, 5, 1)
#define TE_GFX_S_ALPHA_METHOD(v) TE_GFX_SETTING(v, 6, 3)
#define TE_GFX_S_ALPHA_REFERENCE(v) TE_GFX_SETTING(v, 9, 8)
#define TE_GFX_S_ALPHA_FAIL(v) TE_GFX_SETTING(v, 17, 2)
#define TE_GFX_S_DESTINATION_TEST(v) TE_GFX_SETTING(v, 19, 1)
#define TE_GFX_S_DESTINATION_MODE(v) TE_GFX_SETTING(v, 20, 1)
#define TE_GFX_S_DEPTH_TEST(v) TE_GFX_SETTING(v, 21, 2)
#define TE_GFX_S_GOURAUD(v) TE_GFX_SETTING(v, 26, 1)
#define TE_GFX_S_TEXTURED(v) TE_GFX_SETTING(v, 27, 1)
#define TE_GFX_S_STQ(v) TE_GFX_SETTING(v, 28, 1)
#define TE_GFX_S_FOG(v) TE_GFX_SETTING(v, 29, 1)
#define TE_GFX_S_SECOND_CONTEXT(v) TE_GFX_SETTING(v, 30, 1)
#define TE_GFX_S_U_SCROLL(v) TE_GFX_SETTING(v, 32, 3)
#define TE_GFX_S_V_SCROLL(v) TE_GFX_SETTING(v, 35, 3)
#define TE_GFX_S_OWN_ALPHA(v) TE_GFX_SETTING(v, 38, 1)
#define TE_GFX_S_ALPHA_A(v) TE_GFX_SETTING(v, 39, 2)
#define TE_GFX_S_ALPHA_B(v) TE_GFX_SETTING(v, 41, 2)
#define TE_GFX_S_ALPHA_C(v) TE_GFX_SETTING(v, 43, 2)
#define TE_GFX_S_ALPHA_D(v) TE_GFX_SETTING(v, 45, 2)
#define TE_GFX_S_ALPHA_FIX(v) TE_GFX_SETTING(v, 47, 8)
#define TE_GFX_S_LINEAR(v) TE_GFX_SETTING(v, 55, 1)
#define TE_GFX_S_NO_FBA(v) TE_GFX_SETTING(v, 56, 1)
#define TE_GFX_S_ANTI_ALIASED(v) TE_GFX_SETTING(v, 58, 1)
#define TE_GFX_S_NO_DEPTH_WRITES(v) TE_GFX_SETTING(v, 59, 1)
#define TE_GFX_S_ANIMATED_COLOUR(v) TE_GFX_SETTING(v, 60, 1)
#define TE_GFX_S_HAS_ANIMATION(v) TE_GFX_SETTING(v, 61, 1)

// The shaders' alpha presets (renderer.h AlphaPreset) -> ALPHA (A, B, C, D) per InitAlphaPresets
enum te_gfx_alpha_preset { TE_GFX_PRESET_MIX = 0, TE_GFX_PRESET_ADD = 1, TE_GFX_PRESET_SUBTRACT = 2,
                           TE_GFX_PRESET_BRIGHTEN = 3, TE_GFX_PRESET_DARKEN = 4, TE_GFX_PRESET_SCALE = 5 };

// The frame buckets the arm names (renderer.h FrameBucket)
enum te_gfx_bucket { TE_GFX_BUCKET_SKY = 0, TE_GFX_BUCKET_OPAQUE = 2, TE_GFX_BUCKET_UI = 24, TE_GFX_BUCKET_FONTS = 26,
                     TE_GFX_BUCKET_MOVIES = 27, TE_GFX_BUCKET_COUNT = 28 };

// A text glyph quad the wasm side hands the host (Font::Draw): frame-pixel corners and the glyph's texture rectangle in
// texels of its page (the page is the material's), with the text's colour.
typedef struct te_gfx_text_quad {
    float x0, y0, x1, y1;      // GS pixel coordinates (1/16 units already divided: pixels, the frame corner subtracted)
    float u0, v0, u1, v1;      // texels of the page texture
    uint32_t colour;           // RGBA bytes
} te_gfx_text_quad;

// A text of the platform's own over the frame (the fps counter, a mod's timer), handed to the wasm side once a frame
// (ps2e_gfx2_platform_texts) and queued through the game's text system: where (fractions of the screen), the glyph scale
// (1 = the game's menu size), the colour (RGBA bytes), the alignment (game/font.h TextAlignment::Mask bits) and the text.
typedef struct te_platform_text {
    float x, y;
    float scale;
    uint32_t colour;
    uint32_t alignment;
    char text[64];
} te_platform_text;

#ifdef __cplusplus
}
#endif
