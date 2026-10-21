// platform/nativeee (host side) -- the SCENE of Platform::Graphics over NativeEE's typed draws: the chunk culling (the
// engine's CullingMath / CullingPlanes / ClippingMath, the VU0 microprograms of the second set as values), the rigid and
// placed models and the scenery meshes (their submodel packets decoded ONCE from the VIF UNPACKs the wasm side hands over
// when a model is read and PREPARED once for the engine's MeshGeometry, then every instance only transformed,
// clipped and projected by its BuildPrepared -- the VU1 strip programs' transform, six-plane clipping and projection as
// value arithmetic, bit-identical to Build's), the lit models' vertex colours (vs_lit_tristrip's formula, into a scratch
// per instance), the chunk's sky (drawn around the camera, the depth left at the far plane) and the shadow volumes (dark
// translucent stamps in the shadows' bucket; the half-size alpha buffer is not drawn). A submodel's strips under one unblended
// state go to the frame as ONE triangle-list draw (the order kept; the GS rasterised them in that order too), blended
// ones one draw per strip as the engine built them (the backend's blend reads a destination copy per draw).
//
// Everything arrives as u32 guest addresses: the models in te_gfx_layout.h's layouts (the wasm side's
// graphics_models.cpp writes them), the game's RenderView / ChunkView / RenderTargetDescription as the decomp lays them out.
// The draws go through graphics.cpp's buckets (graphics_internal.h).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"
#include "te_mesh_builder.h"
// The engine's skin path (SkinGeometry).
#include "games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.h"

namespace {

using te::gfx::Draw;
using te::gfx::MaterialView;
using Vector = title::CullingMath::Vector;
using Matrix = title::CullingMath::Matrix;
using Geometry = title::MeshGeometry;

// ---- the game's structs in guest memory ------------------------------------------------------------------------------------
struct RenderView {   // include/game/view.h, 0x170 bytes
    float projection[4][4], toCamera[4][4], toClip[4][4], toScreen[4][4], cameraToScreen[4][4];
    float clip[4], forward[4];
    uint32_t cameraObject;
    float inverseAspect;
    uint8_t unused168[8];
};
struct ChunkView {   // include/game/scenery.h ChunkView = renderer.h PlacedObject (0x108 bytes)
    float toClip[4][4];    // 0x00
    float clipped[4][4];   // 0x40 the last culling test's matrix to the clip space (toCamera)
    float toScreen[4][4];  // 0x80
    float clip[4];         // 0xC0
    float origin[4];       // 0xD0
    float camera[4];       // 0xE0
    int32_t distance;      // 0xF0
    uint32_t view;         // 0xF4 RenderView*
    uint32_t visibility;   // 0xF8 the draw mode: 1 wholly in view, 2 partly
    uint32_t lastVisibility;
    uint32_t matrices;     // 0x100
    uint32_t vtable;
};
struct ModelLights {   // Platform::Graphics::ModelLights
    uint32_t directions, colours, ambient;
};
// Retail globals (PAL SLES-52568, game/symbol_addrs.txt): the scene's view, the frustum planes, the shadows' matrices
constexpr uint32_t kRenderViewPointer = 0x00309BE0;    // G_RendRel
constexpr uint32_t kViewPlanes = 0x003B4900;           // D_003B4900 g_ViewPlanes[6]
constexpr uint32_t kFarViewPlanes = 0x003B4960;        // D_003B4960 g_FarViewPlanes[6]
constexpr uint32_t kShadowToCamera = 0x003B0770;       // D_003B0770 g_ShadowToCamera
constexpr uint32_t kShadowToScreen = 0x003B07B0;       // D_003B07B0 g_ShadowToScreen
constexpr uint32_t DrawUnclipped = 1;
constexpr uint32_t BucketSky = 0, BucketOpaque = 2, BucketShadows = 21;

template <typename T>
const T* Guest(uint32_t address) {
    return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T)));
}
template <typename T>
T* GuestMutable(uint32_t address) {
    return reinterpret_cast<T*>(te::GuestPointer(address, sizeof(T)));
}
// The interpolator's tags (graphics_internal.h SetDrawTag): a matrix flattened row-major, the tag of an object with its
// model->world and model->screen matrices as drawn, cleared after the object's draws
void Flat(const Matrix& m, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[r][c];
}
void TagDiag(uint64_t id, const Matrix& world, const Matrix& screen);
// The world matrix the interpolator needs for a draw projected by `screen`: the one with world . toCamera . cameraToScreen
// == screen under the current view (a placed model's screen matrix carries its chunk's matrix; a rigid model's equals
// world . toScreen already). W = screen . (toCamera . cameraToScreen)^-1; nullopt when the view is missing or singular.
std::optional<Matrix> WorldOfScreen(const Matrix& screen);
void TagRigid(uint64_t id, const Matrix& world, const Matrix& screen) {
    float w[16], sc[16];
    Flat(world, w);
    Flat(screen, sc);
    te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{id, 0, ps2r::ps2e::InterpKind::Rigid}, w, sc);
    TagDiag(id, world, screen);
}
uint64_t TagId(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | uint64_t(b); }
// The k-th draw of `model` through `matrix` this frame: a producer that draws several instances of one model through ONE
// matrix address (a stack local, a frame-arena slot: an OGI's jointed rigid pieces, a chunk's placements) names them by
// their order within the picture -- the scene is traversed in the same order every frame, so the ordinal is a stable identity.
// Reset at Present (the picture's end).
std::unordered_map<uint64_t, uint32_t> g_drawOrdinals;
uint64_t OrdinalId(uint64_t base) {
    const uint32_t k = g_drawOrdinals[base]++;
    return base ^ (uint64_t(k) * 0xD6E8FEB86659FD93ull);
}

std::optional<Matrix> ReadMatrix(uint32_t address) {
    const auto* m = reinterpret_cast<const float(*)[4]>(te::GuestPointer(address, 64));
    if (!m) return std::nullopt;
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
Matrix ToMatrix(const float (*m)[4]) {
    Matrix out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return out;
}
std::optional<Vector> ReadVector(uint32_t address) {
    const float* v = reinterpret_cast<const float*>(te::GuestPointer(address, 16));
    if (!v) return std::nullopt;
    return Vector{v[0], v[1], v[2], v[3]};
}
void WriteMatrix(uint32_t address, const Matrix& m) {
    auto* out = reinterpret_cast<float(*)[4]>(te::GuestPointer(address, 64));
    if (!out) return;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
}
// Row-vector matrix product (the game's VuMultiplyMatrices: out = a * b, p' = p * a * b)
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[r][k] * b[k][c];
            out[r][c] = s;
        }
    return out;
}
Matrix Identity() { return Matrix{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; }
Matrix Translation(const Vector& t) {
    Matrix m = Identity();
    m[3] = {t[0], t[1], t[2], 1.f};
    return m;
}
// The affine inverse (a rotation / scale with a translation in row 3, the PS2's row-vector convention): the 3x3 inverted by
// cofactors, the translation carried back; nullopt when singular
std::optional<Matrix> AffineInverse(const Matrix& m) {
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float r = 1.f / det;
    Matrix o = Identity();
    o[0][0] = (e * i - f * h) * r; o[0][1] = (c * h - b * i) * r; o[0][2] = (b * f - c * e) * r;
    o[1][0] = (f * g - d * i) * r; o[1][1] = (a * i - c * g) * r; o[1][2] = (d * c - a * f) * r;
    o[2][0] = (d * h - e * g) * r; o[2][1] = (b * g - a * h) * r; o[2][2] = (a * e - b * d) * r;
    for (int k = 0; k < 3; ++k) {
        o[3][k] = -(m[3][0] * o[0][k] + m[3][1] * o[1][k] + m[3][2] * o[2][k]);
        o[k][3] = 0.f;
    }
    o[3][3] = 1.f;
    return o;
}
// The camera's position in a model's space (models.cpp EyeIn / CameraPosition x the instance's inverse): the point the
// view's toCamera sends to the origin, carried through the world matrix's inverse
std::optional<Vector> EyeIn(const Matrix& world, const float (*toCamera)[4]) {
    const auto invCam = AffineInverse(ToMatrix(toCamera));
    const auto invWorld = AffineInverse(world);
    if (!invCam || !invWorld) return std::nullopt;
    Vector eye{};
    for (int k = 0; k < 3; ++k) eye[k] = (*invCam)[3][k];
    eye[3] = 1.f;
    Vector o{};
    for (int k = 0; k < 3; ++k) o[k] = eye[0] * (*invWorld)[0][k] + eye[1] * (*invWorld)[1][k] + eye[2] * (*invWorld)[2][k] + (*invWorld)[3][k];
    o[3] = 1.f;
    for (int k = 0; k < 3; ++k) if (!std::isfinite(o[k])) return std::nullopt;
    return o;
}

bool Finite(const Matrix& m) {
    for (const auto& row : m)
        for (float v : row)
            if (!std::isfinite(v)) return false;
    return true;
}

const RenderView* CurrentView() {
    const uint32_t* p = Guest<uint32_t>(kRenderViewPointer);
    return p && *p ? Guest<RenderView>(*p) : nullptr;
}

std::optional<Matrix> WorldOfScreen(const Matrix& screen) {
    const RenderView* view = CurrentView();
    if (!view) return std::nullopt;
    // in double throughout: the camera->screen matrix is projective with entries ~1e6 beside ~1; a float product and
    // inverse leave a shear of a few parts per thousand in the world's basis, which a far object's re-projection shows
    double cam[16], c2s[16], ws[16], inv[16], sc[16], w[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            cam[r * 4 + c] = view->toCamera[r][c];
            c2s[r * 4 + c] = view->cameraToScreen[r][c];
            sc[r * 4 + c] = screen[r][c];
        }
    ps2r::ps2e::SceneInterpolator::Multiply4(cam, c2s, ws);
    if (!ps2r::ps2e::SceneInterpolator::Invert4(ws, inv)) return std::nullopt;
    ps2r::ps2e::SceneInterpolator::Multiply4(sc, inv, w);
    Matrix out;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r][c] = float(w[r * 4 + c]);
    if (!Finite(out)) return std::nullopt;
    return out;
}

// TE_GFX_INTERP_DIAG: the tag matrices' consistency -- the interpolator rebuilds model->screen as world . toCamera .
// cameraToScreen; a producer whose drawn matrix is not that product would be re-projected wrongly
void TagDiag(uint64_t id, const Matrix& world, const Matrix& screen) {
    static const bool diag = std::getenv("TE_GFX_INTERP_DIAG") != nullptr;
    if (!diag) return;
    static int said = 0;
    const RenderView* view = CurrentView();
    if (!view || said >= 12) return;
    const Matrix rebuilt = Multiply(Multiply(world, ToMatrix(view->toCamera)), ToMatrix(view->cameraToScreen));
    const Matrix viaScreen = Multiply(world, ToMatrix(view->toScreen));
    float worst = 0, worst2 = 0, scale = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            worst = std::max(worst, std::fabs(rebuilt[r][c] - screen[r][c]));
            worst2 = std::max(worst2, std::fabs(viaScreen[r][c] - screen[r][c]));
            scale = std::max(scale, std::fabs(screen[r][c]));
        }
    std::fprintf(stderr, "[te.gfx.interp] tag %llx: |world.toCamera.cameraToScreen - drawn| %.4g, |world.toScreen - drawn| %.4g (drawn max %.4g)\n",
                 (unsigned long long)id, worst, worst2, scale);
    // the round trip the interpolator does at fraction 1: a model point projected, the drawn vertex taken back through the
    // inverse and forward again (K = S^-1 . world . toCamera . cameraToScreen = identity): the error in frame pixels
    double S[16], Sinv[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) S[r * 4 + c] = screen[r][c];
    if (ps2r::ps2e::SceneInterpolator::Invert4(S, Sinv)) {
        const double pt[4] = {0.3, -0.2, 0.5, 1.0};
        double h[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) h[c] += pt[k] * S[k * 4 + c];
        const double X = h[0] / h[3], Y = h[1] / h[3], Z = double(uint32_t(int64_t(h[2] / h[3])));
        double b[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) b[c] = X * Sinv[c] + Y * Sinv[4 + c] + Z * Sinv[8 + c] + Sinv[12 + c];
        std::fprintf(stderr, "[te.gfx.interp]   round trip: model (%.3f %.3f %.3f) -> screen (%.1f %.1f %.1f w %.4f) -> model (%.4f %.4f %.4f w %.4f) -> %.4f %.4f %.4f\n",
                     pt[0], pt[1], pt[2], X, Y, Z, h[3], b[0], b[1], b[2], b[3], b[0] / b[3], b[1] / b[3], b[2] / b[3]);
        std::fprintf(stderr, "[te.gfx.interp]   S rows: (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g) (%.3g %.3g %.3g %.3g)\n",
                     S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[8], S[9], S[10], S[11], S[12], S[13], S[14], S[15]);
    }
    said++;
}

void Once(const char* what) {
    static std::unordered_map<std::string, bool> said;
    bool& s = said[what];
    if (!s) {
        s = true;
        std::fprintf(stderr, "[te.gfx] %s\n", what);
    }
}

// ---- the decoded models ---------------------------------------------------------------------------------------------------
// A submodel: the strips the VIF packet UNPACKs for the VU1 strip programs (positions, STQ with the colour bytes in the low
// byte of each word, the lit models' normals and their separate colour bytes). Kept as the engine's Mesh (its Build consumes
// it) plus the normals and the static colours it needs when lit.
struct Part {
    Geometry::Mesh mesh;                                      // the decoded strips (the prepared form's source)
    te::gfx::MeshPrepared prepared;                           // prepared once per decode for every instance's build
    std::vector<std::vector<std::array<float, 3>>> normals;   // per strip, per vertex; empty when the packet has none
    std::vector<std::vector<std::array<uint8_t, 4>>> colours; // the V4-8 colour stream (lit models); empty when none
    bool decoded = false;
    uint32_t vertices = 0;
    size_t triangles = 0;
};
// Each submodel's packet BYTES, kept host-side: the game releases the packet's disk-manager memory after ReadModel
// (resources.cpp DeferRelease), so a save state cannot re-read it from RAM0; the state carries these and the restore
// decodes them again (te::gfx::ModelSources / ReplayModelSources), as graphics.cpp keeps the textures' packets
struct PartSource {
    int32_t vertices = 0;
    std::vector<uint8_t> packet;
};
struct Model {
    std::vector<Part> parts;
    std::vector<PartSource> sources;
};
std::unordered_map<uint32_t, Model> g_models;   // RigidModelData guest address -> its parts
// The shadow meshes' geometry as the type-0B program saw it (title::ShadowGeometry: each strip emitted twice, the
// front-facing triangles +8 and the back-facing 248 by the screen-space winding, the clipped fans by the authored flag),
// decoded once per model from the kept packet bytes at the first shadow draw
struct ShadowModel { std::vector<title::ShadowGeometry::Mesh> parts; std::vector<bool> decoded; bool tried = false; };
std::unordered_map<uint32_t, ShadowModel> g_shadowModels;
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices);

// The VIF UNPACK's size in 32-bit words (the data follows the code word-aligned)
bool UnpackWords(uint32_t cmd, uint32_t num, size_t& words) {
    const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    const uint32_t fields = vn + 1;
    if (num == 0) num = 256;
    switch (vl) {
        case 0: words = size_t(num) * fields; return true;                      // 32-bit
        case 1: words = (size_t(num) * fields * 2 + 3) / 4; return true;         // 16-bit
        case 2: words = (size_t(num) * fields + 3) / 4; return true;             // 8-bit
        case 3: if (vn != 3) return false; words = (size_t(num) * 2 + 3) / 4; return true;   // V4-5
    }
    return false;
}

// The packet: a DMA chain fragment (its RET/CNT tags carry two VIF codes each, TTE), the VIF stream in between. Strips are
// delimited by MSCAL; within a strip the UNPACKs are classified by their VU address: 3 = positions (V3-32), 4 = STQ + colour
// bytes (V4-32), 5 = normals (V3-32), 6 = colours (V4-8). Other layouts fall back to "first V3-32 = positions, the V4-32 = STQ".
bool DecodePacket(const uint8_t* bytes, size_t size, uint32_t vertices, Part& part, std::string* why) {
    auto word = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, bytes + at, 4);
        return v;
    };
    if (size < 16 || (size & 3)) { if (why) *why = "size"; return false; }
    size_t cursor = 0;
    std::vector<float> pos, stq, nrm;
    std::vector<uint32_t> stqBits, col8;
    bool havePos = false, haveStq = false;
    uint32_t total = 0;
    auto flushStrip = [&]() -> bool {
        if (!havePos) return true;   // a packet prologue with no vertices
        const size_t n = pos.size() / 3;
        if (n < 1) { havePos = haveStq = false; pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear(); return true; }
        Geometry::Strip strip;
        strip.vertices.resize(n);
        std::vector<std::array<float, 3>> normals;
        std::vector<std::array<uint8_t, 4>> colours;
        for (size_t i = 0; i < n; ++i) {
            auto& v = strip.vertices[i];
            for (int k = 0; k < 3; ++k) {
                v.position[k] = pos[i * 3 + k];
                if (!std::isfinite(v.position[k])) v.position[k] = 0.f;
            }
            if (haveStq && stq.size() >= (i + 1) * 4) {
                for (int k = 0; k < 3; ++k) {
                    v.stq[k] = stq[i * 4 + k];
                    if (!std::isfinite(v.stq[k])) v.stq[k] = k == 2 ? 1.f : 0.f;
                }
                for (int k = 0; k < 4; ++k) v.colour[k] = uint8_t(stqBits[i * 4 + k]);
                v.restart = (stqBits[i * 4 + 3] & 0x8000) != 0;
            } else {
                v.stq = {0.f, 0.f, 1.f};
                v.colour = {0x80, 0x80, 0x80, 0x80};
            }
            if (nrm.size() >= (i + 1) * 3) normals.push_back({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]});
            if (col8.size() > i) {
                const uint32_t c = col8[i];
                colours.push_back({uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), uint8_t(c >> 24)});
            }
        }
        if (normals.size() == n) part.normals.push_back(std::move(normals)); else part.normals.push_back({});
        if (colours.size() == n) part.colours.push_back(std::move(colours)); else part.colours.push_back({});
        total += uint32_t(n);
        part.triangles += n >= 3 ? n - 2 : 0;
        part.mesh.strips.push_back(std::move(strip));
        havePos = haveStq = false;
        pos.clear(); stq.clear(); stqBits.clear(); nrm.clear(); col8.clear();
        return true;
    };
    // The VIF code stream between `from` and `to` (words)
    auto vif = [&](size_t from, size_t to) -> bool {
        size_t at = from;
        while (at + 4 <= to) {
            const uint32_t code = word(at);
            at += 4;
            const uint32_t cmd = (code >> 24) & 0x7F;
            const uint32_t num = (code >> 16) & 0xFF;
            const uint32_t imm = code & 0xFFFF;
            if (cmd >= 0x60) {
                size_t words;
                if (!UnpackWords(cmd, num, words)) { if (why) *why = "unpack format"; return false; }
                if (at + words * 4 > to) { if (why) *why = "unpack overruns"; return false; }
                const uint32_t addr = imm & 0x3FF;
                const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
                const uint32_t count = num ? num : 256;
                if (vl == 0 && vn == 2) {   // V3-32
                    if (!havePos || addr == 3) {
                        if (havePos && addr == 3) { if (!flushStrip()) return false; }
                        pos.resize(size_t(count) * 3);
                        std::memcpy(pos.data(), bytes + at, size_t(count) * 12);
                        havePos = true;
                    } else {   // the second V3-32 stream: normals
                        nrm.resize(size_t(count) * 3);
                        std::memcpy(nrm.data(), bytes + at, size_t(count) * 12);
                    }
                } else if (vl == 0 && vn == 3 && count > 1) {   // V4-32 of the vertices (the one-quadword headers are skipped)
                    stq.resize(size_t(count) * 4);
                    stqBits.resize(size_t(count) * 4);
                    std::memcpy(stq.data(), bytes + at, size_t(count) * 16);
                    std::memcpy(stqBits.data(), bytes + at, size_t(count) * 16);
                    haveStq = true;
                } else if (vl == 2 && vn == 3) {   // V4-8: colours
                    col8.resize(count);
                    std::memcpy(col8.data(), bytes + at, size_t(count) * 4);
                }
                at += words * 4;
                continue;
            }
            switch (cmd) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x10: case 0x11: case 0x13: case 0x15: case 0x17:
                    break;
                case 0x14:   // MSCAL: the strip's program runs on what was unpacked
                    if (!flushStrip()) return false;
                    break;
                case 0x20: at += 4; break;              // STMASK
                case 0x30: case 0x31: at += 16; break;  // STROW / STCOL
                case 0x4A: at += size_t(num ? num : 256) * 8; break;   // MPG
                case 0x50: case 0x51: at += size_t(imm ? imm : 65536) * 16; break;   // DIRECT
                default:
                    if (why) *why = "vif code";
                    return false;
            }
        }
        return true;
    };
    // The DMA tags
    while (cursor + 16 <= size) {
        const uint32_t tag0 = word(cursor);
        const uint32_t qwc = tag0 & 0xFFFF;
        const uint32_t id = (tag0 >> 28) & 7;
        if (!vif(cursor + 8, cursor + 16)) return false;   // the tag's own two VIF codes
        cursor += 16;
        const size_t end = cursor + size_t(qwc) * 16;
        if (id == 3 || id == 4) {   // REF/REFS: the data is elsewhere (not in this packet); nothing follows inline
            continue;
        }
        if (end > size) { if (why) *why = "tag overruns"; return false; }
        if (!vif(cursor, end)) return false;
        cursor = end;
        if (id == 6 || id == 7) break;   // RET / END
        if (id == 2 || id == 5) break;   // NEXT / CALL: cannot follow an address
    }
    if (!flushStrip()) return false;
    part.vertices = total;
    part.decoded = !part.mesh.strips.empty();
    if (!part.decoded && why) *why = "no strips";
    (void)vertices;
    return part.decoded;
}

// ---- building an instance's draws -------------------------------------------------------------------------------------------
// vs_lit_tristrip: lit = L3 + L0*max(n.l0, 0) + L1*max(n.l1, 0) + L2*max(n.l2, 0); RGB = min(col * lit, 255) truncated
struct Lights {
    std::array<Vector, 3> directions;   // in the model's space
    std::array<Vector, 3> colours;
    Vector ambient;
};
// The instance's lit colours, one per vertex in strip order (the prepared mesh's order), into a scratch the build reads
void LitColours(const Part& part, const Lights& lights, std::vector<std::array<uint8_t, 4>>& out) {
    out.resize(part.prepared.vertex_count);
    size_t at = 0;
    for (size_t s = 0; s < part.mesh.strips.size(); ++s) {
        const auto& strip = part.mesh.strips[s];
        const bool normals = s < part.normals.size() && part.normals[s].size() == strip.vertices.size();
        const bool colours = s < part.colours.size() && part.colours[s].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i, ++at) {
            const auto& v = strip.vertices[i];
            std::array<uint8_t, 4> col = colours ? part.colours[s][i] : v.colour;
            float lit[3] = {lights.ambient[0], lights.ambient[1], lights.ambient[2]};
            if (normals) {
                const auto& n = part.normals[s][i];
                for (int l = 0; l < 3; ++l) {
                    const auto& d = lights.directions[l];
                    const float dot = std::max(0.f, n[0] * d[0] + n[1] * d[1] + n[2] * d[2]);
                    for (int k = 0; k < 3; ++k) lit[k] += lights.colours[l][k] * dot;
                }
            }
            auto& o = out[at];
            for (int k = 0; k < 3; ++k) {
                const float value = float(col[k]) * lit[k];
                o[k] = uint8_t(std::min(255.f, std::max(0.f, value)));
            }
            o[3] = col[3];
        }
    }
}

// The engine's Build wants a textured strip state; the draw's own state is applied afterwards
ps2r::gs::DrawIr BuildState() {
    ps2r::gs::DrawIr st{};
    st.prim_type = 4;
    st.tex_enabled = true;
    st.off_x = te::gfx::GsOriginX();
    st.off_y = te::gfx::GsOriginY();
    return st;
}

// ---- diagnosis: the draws of ONE present logged, each with its state and screen box --------------------------------
// TE_GFX_DIAG_PRESENT=N logs every model instance's submodel draw recorded for present N (the frame dumped as frame_N):
// kind, model, submodel, clipped or not, bucket, shader type, texture, blend/alpha state, triangles, the screen box and the
// mean vertex colour. TE_GFX_DIAG_FLAT=1 drops the textures and paints by clip mode (unclipped red, clipped green, the
// override state's blue) so a frame shows WHICH geometry covers a region. With the present's log: each submodel's
// model-space box and each placed instance's world matrix; TE_GFX_DIAG_MESH=<model> dumps that model's strips' vertices;
// TE_GFX_DIAG_SKIP_BUCKET=a,b leaves those buckets' model draws out.
extern "C" uint64_t te_graphics_presents();
struct Diag {
    long present = -1;
    bool flat = false;
    bool read = false;
    void Read() {
        if (read) return;
        read = true;
        if (const char* p = std::getenv("TE_GFX_DIAG_PRESENT")) present = std::atol(p);
        flat = std::getenv("TE_GFX_DIAG_FLAT") != nullptr;
    }
    bool Logging() { Read(); return present >= 0 && te_graphics_presents() + 1 == uint64_t(present); }
} g_diag;
struct DiagBox {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    double r = 0, g = 0, b = 0, a = 0;
    size_t n = 0;
    void Add(const ps2e_draw_vertex& v) {
        x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
        r += v.r; g += v.g; b += v.b; a += v.a; n++;
    }
};

// The built vertices (frame pixels, GS depth, STQ) become triangle-list draws under the material's state, appended
// straight into the bucket's open draw (the bucket merges consecutive unblended draws of one state -- a submodel's
// strips and fans, and the instances of one material after it -- into ONE draw in their order; blended states stay one
// draw per published strip/fan, as the backend may read the destination before each draw). The bucket owns the vertex
// storage; nothing is allocated per build once the buckets have grown.
// One built vertex as the draw records it: the GS's STQ as the program left it on a textured state (the engine interpolates
// S, T and Q over the screen and divides per pixel, so the mapping is perspective-correct -- divided here per vertex it was
// affine per triangle: the seams across the intro's water tiles at grazing angles)
ps2e_draw_vertex DrawVertex(const ps2r::gs::VertexRec& v, bool textured) {
    ps2e_draw_vertex o{};
    o.x = v.x;
    o.y = v.y;
    o.z = v.z;
    if (textured && v.q != 0.f) {
        o.u = v.s;
        o.v = v.t;
        o.q = v.q;
    }
    o.r = v.r;
    o.g = v.g;
    o.b = v.b;
    o.a = v.a;
    return o;
}

struct Emitter {
    uint32_t bucket = 0;
    ps2e_draw_state state{};
    bool textured = false;
    size_t triangles = 0;
    DiagBox* box = nullptr;        // diagnosis: the emitted vertices' box and mean colour
    int flatTint = -1;             // diagnosis: 0 unclipped, 1 clipped, 2 override state (TE_GFX_DIAG_FLAT)
    // the screen-copy pass (shader types 0x10 / 0x11; the crystal program vs_crys_dual's 0x1720 sub): the texture is
    // the frame's copy and each vertex samples it at its own screen position (the fraction of the frame) displaced by the
    // built STQ -- the pass's mesh carries (n_clip.x, n_clip.y, 1) * cornerValue as its STQ, the normal through the clip
    // matrix's three rows (StMtx) -- clamped to the copy, plus the shader's scroll (P2); the first Append asks for the copy
    bool screenCopy = false;
    bool copyPending = false;
    std::array<float, 2> copyScroll{};
    void Begin(uint32_t b, const ps2e_draw_state& st) {
        bucket = b;
        state = st;
        g_diag.Read();
        if (g_diag.flat) { state.texture = 0; state.blend = 0; state.alpha_test = 0; }
        textured = state.texture != 0;
        screenCopy = false;
        copyPending = false;
    }
    void BeginScreenCopy(uint32_t b, const ps2e_draw_state& st, std::array<float, 2> scroll) {
        Begin(b, st);
        screenCopy = textured;
        copyPending = screenCopy;
        copyScroll = scroll;
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        Draw& d = te::gfx::OpenDraw(bucket, state, copyPending);
        copyPending = false;
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        const float invW = 1.f / float(std::max(1, te::gfx::ScreenW())), invH = 1.f / float(std::max(1, te::gfx::ScreenH()));
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = d.verts[base + i];
            o = DrawVertex(v, textured && !screenCopy);
            if (screenCopy) {
                const float ds = v.q != 0.f ? v.s / v.q : 0.f, dt = v.q != 0.f ? v.t / v.q : 0.f;
                o.u = std::min(1.f, std::max(0.f, v.x * invW + ds)) + copyScroll[0];
                o.v = std::min(1.f, std::max(0.f, v.y * invH + dt)) + copyScroll[1];
            }
            if (box) box->Add(o);
            if (g_diag.flat && flatTint >= 0) {
                o.r = flatTint == 0 ? 0xC0 : 0x20;
                o.g = flatTint == 1 ? 0xC0 : 0x20;
                o.b = flatTint == 2 ? 0xC0 : 0x20;
                o.a = 0x80;
            }
        }
        te::gfx::CloseDraw(bucket);
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Emitter*>(user)->Append(verts, count);
    }
    // One build of a prepared submodel under this emitter's state; the triangles recorded are returned
    size_t Build(const Part& part, const Geometry::View& view, const std::array<uint8_t, 4>* colours, Geometry::Result* result) {
        const size_t before = triangles;
        const Geometry::Result r = te::gfx::MeshBuild(part.prepared, view, BuildState(), colours, &Sink, this);
        if (result) *result = r;
        return triangles - before;
    }
};
std::vector<std::array<uint8_t, 4>> g_colourScratch;   // the lit colours / the shadows' stamp colour per vertex

// The Emitter's appends on a worker (te::gfx::DeferredBuild): the same vertices, each Append's triangles one segment
struct Collector {
    te::gfx::DeferredBuild* out = nullptr;
    bool textured = false;
    size_t triangles = 0;
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        const size_t base = out->verts.size();
        out->verts.resize(base + count);
        for (size_t i = 0; i < count; ++i) out->verts[base + i] = DrawVertex(verts[i], textured);
        out->segments.push_back(uint32_t(count));
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<Collector*>(user)->Append(verts, count);
    }
};
// The triangles the workers built, folded into the scene's census on the game thread
std::atomic<uint64_t> g_deferredModelTriangles{0}, g_deferredSkinTriangles{0};
// Whether this draw's build may run on a worker: workers running, nothing logged or tinted for diagnosis
bool Deferrable(bool logging) {
    return !logging && !g_diag.flat && te::gfx::DeferringBuilds();
}


// One instance of a model: every submodel the model and its materials both have, through the view
struct InstanceState {
    const ps2e_draw_state* overrideState = nullptr;   // the shadows' stamp state (else each submodel's material)
    uint32_t overrideBucket = 0;
    const Lights* lights = nullptr;
    std::optional<Vector> eye;                        // the camera in the model's space (the env-map passes' half vector)
};
// the screen-copy pass's mesh -- the part's strips with the STQ replaced by the clip-space normal's x / y (the
// normal through the clip matrix's three rows, as the crystal program's StMtx) times the shader's cornerValue, q = 1;
// prepared per instance (a scratch: the crystals are a few thousand vertices)
struct ScreenCopyScratch {
    Geometry::Mesh mesh;
    te::gfx::MeshPrepared prepared;
} g_screenCopyScratch;
bool PrepareScreenCopyMesh(const Part& part, const Matrix& clip, float cornerValue) {
    auto& sc = g_screenCopyScratch;
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float dx = 0.f, dy = 0.f;
            if (normals) {
                const auto& n = part.normals[st][i];
                dx = (n[0] * clip[0][0] + n[1] * clip[1][0] + n[2] * clip[2][0]) * cornerValue;
                dy = (n[0] * clip[0][1] + n[1] * clip[1][1] + n[2] * clip[2][1]) * cornerValue;
            }
            v.stq = {std::isfinite(dx) ? dx : 0.f, std::isfinite(dy) ? dy : 0.f, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}

// the env-map pass's mesh (shader types 0x0C / 0x0F; the VU1 library's TexturedTristrip::RunEnvCall): per vertex
// the half vector h = normalize(eye - pos) + n in the model's space, env = (h / 2) through the clip matrix's three rows, the
// sphere map u = 0.5 * envn.x + 0.5, v = -0.5 * envn.y + 0.5 over the normalised env, clamped to [0, 1], q = 1 (the shader's
// scroll is the view's payload, added by the build as the micro added it after the clamp)
bool PrepareEnvMesh(const Part& part, const Matrix& clip, const Vector& eye) {
    auto& sc = g_screenCopyScratch;
    sc.mesh = part.mesh;
    for (size_t st = 0; st < sc.mesh.strips.size(); ++st) {
        auto& strip = sc.mesh.strips[st];
        const bool normals = st < part.normals.size() && part.normals[st].size() == strip.vertices.size();
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            auto& v = strip.vertices[i];
            float u = 0.5f, t = 0.5f;
            if (normals) {
                const auto& n = part.normals[st][i];
                float e[3], h2[3], env[3];
                float d2 = 0.f;
                for (int k = 0; k < 3; ++k) { e[k] = eye[k] - v.position[k]; d2 += e[k] * e[k]; }
                const float rsq = d2 > 0.f ? 1.f / std::sqrt(d2) : 0.f;
                for (int k = 0; k < 3; ++k) h2[k] = (e[k] * rsq + n[k]) * 0.5f;
                for (int k = 0; k < 3; ++k) env[k] = h2[0] * clip[0][k] + h2[1] * clip[1][k] + h2[2] * clip[2][k];
                const float d2v = env[0] * env[0] + env[1] * env[1] + env[2] * env[2];
                const float rsqv = d2v > 0.f ? 1.f / std::sqrt(d2v) : 0.f;
                u = 0.5f * env[0] * rsqv + 0.5f;
                t = -0.5f * env[1] * rsqv + 0.5f;
            }
            u = std::isfinite(u) ? std::min(1.f, std::max(0.f, u)) : 0.5f;
            t = std::isfinite(t) ? std::min(1.f, std::max(0.f, t)) : 0.5f;
            v.stq = {u, t, 1.f};
        }
    }
    return te::gfx::MeshPrepare(sc.mesh, sc.prepared) == Geometry::Result::Ok;
}
bool EnvType(uint32_t type) { return type == 0x0C || type == 0x0F; }

// The shader types a later pass of a material is drawn as (TR: a material's shaders are its passes over the same strips,
// the VU1 dispatcher walking them in order): the plain and lit strip families with the mesh's own STQ, and the screen
// copies. Types whose program derives other coordinates (0x0C / 0x0F env maps, 0x18 distortion, the cloth 0x17 / 0x1A...)
// are not drawn as later passes (the first shader is always drawn, as before)
bool PassDrawable(uint32_t type, bool haveEye) {
    switch (type) {
        case 0x00: case 0x01: case 0x02: case 0x04: case 0x0A: case 0x0D: case 0x0E: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x15: case 0x16: case 0x19: case 0x1B: case 0x1E: return true;
        case 0x0C: case 0x0F: return haveEye;   // the env-map seconds, when the instance's eye is known
        default: return false;
    }
}
bool ScreenCopyType(uint32_t type) { return type == 0x10 || type == 0x11; }

void DrawInstance(const te_gfx_rigid_model* rigid, const Geometry::View& view, const InstanceState& inst, uint64_t& counter) {
    auto& scene = te::gfx::Scene();
    scene.model_triangles += g_deferredModelTriangles.exchange(0, std::memory_order_relaxed);
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;   // diagnosis: culling without the draws
    // Diagnosis: TE_GFX_NO_PASSES=1 draws the first shader only (the single-pass picture: the judge's reference arm)
    static const bool firstPassOnly = std::getenv("TE_GFX_NO_PASSES") != nullptr;
    if (noDraw) return;
    const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
    if (!data) return;
    auto it = g_models.find(rigid->model);
    if (it == g_models.end()) {
        scene.submodels_missing++;
        return;
    }
    const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(rigid->materials, 4 * size_t(count))) : nullptr;
    static Emitter emitter;
    for (uint32_t i = 0; i < count && i < it->second.parts.size(); ++i) {
        const Part& part = it->second.parts[i];
        if (!part.decoded) continue;
        const uint32_t resource = materials ? materials[i] : 0;
        const uint32_t* materialAddress = (!inst.overrideState && resource) ? Guest<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
        const MaterialView material = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
        // the passes: the override state is one pass; else the material's shaders in order (the first always, the later
        // ones when their type is drawable as a pass -- the crystals' [0x10 screen copy] [0x02 lit] pairs)
        const uint32_t passes = inst.overrideState || firstPassOnly || !material.material
                                    ? 1u : std::max<uint32_t>(1u, std::min<uint32_t>(material.material->shaderCount, TE_GFX_MAX_SHADERS));
        for (uint32_t pass = 0; pass < passes; ++pass) {
            ps2e_draw_state state;
            uint32_t bucket;
            bool lit = false, screenCopy = false, envMap = false;
            MaterialView m = material;
            Geometry::View sub = view;   // the submodel's view: the material's UV scroll (StepAnimations) is its payload
            std::array<float, 2> scroll{0.f, 0.f};
            if (inst.overrideState) {
                state = *inst.overrideState;
                bucket = inst.overrideBucket;
            } else {
                if (pass) {
                    m = te::gfx::ShaderView(material, pass);
                    if (!m.shader || !PassDrawable(m.shader->type, inst.eye.has_value())) continue;
                }
                state = te::gfx::StateOf(m);
                bucket = m.material ? m.bucket : BucketOpaque;
                // The lit shader families: 02 (lit tristrip), 04 (skinned lit), 0C/0F (env map seconds), 10 (crystal), 16
                lit = inst.lights && m.shader && (m.shader->type == 0x02 || m.shader->type == 0x10 || m.shader->type == 0x16 ||
                                                  (pass && EnvType(m.shader->type)));
                envMap = pass && m.shader && EnvType(m.shader->type) && inst.eye.has_value();
                if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) scroll = {m.shader->scroll[0], m.shader->scroll[1]};
                screenCopy = m.shader && ScreenCopyType(m.shader->type);
                if (screenCopy) {
                    // the frame as the texture (MakeScreenCopySettings: the copy, clamped, MODULATE, its alpha taken)
                    state.texture = PS2E_DRAW_TEXTURE_FRAME_COPY;
                    state.tfx = PS2E_DRAW_TFX_MODULATE;
                    state.tcc = 1;
                    state.wrap_u = state.wrap_v = PS2E_DRAW_WRAP_CLAMP;
                } else {
                    sub.scroll = scroll;
                }
            }
            // diagnosis: TE_GFX_DIAG_SKIP_BUCKET=6,8 leaves those buckets' model draws out (which layer of a composite is which)
            static const std::string skipBuckets = std::getenv("TE_GFX_DIAG_SKIP_BUCKET") ? std::string(",") + std::getenv("TE_GFX_DIAG_SKIP_BUCKET") + "," : std::string();
            if (!skipBuckets.empty() && skipBuckets.find("," + std::to_string(bucket) + ",") != std::string::npos) continue;
            const std::array<uint8_t, 4>* colours = nullptr;
            if (lit) {
                LitColours(part, *inst.lights, g_colourScratch);
                colours = g_colourScratch.data();
            }
            if (screenCopy) {
                if (!PrepareScreenCopyMesh(part, view.clip, m.shader->extra[0])) continue;
                emitter.BeginScreenCopy(bucket, state, scroll);
            } else {
                if (envMap && !PrepareEnvMesh(part, view.clip, *inst.eye)) continue;
                emitter.Begin(bucket, state);
            }
            emitter.flatTint = inst.overrideState ? 2 : view.clipped ? 1 : 0;
            DiagBox box;
            const bool logging = g_diag.Logging();
            emitter.box = logging ? &box : nullptr;
            if (!screenCopy && !envMap && Deferrable(logging)) {
                // the plain build on a worker: the placeholder takes this draw's place in the bucket
                te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, emitter.state);
                std::vector<std::array<uint8_t, 4>> lit;
                if (colours) lit.assign(g_colourScratch.begin(), g_colourScratch.end());
                te::gfx::PostBuild(build, [&build, prepared = &part.prepared, sub, lit = std::move(lit), state = BuildState(),
                                           textured = emitter.textured, model = rigid->model, i, pass] {
                    Collector collector{&build, textured};
                    const Geometry::Result r = te::gfx::MeshBuild(*prepared, sub, state, lit.empty() ? nullptr : lit.data(), &Collector::Sink, &collector);
                    if (r != Geometry::Result::Ok) {
                        static std::atomic<int> said{0};
                        if (said++ < 8) std::fprintf(stderr, "[te.gfx] model %#x part %u pass %u: Build refused (%d)\n", model, i, pass, int(r));
                    }
                    g_deferredModelTriangles += collector.triangles;
                });
                emitter.box = nullptr;
                if (pass) scene.material_passes++;
                continue;
            }
            Geometry::Result result;
            size_t triangles;
            if (screenCopy || envMap) {
                const size_t before = emitter.triangles;
                result = te::gfx::MeshBuild(g_screenCopyScratch.prepared, sub, BuildState(), colours, &Emitter::Sink, &emitter);
                triangles = emitter.triangles - before;
            } else {
                triangles = emitter.Build(part, sub, colours, &result);
            }
            emitter.box = nullptr;
            if (result != Geometry::Result::Ok) {
                static int said = 0;
                if (said++ < 8) std::fprintf(stderr, "[te.gfx] model %#x part %u pass %u: Build refused (%d)\n", rigid->model, i, pass, int(result));
                continue;
            }
            if (logging) {
                std::fprintf(stderr,
                             "[te.gfx.diag] present %ld %s model %#x part %u/%u pass %u %s bucket %u shader %#x tex %u tfx %d tcc %d blend %d(%u %u %u %u fix %u) atest %d(m%d ref %u fail %d) ztest %d zwrite %d tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) n %zu scroll (%g %g)%s\n",
                             g_diag.present, inst.overrideState ? "shadow" : (&counter == &scene.rigid_models ? "rigid" : "placed"), rigid->model, i, count, pass,
                             view.clipped ? "clipped" : "UNCLIPPED", bucket, m.shader ? unsigned(m.shader->type) : 0u, state.texture, state.tfx, state.tcc, state.blend,
                             state.blend_a, state.blend_b, state.blend_c, state.blend_d, state.blend_fix, state.alpha_test, state.alpha_method, state.alpha_reference,
                             state.alpha_fail, state.depth_test, state.depth_write, triangles, box.x0, box.y0, box.x1, box.y1,
                             box.n ? box.r / box.n : 0.0, box.n ? box.g / box.n : 0.0, box.n ? box.b / box.n : 0.0, box.n ? box.a / box.n : 0.0, box.n,
                             scroll[0], scroll[1], screenCopy ? " SCREEN-COPY" : envMap ? " ENV-MAP" : "");
                if (pass == 0) {
                    // the submodel's streams and the material's shaders (the first is the draw's state)
                    const bool normals = !part.normals.empty() && !part.normals[0].empty(), colours8 = !part.colours.empty() && !part.colours[0].empty();
                    std::string shaders;
                    if (m.material) {
                        for (uint32_t k = 0; k < m.material->shaderCount && k < TE_GFX_MAX_SHADERS; ++k) {
                            const auto* sh = Guest<te_gfx_shader>(m.material->shaders[k]);
                            char buf[160];
                            if (sh) std::snprintf(buf, sizeof buf, " [type %#x settings %#llx tex %u @%#x colour %u %u %u %u]", unsigned(sh->type), (unsigned long long)sh->settings, sh->textureId, sh->texture, sh->colour[0], sh->colour[1], sh->colour[2], sh->colour[3]);
                            else std::snprintf(buf, sizeof buf, " [null]");
                            shaders += buf;
                        }
                    }
                    // the colour histogram of the whole submodel (authored colours: are they dark everywhere or a gradient?)
                    size_t dark = 0, mid = 0, bright = 0, total = 0;
                    for (const auto& strip : part.mesh.strips)
                        for (const auto& v : strip.vertices) {
                            const unsigned lum = (unsigned(v.colour[0]) + unsigned(v.colour[1]) + unsigned(v.colour[2])) / 3;
                            (lum < 16 ? dark : lum < 64 ? mid : bright)++;
                            total++;
                        }
                    std::fprintf(stderr, "[te.gfx.diag]   colours of %zu vertices: dark(<16) %zu mid(<64) %zu bright %zu\n", total, dark, mid, bright);
                    const auto& v0 = part.mesh.strips[0].vertices;
                    std::fprintf(stderr, "[te.gfx.diag]   streams: normals %d colours8 %d, strips %zu, strip0 v0 colour %u %u %u %u stq %g %g %g; material kind %u bucket %u shaders %u:%s\n",
                                 int(normals), int(colours8), part.mesh.strips.size(), v0[0].colour[0], v0[0].colour[1], v0[0].colour[2], v0[0].colour[3], v0[0].stq[0], v0[0].stq[1], v0[0].stq[2],
                                 m.material ? m.material->kind : 0u, m.material ? m.material->bucket : 0u, m.material ? m.material->shaderCount : 0u, shaders.c_str());
                    // the submodel's model-space box and, for TE_GFX_DIAG_MESH=<model>, every strip's vertices (position, STQ)
                    static const char* diagMesh = std::getenv("TE_GFX_DIAG_MESH");
                    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                    for (const auto& strip : part.mesh.strips)
                        for (const auto& v : strip.vertices)
                            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.position[k]); hi[k] = std::max(hi[k], v.position[k]); }
                    std::fprintf(stderr, "[te.gfx.diag]   model box (%g %g %g)-(%g %g %g)\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                    if (diagMesh && std::strtoul(diagMesh, nullptr, 0) == rigid->model) {
                        for (size_t s = 0; s < part.mesh.strips.size(); ++s) {
                            const auto& strip = part.mesh.strips[s];
                            for (size_t k = 0; k < strip.vertices.size(); ++k) {
                                const auto& v = strip.vertices[k];
                                std::fprintf(stderr, "[te.gfx.mesh] part %u strip %zu v %zu%s pos (%g %g %g) stq (%g %g %g) rgba %u %u %u %u\n", i, s, k, v.restart ? " RESTART" : "",
                                             v.position[0], v.position[1], v.position[2], v.stq[0], v.stq[1], v.stq[2], v.colour[0], v.colour[1], v.colour[2], v.colour[3]);
                            }
                        }
                    }
                }
            }
            scene.model_triangles += triangles;
            if (pass) scene.material_passes++;
        }
    }
    counter++;
}

// ---- culling state (the VU0 registers' roles) -------------------------------------------------------------------------------
struct Culling {
    std::optional<title::CullingMath::View> view;
    std::optional<title::CullingMath::Link> link;
    std::optional<title::CullingMath::Frustum> planes;
    std::optional<title::CullingMath::Model> model;      // the last CullLoadModel: distance and matrix to the screen (vf15, vf11-14)
    std::optional<title::CullingMath::Classification> cell;   // the last CullTestBox
    std::optional<title::CullingMath::BoxView> box;      // the last CullTestBoxAt
    Matrix toScreen = Identity(), toClip = Identity();   // vf11-14 / vf7-10 as the programs leave them
} g_cull;

std::optional<title::CullingPlanes::Planes> ReadPlanes(uint32_t address) {
    const float* p = reinterpret_cast<const float*>(te::GuestPointer(address, 6 * 16));
    if (!p) return std::nullopt;
    title::CullingPlanes::Planes planes;
    for (int i = 0; i < 6; ++i) planes[i] = {p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]};
    return planes;
}

}  // namespace

extern "C" {

// ---- the models' geometry (wasm side ReadModel -> the packet bytes) -------------------------------------------------------
void ps2e_gfx2_model_loaded(ps2e_u32 model, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    const uint8_t* bytes = size > 0 ? te::GuestPointer(packet, size_t(size)) : nullptr;
    LoadModelPart(model, index, count, bytes, size, vertices);
}

void ps2e_gfx2_model_released(ps2e_u32 model) {
    te::gfx::DrainBuilds();   // a posted build may still read its parts
    g_models.erase(model);
    g_shadowModels.erase(model);
}

}  // extern "C"

namespace {
void LoadModelPart(uint32_t model, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    auto& scene = te::gfx::Scene();
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;
    te::gfx::DrainBuilds();   // a posted build may still read the parts this replaces
    Model& m = g_models[model];
    if (m.parts.size() != size_t(count)) { m.parts.assign(size_t(count), Part{}); m.sources.assign(size_t(count), PartSource{}); }
    PartSource& source = m.sources[size_t(index)];
    source.vertices = vertices;
    if (bytes && size > 0 && source.packet.data() != bytes) source.packet.assign(bytes, bytes + size);
    Part& part = m.parts[size_t(index)];
    part = Part{};
    std::string why;
    if (!bytes || !DecodePacket(bytes, size_t(size), uint32_t(vertices), part, &why)) {
        scene.models_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: packet not decoded (%s; %d bytes, %d vertices)\n", model, index, count,
                         why.c_str(), size, vertices);
        return;
    }
    // prepared once here; every instance's build only transforms, clips and projects it
    const Geometry::Result prepared = te::gfx::MeshPrepare(part.mesh, part.prepared);
    if (prepared != Geometry::Result::Ok) {
        part.decoded = false;
        scene.models_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: geometry refused by the engine (%d; %zu strips, %u vertices)\n", model, index,
                         count, int(prepared), part.mesh.strips.size(), part.vertices);
        return;
    }
    scene.models_decoded++;
    if (te::gfx::LogEnabled() && scene.models_decoded <= 64)
        std::fprintf(stderr, "[te.gfx] model %#x submodel %d/%d: %zu strips, %u vertices (declared %d), %zu triangles, normals %s, colours %s\n",
                     model, index, count, part.mesh.strips.size(), part.vertices, vertices, part.triangles,
                     !part.normals.empty() && !part.normals[0].empty() ? "yes" : "no",
                     !part.colours.empty() && !part.colours[0].empty() ? "yes" : "no");
}
}  // namespace

// ---- save state (graphics.cpp's kTagGraphics section carries these): the decoded models are rebuilt from their kept
// packet bytes
namespace te::gfx {
void ResetDrawOrdinals() { g_drawOrdinals.clear(); }
// The frame's camera for the interpolator: the game's current RenderView -- world->camera, camera->screen (the GS pixel
// space, the viewport included: what toScreen = toCamera . cameraToScreen builds every rigid draw from) and its camera object
bool CurrentCamera(ps2r::ps2e::InterpCamera& out, uint32_t* camera_object) {
    const RenderView* view = CurrentView();
    if (!view) return false;
    const Matrix cam = ToMatrix(view->toCamera), c2s = ToMatrix(view->cameraToScreen);
    if (!Finite(cam) || !Finite(c2s)) return false;
    Flat(cam, out.to_camera);
    Flat(c2s, out.camera_to_screen);
    out.valid = true;
    if (camera_object) *camera_object = view->cameraObject;
    return true;
}
std::vector<ModelSource> ModelSources() {
    std::vector<ModelSource> out;
    for (const auto& [address, m] : g_models)
        for (size_t i = 0; i < m.sources.size(); ++i)
            if (!m.sources[i].packet.empty())
                out.push_back(ModelSource{address, int32_t(i), int32_t(m.sources.size()), m.sources[i].vertices, m.sources[i].packet});
    return out;
}
void ReplayModelSources(const std::vector<ModelSource>& sources) {
    DrainBuilds();
    g_models.clear();
    g_shadowModels.clear();
    auto& scene = Scene();
    const uint64_t decoded = scene.models_decoded, refused = scene.models_refused;
    for (const ModelSource& s : sources)
        LoadModelPart(s.model, s.index, s.count, s.packet.data(), int32_t(s.packet.size()), s.vertices);
    // the counters are the run's census, not the restore's
    scene.models_decoded = decoded;
    scene.models_refused = refused;
}
}  // namespace te::gfx

extern "C" {

// ---- the scene's models ------------------------------------------------------------------------------------------------------
// DrawRigidModel (models.cpp SetRigidModelRenderDMA): the instance's matrix to the screen = matrix * view.toScreen, to the clip
// space = matrix * view.toClip, the lights' directions turned into the model's space (the rotation's transpose), drawn as the
// view test found it (mode 1 wholly in view: no clipping; 2: clipped)
void ps2e_graphics_draw_rigid_model(ps2e_u32 model, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    const auto* rigid = Guest<te_gfx_rigid_model>(model);
    const auto world = ReadMatrix(matrix);
    const RenderView* view = CurrentView();
    if (!rigid || !world || !view || !te::gfx::EnsureDevice()) return;
    Geometry::View v;
    v.screen = Multiply(*world, ToMatrix(view->toScreen));
    v.clip = Multiply(*world, ToMatrix(view->toClip));
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = uint32_t(mode) != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    Lights l{};
    bool haveLights = false;
    if (const auto* ml = Guest<ModelLights>(lights)) {
        const float* dirs = reinterpret_cast<const float*>(te::GuestPointer(ml->directions, 48));
        const float* cols = reinterpret_cast<const float*>(te::GuestPointer(ml->colours, 48));
        const float* amb = reinterpret_cast<const float*>(te::GuestPointer(ml->ambient, 16));
        if (dirs && cols && amb) {
            haveLights = true;
            for (int i = 0; i < 3; ++i) {
                // RotateVector(transpose(matrix), direction): l_k = sum_r matrix[k][r] * d[r]
                for (int k = 0; k < 3; ++k) {
                    float s = 0.f;
                    for (int r = 0; r < 3; ++r) s += (*world)[k][r] * dirs[i * 4 + r];
                    l.directions[i][k] = s;
                }
                l.directions[i][3] = 0.f;
                l.colours[i] = {cols[i * 4], cols[i * 4 + 1], cols[i * 4 + 2], cols[i * 4 + 3]};
            }
            l.ambient = {amb[0], amb[1], amb[2], amb[3]};
        }
    }
    InstanceState inst;
    inst.lights = haveLights ? &l : nullptr;
    if (rigid->needsEye) inst.eye = EyeIn(*world, view->toCamera);   // the env-map passes
    // the interpolator's identity: this model at this instance's matrix (a stable address while the instance lives)
    if (const auto w = WorldOfScreen(v.screen)) TagRigid(OrdinalId(TagId(model, 0)), *w, v.screen);   // (the matrix is a stack local: the order names the instance)
    if (g_diag.Logging()) {
        const auto* t = te::gfx::CurrentTarget();
        std::fprintf(stderr, "[te.gfx.diag] present %ld rigid view: toScreen row3 (%.1f %.1f %.1f %.1f) proj row0 (%.3f %.3f %.3f %.3f) clip (%.2f %.2f %.2f %.2f) world row3 (%.2f %.2f %.2f) target %s off (%d %d) size (%d %d) display (%d %d) xy (%d %d) c2s row3 (%.1f %.1f %.1f %.1f)\n",
                     g_diag.present, view->toScreen[3][0], view->toScreen[3][1], view->toScreen[3][2], view->toScreen[3][3],
                     view->projection[0][0], view->projection[0][1], view->projection[0][2], view->projection[0][3],
                     view->clip[0], view->clip[1], view->clip[2], view->clip[3], (*world)[3][0], (*world)[3][1], (*world)[3][2],
                     t ? "yes" : "none", t ? t->offsetX : 0, t ? t->offsetY : 0, t ? t->width : 0, t ? t->height : 0, t ? t->displayWidth : 0, t ? t->displayHeight : 0,
                     t ? t->x : 0, t ? t->y : 0, t ? t->clipToScreen[3][0] : 0.f, t ? t->clipToScreen[3][1] : 0.f, t ? t->clipToScreen[3][2] : 0.f, t ? t->clipToScreen[3][3] : 0.f);
    }
    DrawInstance(rigid, v, inst, te::gfx::Scene().rigid_models);
    te::gfx::ClearDrawTag();
}

// DrawPlacedModel (models.cpp SetPlacedModelDMA): the chunk view's matrices of its last culling test (to the screen, to the clip
// space), its clip vector and how the test found it. Billboards (shader type 1B) are drawn as placed for now.
void ps2e_graphics_draw_placed_model(ps2e_u32 model, ps2e_u32 viewAddress, ps2e_u32 world) {
    const auto* rigid = Guest<te_gfx_rigid_model>(model);
    const auto* cv = Guest<ChunkView>(viewAddress);
    if (!rigid || !cv || !te::gfx::EnsureDevice()) return;
    Geometry::View v;
    v.screen = ToMatrix(cv->toScreen);
    v.clip = ToMatrix(cv->clipped);
    v.guard = {cv->clip[0], cv->clip[1], cv->clip[2], cv->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = cv->visibility != DrawUnclipped;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    if (rigid->billboard) Once("DrawPlacedModel: a billboard model drawn without its turn to the camera");
    if (g_diag.Logging()) {
        const auto w = ReadMatrix(world);
        if (w) std::fprintf(stderr, "[te.gfx.diag] present %ld placed view: model %#x world row0 (%.4f %.4f %.4f) row1 (%.4f %.4f %.4f) row2 (%.4f %.4f %.4f) row3 (%.4f %.4f %.4f) visibility %u clip (%.2f %.2f %.2f %.2f)\n",
                               g_diag.present, model, (*w)[0][0], (*w)[0][1], (*w)[0][2], (*w)[1][0], (*w)[1][1], (*w)[1][2], (*w)[2][0], (*w)[2][1], (*w)[2][2],
                               (*w)[3][0], (*w)[3][1], (*w)[3][2], unsigned(cv->visibility), cv->clip[0], cv->clip[1], cv->clip[2], cv->clip[3]);
    }
    InstanceState inst;
    if (rigid->needsEye) {   // the env-map passes (models.cpp: the camera through the placed instance's inverse)
        const auto w = ReadMatrix(world);
        const RenderView* view = CurrentView();
        if (w && view) inst.eye = EyeIn(*w, view->toCamera);
    }
    // the interpolator's identity: this model at this placement (the placed instance's own matrix address: stable while the
    // chunk lives, one per placement); the instance's world matrix is the one the chunk view's screen matrix stands for under
    // the current camera (the view carries the chunk's matrix; the game's `world` is chunk-local)
    // (the matrices live in the frame arena, recycled per frame: the same offsets recur across chunks, so the chunk view is
    // part of the identity; within a chunk the array's order is the placement's)
    if (const auto w = WorldOfScreen(v.screen)) TagRigid(OrdinalId(TagId(model, viewAddress)), *w, v.screen);   // (the world matrix is a frame-arena slot: the order within the chunk view names the placement)
    DrawInstance(rigid, v, inst, te::gfx::Scene().placed_models);
    te::gfx::ClearDrawTag();
}

// ---- the sky (sky.cpp DrawSky): the sky's models around the camera, first in the frame, the depth left at the far plane ----
void ps2e_gfx2_draw_sky(ps2e_u32 skyAddress, ps2e_u32 viewAddress, ps2e_u32 cameraAddress) {
    const auto* sky = Guest<te_gfx_sky>(skyAddress);
    const auto* view = Guest<RenderView>(viewAddress);
    const auto camera = ReadVector(cameraAddress);
    const auto* target = te::gfx::CurrentTarget();
    if (!sky || !view || !camera || !te::gfx::EnsureDevice()) return;
    if (sky->count == 0 || sky->count > 64) return;
    const uint32_t* models = reinterpret_cast<const uint32_t*>(te::GuestPointer(sky->models, 4 * size_t(sky->count)));
    if (!models) return;
    // The PS2 drew into a half-size buffer with the view's matrix to it; here the frame's own clip-to-screen matrix
    Matrix clipToScreen = target ? ToMatrix(target->clipToScreen) : Identity();
    if (!target) {
        const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
        clipToScreen = Matrix{{{w * 8.f, 0, 0, 0}, {0, -h * 8.f, 0, 0}, {0, 0, -8388607.f, 0}, {2048.f * 16.f, 2048.f * 16.f, 8388607.f, 1.f}}};
    }
    const Matrix translation = Translation(*camera);
    const Matrix toClip = Multiply(translation, ToMatrix(view->toClip));
    const Matrix toScreen = Multiply(toClip, clipToScreen);
    Geometry::View v;
    v.screen = toScreen;
    v.clip = toClip;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    auto& scene = te::gfx::Scene();
    for (uint32_t i = 0; i < sky->count; ++i) {
        const auto* rigid = Guest<te_gfx_rigid_model>(models[i]);
        if (!rigid) continue;
        // the interpolator's identity: the sky's i-th model, its world the camera's place (it turns with the camera, no parallax)
        if (const auto w = WorldOfScreen(toScreen)) TagRigid(TagId(skyAddress, models[i]), *w, toScreen);
        // Drawn in the sky bucket under the materials' own blend state, the depth test ALWAYS and the depth not written
        const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
        auto it = g_models.find(rigid->model);
        if (!data || it == g_models.end()) { scene.submodels_missing++; continue; }
        const uint32_t count = std::min<uint32_t>(rigid->count, data->subModelCount);
        const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(rigid->materials, 4 * size_t(count))) : nullptr;
        static Emitter emitter;
        for (uint32_t p = 0; p < count && p < it->second.parts.size(); ++p) {
            const Part& part = it->second.parts[p];
            if (!part.decoded) continue;
            const uint32_t resource = materials ? materials[p] : 0;
            const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;
            const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
            ps2e_draw_state state = te::gfx::StateOf(m);
            state.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
            state.depth_write = 0;
            emitter.Begin(BucketSky, state);
            Geometry::View sub = v;
            if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) sub.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
            Geometry::Result result;
            const size_t triangles = emitter.Build(part, sub, nullptr, &result);
            if (result != Geometry::Result::Ok) continue;
            scene.model_triangles += triangles;
        }
        scene.sky_models++;
    }
    te::gfx::ClearDrawTag();
    // FinishSky drew the half-size buffer over the screen with the depth written at the far plane: the depth cleared here
    ps2e_draw_state clear{};
    clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    clear.depth_write = 1;
    clear.no_colour_write = 1;
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f; o.a = 0x80;
        return o;
    };
    Draw d;
    d.state = clear;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketSky, std::move(d));
}

}  // extern "C"

namespace {
// ---- the shadows (shadows.cpp StartShadows / DrawShadowMesh / ApplyShadows) -----------------------------------------------------
// The PS2 counted the shadow volumes in a half-size accumulator: the byte at 120, every type-0B volume strip adding its
// winding colour (+8 / 248 = -8) where it passed the depth test against the scene, then the frame darkened by a quarter of
// 0x84 (33 levels) where the count was 1..16 (the engine's ShadowDraw is the step list). Here the frame's
// stencil is that counter through ps2e_draw: StartShadows clears it, each strip or clipped fan counts up or down by its
// authored colour (colour and depth not written, depth GEQUAL against the scene), ApplyShadows draws the full-frame
// composite where the count is inside -- so the volume's walls above the ground and the caster himself are NOT darkened (the
// front and back faces cancel there), only the surfaces inside the volume.
constexpr uint8_t kWindingUp = 8, kWindingDown = 248;
constexpr uint8_t kShadowMask = 0x84;      // ShadowDraw::QuantizedByte: the composite's source where the count is inside
constexpr uint8_t kShadowFix = 0x20;       // ALPHA {2, 0, 2, 1, 0x20}: Cd - Cs * 0x20 / 0x80 = Cd - 33

void SetStencil(ps2e_draw_state& st, int value) {
    st.stencil = value;
}

// The volume strips: each published strip / fan counts in the direction its authored colour says
struct ShadowEmitter {
    ps2e_draw_state up{}, down{};
    size_t triangles = 0, odd = 0;
    DiagBox* box = nullptr;
    void Begin() {
        up = ps2e_draw_state{};
        up.gouraud = 0;
        up.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
        up.depth_write = 0;
        up.no_colour_write = 1;
        up.tcc = 1;
        // StartShadows' UndrawnAndNearer -- DATE with DATM 0: a strip counts only where the frame's alpha top bit
        // is 0 (the receivers: materials without FBA); the casters (Crash's skin, FBA on) are left alone. Without it the
        // caster's own body was darkened wherever his volume's front face lay in front of his skin (the user's blotches).
        // TE_GFX_SHADOW_NO_DATE=1 keeps the old counting for A/B.
        static const bool noDate = std::getenv("TE_GFX_SHADOW_NO_DATE") != nullptr;
        up.dest_alpha_test = noDate ? 0 : 1;
        down = up;
        SetStencil(up, PS2E_DRAW_STENCIL_COUNT_UP);
        SetStencil(down, PS2E_DRAW_STENCIL_COUNT_DOWN);
    }
    void Append(const ps2r::gs::VertexRec* verts, size_t count) {
        count = count / 3 * 3;
        if (!count) return;
        const uint8_t colour = verts[0].r;
        if (colour != kWindingUp && colour != kWindingDown) odd++;
        const ps2e_draw_state& state = colour == kWindingDown ? down : up;
        Draw& d = te::gfx::OpenDraw(BucketShadows, state);
        const size_t base = d.verts.size();
        d.verts.resize(base + count);
        for (size_t i = 0; i < count; ++i) {
            const auto& v = verts[i];
            ps2e_draw_vertex& o = d.verts[base + i];
            o = ps2e_draw_vertex{};
            o.x = v.x;
            o.y = v.y;
            // ShadowGeometry emits the type-0B program's NEGATED depth (2^32 - z, for the GS's inverted depth copy); the
            // frame's depth is the ordinary z, the count taken where the ordinary test fails
            o.z = v.z > 0.f ? 4294967296.f - v.z : 0.f;
            o.r = o.g = o.b = colour;
            o.a = 0x80;
            if (box) box->Add(o);
        }
        te::gfx::CloseDraw(BucketShadows);
        triangles += count / 3;
    }
    static void Sink(void* user, const ps2r::gs::VertexRec* verts, size_t count, bool fan) {
        (void)fan;
        static_cast<ShadowEmitter*>(user)->Append(verts, count);
    }
};
ShadowEmitter g_shadowEmitter;
bool g_shadowsOpen = false;
// The cost of a body on the game thread, added to a counter on scope exit
struct Timed {
    double& sink;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Timed(double& s) : sink(s) {}
    ~Timed() { sink += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};
ShadowModel& ShadowModelOf(uint32_t model, const Model& m) {
    ShadowModel& sm = g_shadowModels[model];
    if (sm.tried) return sm;
    sm.tried = true;
    sm.parts.resize(m.sources.size());
    sm.decoded.assign(m.sources.size(), false);
    for (size_t i = 0; i < m.sources.size(); ++i) {
        const PartSource& src = m.sources[i];
        if (src.packet.empty()) continue;
        const auto r = title::ShadowGeometry::Decode(src.packet.data(), src.packet.size(), uint32_t(src.vertices), sm.parts[i]);
        sm.decoded[i] = r == title::ShadowGeometry::Result::Ok;
        if (!sm.decoded[i]) {
            static int said = 0;
            if (said++ < 8) std::fprintf(stderr, "[te.gfx] shadow model %#x part %zu: ShadowGeometry refused the packet (%d)\n", model, i, int(r));
        }
    }
    return sm;
}
size_t g_shadowVolumes = 0;   // strips counted since StartShadows (no composite for an empty bracket)

// A full-frame quad in the shadows' bucket under `state`
void ShadowQuad(const ps2e_draw_state& state, uint8_t colour, uint8_t alpha) {
    const float w = float(te::gfx::ScreenW()), h = float(te::gfx::ScreenH());
    auto V = [&](float x, float y) {
        ps2e_draw_vertex o{};
        o.x = x; o.y = y; o.z = 0.f;
        o.r = o.g = o.b = colour; o.a = alpha;
        return o;
    };
    Draw d;
    d.state = state;
    d.verts = {V(0, 0), V(w, 0), V(0, h), V(w, 0), V(w, h), V(0, h)};
    te::gfx::Record(BucketShadows, std::move(d));
}
}  // namespace

extern "C" {

void ps2e_graphics_set_up_shadow_pass(void) { Once("SetUpShadowPass: the shadows' palette slot has no meaning here (the stencil counts the volumes)"); }

void ps2e_graphics_begin_shadows(void) {
    const RenderView* view = CurrentView();
    const auto* target = te::gfx::CurrentTarget();
    if (!view) return;
    const Matrix toCamera = ToMatrix(view->toClip);
    const Matrix toScreen = Multiply(toCamera, target ? ToMatrix(target->clipToScreen) : Identity());
    WriteMatrix(kShadowToCamera, toCamera);
    WriteMatrix(kShadowToScreen, toScreen);
    g_shadowsOpen = true;
    g_shadowVolumes = 0;
    g_shadowEmitter.Begin();
    if (te::gfx::EnsureDevice()) {
        // StartShadows: the counter's origin over the whole frame (a submit that draws nothing)
        ps2e_draw_state clear{};
        clear.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        clear.depth_write = 0;
        clear.no_colour_write = 1;
        SetStencil(clear, PS2E_DRAW_STENCIL_CLEAR);
        ShadowQuad(clear, 0, 0);
    }
}

void ps2e_graphics_end_shadows(void) {
    if (!g_shadowsOpen) return;
    g_shadowsOpen = false;
    if (!g_shadowVolumes || !te::gfx::EnsureDevice()) return;
    // TE_GFX_NO_SHADOWS=1: the volumes are counted but never composited (the shadow_caster_judge's reference frame)
    static const bool off = std::getenv("TE_GFX_NO_SHADOWS") != nullptr;
    if (off) return;
    // ApplyShadows: Cd - 0x84 * 0x20 / 0x80 where the count is 1..16; the depth untouched
    ps2e_draw_state composite{};
    composite.gouraud = 0;
    composite.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    composite.depth_write = 0;
    composite.blend = 1;
    composite.blend_a = 2; composite.blend_b = 0; composite.blend_c = 2; composite.blend_d = 1;
    composite.blend_fix = kShadowFix;
    composite.tcc = 1;
    SetStencil(composite, PS2E_DRAW_STENCIL_INSIDE);
    // diagnosis: TE_GFX_SHADOW_DIAG=1 composites without the stencil test (the whole frame darkens: the blend path alone)
    static const bool diag = std::getenv("TE_GFX_SHADOW_DIAG") != nullptr;
    if (diag) SetStencil(composite, PS2E_DRAW_STENCIL_OFF);
    ShadowQuad(composite, kShadowMask, 0x80);
    te::gfx::Scene().shadow_passes++;
}

void ps2e_graphics_draw_shadow_mesh(ps2e_u32 mesh, ps2e_u32 toScreen, ps2e_u32 toCamera, ps2e_u32 world) {
    Timed timed(te::gfx::Scene().shadow_ms);
    const auto* rigid = Guest<te_gfx_rigid_model>(mesh);
    const auto screen = ReadMatrix(toScreen);
    const auto camera = ReadMatrix(toCamera);
    const RenderView* view = CurrentView();
    if (!rigid || !screen || !camera || !view || !te::gfx::EnsureDevice()) return;
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
    Geometry::View v;
    v.screen = *screen;
    v.clip = *camera;
    v.guard = {view->clip[0], view->clip[1], view->clip[2], view->clip[3]};
    v.scroll = {0.f, 0.f};
    v.clipped = true;
    if (!Finite(v.screen) || !Finite(v.clip)) return;
    auto& scene = te::gfx::Scene();
    const te_gfx_model* data = rigid->model ? Guest<te_gfx_model>(rigid->model) : nullptr;
    auto it = g_models.find(rigid->model);
    if (!data || it == g_models.end()) { scene.submodels_missing++; return; }
    if (!g_shadowsOpen) g_shadowEmitter.Begin();   // a mesh outside a bracket: counted into the frame's last clear
    const uint32_t count = std::min<uint32_t>(rigid->count ? rigid->count : data->subModelCount, data->subModelCount);
    const bool logging = g_diag.Logging();
    DiagBox box;
    g_shadowEmitter.box = logging ? &box : nullptr;
    const size_t before = g_shadowEmitter.triangles, oddBefore = g_shadowEmitter.odd;
    ShadowModel& sm = ShadowModelOf(rigid->model, it->second);
    title::ShadowGeometry::View sv;
    sv.screen = v.screen;
    sv.clip = v.clip;
    sv.guard = v.guard;
    sv.offset = {te::gfx::GsOriginX(), te::gfx::GsOriginY()};
    // the interpolator's identity: this volume mesh at this instance's world matrix (a rigid object, re-projected with it)
    if (const auto w = WorldOfScreen(*screen)) TagRigid(OrdinalId(TagId(mesh, 0)), *w, *screen);
    static std::vector<ps2r::gs::DrawIr> volumes;
    for (uint32_t p = 0; p < count && p < sm.parts.size(); ++p) {
        if (!sm.decoded[p]) continue;
        volumes.clear();
        const auto r = title::ShadowGeometry::Build(sm.parts[p], {sv}, volumes);
        if (r != title::ShadowGeometry::Result::Ok) continue;
        for (const auto& d : volumes) g_shadowEmitter.Append(d.verts.data(), d.verts.size());
    }
    te::gfx::ClearDrawTag();
    g_shadowEmitter.box = nullptr;
    const size_t triangles = g_shadowEmitter.triangles - before;
    if (g_shadowEmitter.odd != oddBefore) {
        scene.shadow_odd_colours += g_shadowEmitter.odd - oddBefore;
        static int said = 0;
        if (said++ < 4) std::fprintf(stderr, "[te.gfx] shadow mesh %#x: %zu strips without a +8/-8 winding colour\n", mesh, g_shadowEmitter.odd - oddBefore);
    }
    if (logging)
        std::fprintf(stderr, "[te.gfx.diag] present %ld shadow model %#x parts %u tris %zu box (%.0f %.0f)-(%.0f %.0f)\n", g_diag.present,
                     rigid->model, count, triangles, box.x0, box.y0, box.x1, box.y1);
    scene.model_triangles += triangles;
    if (triangles) g_shadowVolumes++;
    scene.shadow_meshes++;
}

// The chunk's planes from the camera's place in it: the view's planes and the far set (the retail arrays SetViewFrustum fills)
void ps2e_graphics_load_chunk_planes(ps2e_u32 place) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(kViewPlanes), far = ReadPlanes(kFarViewPlanes);
    if (!matrix || !view || !far) return;
    g_cull.planes = title::CullingPlanes::InChunk(*view, *far, *matrix);
    static int said = 0;
    if (te::gfx::LogEnabled() && said++ < 3)
        std::fprintf(stderr, "[te.gfx] LoadChunkPlanes: near (%g %g %g %g) right (%g %g %g %g) far (%g %g %g %g); far-set right (%g %g %g %g)\n",
                     (*view)[0][0], (*view)[0][1], (*view)[0][2], (*view)[0][3], (*view)[1][0], (*view)[1][1], (*view)[1][2], (*view)[1][3],
                     (*view)[5][0], (*view)[5][1], (*view)[5][2], (*view)[5][3], (*far)[1][0], (*far)[1][1], (*far)[1][2], (*far)[1][3]);
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_load_portal_planes(ps2e_u32 place, ps2e_u32 portal) {
    const auto matrix = ReadMatrix(place);
    const auto view = ReadPlanes(kViewPlanes), far = ReadPlanes(kFarViewPlanes), sides = ReadPlanes(portal);
    if (!matrix || !view || !far || !sides) return;
    g_cull.planes = title::CullingPlanes::ThroughPortal(*view, *far, *matrix, *sides);
    g_cull.cell.reset();
    g_cull.box.reset();
}
// The view's matrix, the renderer view's matrix to the screen and the camera's place; the link made the identity
void ps2e_graphics_load_culling_view(ps2e_u32 toClip, ps2e_u32 toScreen, ps2e_u32 camera) {
    const auto clip = ReadMatrix(toClip), screen = ReadMatrix(toScreen);
    const auto eye = ReadVector(camera);
    if (!clip || !screen || !eye) return;
    g_cull.view = title::CullingMath::View{*clip, *screen, *eye};
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, Identity(), Identity());
    g_cull.model.reset();
    g_cull.cell.reset();
    g_cull.box.reset();
}
void ps2e_graphics_cull_load_link(ps2e_u32 chunk, ps2e_u32 object) {
    const auto c = ReadMatrix(chunk), o = ReadMatrix(object);
    if (!c || !o || !g_cull.view) return;
    g_cull.link = title::CullingMath::LoadLink(*g_cull.view, *c, *o);
}
void ps2e_graphics_cull_load_model(ps2e_u32 model) {
    const auto m = ReadMatrix(model);
    if (!m || !g_cull.link) return;
    g_cull.model = title::CullingMath::LoadModel(*g_cull.link, *m);
    g_cull.toScreen = g_cull.model->to_screen;
    g_cull.box.reset();
    g_cull.cell.reset();
    te::gfx::Scene().culls++;
}
void ps2e_graphics_cull_test_box(ps2e_u32 corners) {
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(corners, 32));
    if (!c || !g_cull.planes) return;
    title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    g_cull.cell = title::CullingMath::TestBox(*g_cull.planes, box);
    g_cull.box.reset();
}
void ps2e_graphics_cull_test_box_at(ps2e_u32 boxAddress, ps2e_u32 model) {
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(boxAddress, 32));
    const auto m = ReadMatrix(model);
    if (!c || !m || !g_cull.planes || !g_cull.link) return;
    title::CullingMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    g_cull.box = title::CullingMath::TestBoxAt(*g_cull.planes, *g_cull.link, box, *m);
    g_cull.cell.reset();
    if (g_cull.box->to_screen) g_cull.toScreen = *g_cull.box->to_screen;
    if (g_cull.box->to_clip) g_cull.toClip = *g_cull.box->to_clip;
    te::gfx::Scene().culls++;
}
// CullOutcome {outside, clipped, distance}: the last test's masks (nonzero = out / partly out), the distance of the last
// model or box test (vf15 keeps it across a cell test)
void ps2e_graphics_cull_result(ps2e_u32 out) {
    uint32_t* o = reinterpret_cast<uint32_t*>(te::GuestPointer(out, 12));
    if (!o) return;
    static const bool allOut = std::getenv("TE_GFX_CULL_ALL_OUT") != nullptr;   // diagnosis: the empty answer (nothing in view)
    if (allOut) { o[0] = 1; o[1] = 0; o[2] = 0; return; }
    static const bool allIn = std::getenv("TE_GFX_CULL_ALL_IN") != nullptr;   // diagnosis: everything wholly in view
    if (allIn) { o[0] = 0; o[1] = 0; o[2] = g_cull.box ? uint32_t(g_cull.box->distance) : g_cull.model ? uint32_t(g_cull.model->distance) : 0; return; }
    static const bool boxesOut = std::getenv("TE_GFX_CULL_BOXES_OUT") != nullptr;   // diagnosis: CullTestBoxAt (the instances) out
    uint32_t outside = 0, clipped = 0;
    int32_t distance = g_cull.model ? g_cull.model->distance : 0;
    if (g_cull.box && boxesOut) { o[0] = 1; o[1] = 0; o[2] = uint32_t(g_cull.box->distance); return; }
    if (g_cull.box) {
        outside = g_cull.box->visibility.outside;
        clipped = g_cull.box->visibility.clipped;
        distance = g_cull.box->distance;
    } else if (g_cull.cell) {
        outside = g_cull.cell->outside;
        clipped = g_cull.cell->clipped;
    } else if (!g_cull.view || !g_cull.planes) {
        outside = 1;   // nothing loaded: nothing in view
    }
    o[0] = outside;
    o[1] = clipped;
    o[2] = uint32_t(distance);
}
void ps2e_graphics_cull_matrices(ps2e_u32 clipped, ps2e_u32 toScreen) {
    if (clipped) WriteMatrix(clipped, g_cull.toClip);
    if (toScreen) WriteMatrix(toScreen, g_cull.toScreen);
}

// ---- ClipBox / ClipBoxAt (culling.cpp macro mode; the engine's ClippingMath): the corners through the view's matrix (and a
// model's), scaled by the row after the matrix; flags[0] = every corner's bits together, flags[1] = their common bits -----------
void ps2e_graphics_clip_box(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::GuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard);
    f[0] = result.any;
    f[1] = result.common;
}
void ps2e_graphics_clip_box_at(ps2e_u32 view, ps2e_u32 boxAddress, ps2e_u32 flags, ps2e_u32 model) {
    const auto matrix = ReadMatrix(view);
    const auto guard = ReadVector(view + 0x40);
    const auto m = ReadMatrix(model);
    const float* c = reinterpret_cast<const float*>(te::GuestPointer(boxAddress, 32));
    uint32_t* f = reinterpret_cast<uint32_t*>(te::GuestPointer(flags, 8));
    if (!f) return;
    if (!matrix || !guard || !c || !m) { f[0] = 0x3F; f[1] = 0x3F; return; }
    title::BoundsMath::Box box{{c[0], c[1], c[2], c[3]}, {c[4], c[5], c[6], c[7]}};
    const auto result = title::ClippingMath::ClipBox(box, *matrix, *guard, &*m);
    f[0] = result.any;
    f[1] = result.common;
}

}  // extern "C"

// ---- skins and blend skins: the skinned characters through the engine's SkinGeometry -------------------------------
// skins.cpp DrawSkin / DrawBlendSkin: per submodel its material's state, the skin's matrix times the view's (to the screen;
// to the clip space when drawn clipped), the lights turned into the skin's space, the joints' matrices, and the submodel's
// precompiled VU1 packet -- the engine runs the skinned lit strip program over them as values (SkinGeometry::Build /
// BuildBlend) and the strips come back as triangle lists in frame pixels, recorded into the material's bucket like a rigid
// model's. A skin's packets cross from the wasm side once when read (ps2e_gfx2_skin_loaded: the disk manager may move the
// block) and are kept host-side (the save state carries them); a blend skin's parts' packets live on the game's heap for the
// skin's life and are decoded at the first draw by their address.
namespace {
using SkinGeo = title::SkinGeometry;
struct SkinPart {
    SkinGeo::Part part;
    bool decoded = false;
    int32_t vertices = 0;
    std::vector<uint8_t> packet;   // kept for the save state
};
struct SkinModel { std::vector<SkinPart> parts; };
std::unordered_map<uint32_t, SkinModel> g_skins;   // Skin guest address -> its submodels
struct BlendPartGeo { SkinGeo::Part part; bool decoded = false; bool tried = false; };
std::unordered_map<uint32_t, std::unordered_map<uint32_t, BlendPartGeo>> g_blendSkins;   // BlendSkin -> part packet address -> geometry

uint32_t Bits(float value) { uint32_t b; std::memcpy(&b, &value, 4); return b; }
SkinGeo::Qw RowQw(const Matrix& m, int r) { return {Bits(m[r][0]), Bits(m[r][1]), Bits(m[r][2]), Bits(m[r][3])}; }

// The per-draw inputs (SetSkinDMA): false when the view, the matrix or the joints cannot be read
bool SkinInputs(uint32_t jointsAddress, int32_t jointCount, uint32_t matrixAddress, uint32_t lightsAddress, int32_t mode,
                SkinGeo::Inputs& in) {
    const RenderView* view = CurrentView();
    const auto world = ReadMatrix(matrixAddress);
    if (!view || !world || jointCount <= 0 || jointCount > int32_t(SkinGeo::kMaxJoints)) return false;
    const auto* joints = reinterpret_cast<const uint32_t*>(te::GuestPointer(jointsAddress, 64 * size_t(jointCount)));
    if (!joints) return false;
    in.joints.resize(4 * size_t(jointCount));
    for (size_t q = 0; q < in.joints.size(); ++q)
        for (int k = 0; k < 4; ++k) in.joints[q][k] = joints[q * 4 + size_t(k)];
    const Matrix toScreen = Multiply(*world, ToMatrix(view->toScreen));
    if (!Finite(toScreen)) return false;
    for (int r = 0; r < 4; ++r) in.to_screen[r] = RowQw(toScreen, r);
    in.clipped = uint32_t(mode) != DrawUnclipped;
    if (in.clipped) {
        const Matrix toCamera = Multiply(*world, ToMatrix(view->toClip));
        if (!Finite(toCamera)) return false;
        for (int r = 0; r < 4; ++r) in.to_camera[r] = RowQw(toCamera, r);
        in.clip = {Bits(view->clip[0]), Bits(view->clip[1]), Bits(view->clip[2]), Bits(view->clip[3])};
    }
    // MakeSkinSpace: the lights' directions turned into the skin's space (the rotation's transpose), a row each; the
    // fourth words the PS2 left as stack leftovers are zero here (the program never reads them)
    bool lit = false;
    if (const auto* ml = Guest<ModelLights>(lightsAddress)) {
        const float* dirs = reinterpret_cast<const float*>(te::GuestPointer(ml->directions, 48));
        const float* cols = reinterpret_cast<const float*>(te::GuestPointer(ml->colours, 48));
        const float* amb = reinterpret_cast<const float*>(te::GuestPointer(ml->ambient, 16));
        if (dirs && cols && amb) {
            lit = true;
            for (int i = 0; i < 3; ++i) {
                float d[3];
                for (int k = 0; k < 3; ++k) {
                    float sum = 0.f;
                    for (int r = 0; r < 3; ++r) sum += (*world)[k][r] * dirs[i * 4 + r];
                    d[k] = sum;
                }
                in.lights[i] = {Bits(d[0]), Bits(d[1]), Bits(d[2]), 0};
                in.colours[i] = {Bits(cols[i * 4]), Bits(cols[i * 4 + 1]), Bits(cols[i * 4 + 2]), Bits(cols[i * 4 + 3])};
            }
            in.ambient = {Bits(amb[0]), Bits(amb[1]), Bits(amb[2]), Bits(amb[3])};
        }
    }
    if (!lit) {
        for (int i = 0; i < 3; ++i) { in.lights[i] = {0, 0, 0, 0}; in.colours[i] = {0, 0, 0, 0}; }
        in.ambient = {Bits(1.f), Bits(1.f), Bits(1.f), Bits(1.f)};
    }
    in.scroll = {0.f, 0.f};
    return true;
}

// The material of a skin's submodel (MaterialResource -> material) as the draw's state and bucket
void SkinMaterial(uint32_t materialsAddress, uint32_t count, uint32_t index, ps2e_draw_state& state, uint32_t& bucket, MaterialView* out) {
    const uint32_t* materials = count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(materialsAddress, 4 * size_t(count))) : nullptr;
    const uint32_t resource = materials ? materials[index] : 0;
    const uint32_t* materialAddress = resource ? Guest<uint32_t>(resource + 8) : nullptr;   // MaterialResource::material
    const MaterialView m = te::gfx::ReadMaterial(materialAddress ? *materialAddress : 0);
    state = te::gfx::StateOf(m);
    bucket = m.material ? m.bucket : BucketOpaque;
    if (out) *out = m;
}

// The built draws into the bucket (the Emitter's triangle-list form; a skin's strips are published one DrawIr per batch)
size_t EmitSkinDraws(Emitter& emitter, const std::vector<ps2r::gs::DrawIr>& draws) {
    const size_t before = emitter.triangles;
    for (const auto& d : draws) emitter.Append(d.verts.data(), d.verts.size());
    return emitter.triangles - before;
}

void SkinDiag(const char* kind, uint32_t skin, uint32_t index, uint32_t count, const SkinGeo::Inputs& in, uint32_t bucket,
              const MaterialView& m, const ps2e_draw_state& state, size_t triangles, const DiagBox& box, int result) {
    std::fprintf(stderr,
                 "[te.gfx.diag] present %ld %s %#x part %u/%u %s joints %zu bucket %u shader %#x tex %u tfx %d blend %d atest %d ztest %d zwrite %d result %d tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) n %zu\n",
                 g_diag.present, kind, skin, index, count, in.clipped ? "clipped" : "UNCLIPPED", in.joints.size() / 4, bucket,
                 m.shader ? unsigned(m.shader->type) : 0u, state.texture, state.tfx, state.blend, state.alpha_test, state.depth_test, state.depth_write,
                 result, triangles, box.x0, box.y0, box.x1, box.y1, box.n ? box.r / box.n : 0.0, box.n ? box.g / box.n : 0.0,
                 box.n ? box.b / box.n : 0.0, box.n ? box.a / box.n : 0.0, box.n);
}

void LoadSkinPart(uint32_t skin, int32_t index, int32_t count, const uint8_t* bytes, int32_t size, int32_t vertices) {
    auto& scene = te::gfx::Scene();
    if (index < 0 || count <= 0 || index >= count || count > 4096) return;
    te::gfx::DrainBuilds();   // a posted build may still read the parts this replaces
    SkinModel& m = g_skins[skin];
    if (m.parts.size() != size_t(count)) m.parts.assign(size_t(count), SkinPart{});
    SkinPart& part = m.parts[size_t(index)];
    part = SkinPart{};
    part.vertices = vertices;
    if (bytes && size > 0 && part.packet.data() != bytes) part.packet.assign(bytes, bytes + size);
    const SkinGeo::Result r = bytes ? SkinGeo::Decode(part.packet.data(), part.packet.size(), uint32_t(vertices), part.part)
                                    : SkinGeo::Result::InvalidAsset;
    if (r != SkinGeo::Result::Ok) {
        scene.skins_refused++;
        static int said = 0;
        if (said++ < 16)
            std::fprintf(stderr, "[te.gfx] skin %#x submodel %d/%d: packet not decoded (%d; %d bytes, %d vertices)\n", skin, index, count,
                         int(r), size, vertices);
        // diagnosis: TE_GFX_SKIN_DUMP=DIR writes every refused packet's bytes there (skin_<address>_<index>.bin)
        if (const char* dir = std::getenv("TE_GFX_SKIN_DUMP")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/skin_%08x_%d.bin", dir, skin, index);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(part.packet.data(), 1, part.packet.size(), f); std::fclose(f); }
        }
        return;
    }
    part.decoded = true;
    scene.skins_decoded++;
    if (te::gfx::LogEnabled() && scene.skins_decoded <= 64)
        std::fprintf(stderr, "[te.gfx] skin %#x submodel %d/%d: %zu batches, %u vertices (declared %d)\n", skin, index, count,
                     part.part.batches.size(), part.part.vertices, vertices);
}
}  // namespace

namespace te::gfx {
std::vector<ModelSource> SkinSources() {
    std::vector<ModelSource> out;
    for (const auto& [address, m] : g_skins)
        for (size_t i = 0; i < m.parts.size(); ++i)
            if (!m.parts[i].packet.empty())
                out.push_back(ModelSource{address, int32_t(i), int32_t(m.parts.size()), m.parts[i].vertices, m.parts[i].packet});
    return out;
}
void ReplaySkinSources(const std::vector<ModelSource>& sources) {
    DrainBuilds();
    g_skins.clear();
    g_blendSkins.clear();
    auto& scene = Scene();
    const uint64_t decoded = scene.skins_decoded, refused = scene.skins_refused;
    for (const ModelSource& s : sources)
        LoadSkinPart(s.model, s.index, s.count, s.packet.data(), int32_t(s.packet.size()), s.vertices);
    scene.skins_decoded = decoded;
    scene.skins_refused = refused;
}
}  // namespace te::gfx

extern "C" {

void ps2e_gfx2_skin_loaded(ps2e_u32 skin, ps2e_s32 index, ps2e_s32 count, ps2e_u32 packet, ps2e_s32 size, ps2e_s32 vertices) {
    const uint8_t* bytes = size > 0 ? te::GuestPointer(packet, size_t(size)) : nullptr;
    LoadSkinPart(skin, index, count, bytes, size, vertices);
}
void ps2e_gfx2_skin_released(ps2e_u32 skin) {
    te::gfx::DrainBuilds();   // a posted build may still read its parts
    g_skins.erase(skin);
}
void ps2e_gfx2_blend_skin_released(ps2e_u32 skin) {
    g_blendSkins.erase(skin);
}

// DrawSkin (skins.cpp SetSkinDMA): every submodel under its material through the skinned lit strip program
void ps2e_graphics_draw_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights, ps2e_s32 mode) {
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
    Timed timed(te::gfx::Scene().skin_ms);
    const auto* skin = Guest<te_gfx_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
    auto it = g_skins.find(skinAddress);
    if (it == g_skins.end()) { te::gfx::Scene().submodels_missing++; return; }
    SkinGeo::Inputs in;
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    auto& scene = te::gfx::Scene();
    scene.skin_triangles += g_deferredSkinTriangles.exchange(0, std::memory_order_relaxed);
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
    const uint32_t count = std::min<uint32_t>(skin->count, uint32_t(it->second.parts.size()));
    const uint64_t skinId = OrdinalId(TagId(skinAddress, 0));   // the k-th draw of this skin this frame
    for (uint32_t i = 0; i < count; ++i) {
        const SkinPart& part = it->second.parts[i];
        if (!part.decoded) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};
        // the interpolator's identity: this skin at this instance matrix, submodel i -- a vertex run paired when its count holds
        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{skinId, i, ps2r::ps2e::InterpKind::Vertices});
        if (Deferrable(g_diag.Logging())) {
            // the build on a worker: the placeholder takes this submodel's place in the bucket
            te::gfx::DeferredBuild& build = te::gfx::ReserveDeferred(bucket, state);
            te::gfx::PostBuild(build, [&build, geometry = &part.part, in, buildState = BuildState(), textured = state.texture != 0, skinAddress, i] {
                std::vector<ps2r::gs::DrawIr> built;
                const SkinGeo::Result r = SkinGeo::Build(*geometry, in, buildState, built);
                if (r != SkinGeo::Result::Ok) {
                    static std::atomic<int> said{0};
                    if (said++ < 8) std::fprintf(stderr, "[te.gfx] skin %#x part %u: Build refused (%d)\n", skinAddress, i, int(r));
                    return;
                }
                Collector collector{&build, textured};
                for (const auto& d : built) collector.Append(d.verts.data(), d.verts.size());
                g_deferredSkinTriangles += collector.triangles;
            });
            continue;
        }
        draws.clear();
        const SkinGeo::Result r = SkinGeo::Build(part.part, in, BuildState(), draws);
        if (r != SkinGeo::Result::Ok) {
            static int said = 0;
            if (said++ < 8) std::fprintf(stderr, "[te.gfx] skin %#x part %u: Build refused (%d)\n", skinAddress, i, int(r));
            continue;
        }
        emitter.Begin(bucket, state);
        emitter.flatTint = in.clipped ? 1 : 0;
        DiagBox box;
        const bool logging = g_diag.Logging();
        emitter.box = logging ? &box : nullptr;
        const size_t triangles = EmitSkinDraws(emitter, draws);
        emitter.box = nullptr;
        if (logging) SkinDiag("skin", skinAddress, i, count, in, bucket, m, state, triangles, box, int(r));
        scene.skin_triangles += triangles;
    }
    te::gfx::ClearDrawTag();
    scene.skins++;
}

// DrawBlendSkin (skins.cpp SetBlendSkinDMA / WriteBlendSubModel / WriteBlendPart): every submodel's parts, each part's
// packet through the blend programs with the shapes the game lists (weights[i] on shapes[i]) over the first part's factors
void ps2e_graphics_draw_blend_skin(ps2e_u32 skinAddress, ps2e_u32 joints, ps2e_s32 jointCount, ps2e_u32 matrix, ps2e_u32 lights,
                                   ps2e_u32 weightsAddress, ps2e_u32 shapesAddress, ps2e_s32 shapeCount, ps2e_s32 mode) {
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr;
    if (noDraw) return;
    Timed timed(te::gfx::Scene().skin_ms);
    const auto* skin = Guest<te_gfx_blend_skin>(skinAddress);
    if (!skin || !te::gfx::EnsureDevice()) return;
    SkinGeo::Inputs in;
    if (!SkinInputs(joints, jointCount, matrix, lights, mode, in)) return;
    if (shapeCount < 0 || shapeCount > 64) return;
    const float* weights = shapeCount ? reinterpret_cast<const float*>(te::GuestPointer(weightsAddress, 4 * size_t(shapeCount))) : nullptr;
    const int32_t* shapes = shapeCount ? reinterpret_cast<const int32_t*>(te::GuestPointer(shapesAddress, 4 * size_t(shapeCount))) : nullptr;
    if (shapeCount && (!weights || !shapes)) return;
    const uint32_t* subModels = skin->count ? reinterpret_cast<const uint32_t*>(te::GuestPointer(skin->subModels, 4 * size_t(skin->count))) : nullptr;
    if (!subModels || skin->count > 4096) return;
    auto& scene = te::gfx::Scene();
    auto& cache = g_blendSkins[skinAddress];
    static Emitter emitter;
    static std::vector<ps2r::gs::DrawIr> draws;
    const uint64_t blendId = OrdinalId(TagId(skinAddress, 1));
    for (uint32_t i = 0; i < skin->count; ++i) {
        const auto* subModel = Guest<te_gfx_blend_sub_model>(subModels[i]);
        if (!subModel || !subModel->count || subModel->count > 4096) continue;
        const uint32_t* parts = reinterpret_cast<const uint32_t*>(te::GuestPointer(subModel->parts, 4 * size_t(subModel->count)));
        if (!parts) continue;
        ps2e_draw_state state;
        uint32_t bucket;
        MaterialView m;
        SkinMaterial(skin->materials, skin->count, i, state, bucket, &m);
        if (m.shader && std::isfinite(m.shader->scroll[0]) && std::isfinite(m.shader->scroll[1])) in.scroll = {m.shader->scroll[0], m.shader->scroll[1]};
        else in.scroll = {0.f, 0.f};
        const auto* first = Guest<te_gfx_blend_part>(parts[0]);
        if (!first) continue;
        SkinGeo::BlendInputs blend;
        blend.factors = {first->shapeFactors[0], first->shapeFactors[1], first->shapeFactors[2]};
        te::gfx::SetDrawTag(ps2r::ps2e::InterpTag{blendId, i, ps2r::ps2e::InterpKind::Vertices});
        emitter.Begin(bucket, state);
        emitter.flatTint = in.clipped ? 1 : 0;
        DiagBox box;
        const bool logging = g_diag.Logging();
        emitter.box = logging ? &box : nullptr;
        size_t triangles = 0;
        int lastResult = 0;
        for (uint32_t p = 0; p < subModel->count; ++p) {
            const auto* part = Guest<te_gfx_blend_part>(parts[p]);
            if (!part || !part->packet || !part->packetSize) continue;
            BlendPartGeo& geo = cache[part->packet];
            if (!geo.tried) {
                geo.tried = true;
                const uint8_t* bytes = te::GuestPointer(part->packet, part->packetSize);
                const SkinGeo::Result r = bytes ? SkinGeo::DecodePart(bytes, part->packetSize, 0, geo.part) : SkinGeo::Result::InvalidAsset;
                if (r == SkinGeo::Result::Ok) {
                    geo.decoded = true;
                    scene.skins_decoded++;
                } else {
                    scene.skins_refused++;
                    static int said = 0;
                    if (said++ < 16)
                        std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: packet not decoded (%d; %u bytes)\n", skinAddress, i, p,
                                     int(r), part->packetSize);
                    if (const char* dir = std::getenv("TE_GFX_SKIN_DUMP")) {
                        char path[512];
                        std::snprintf(path, sizeof path, "%s/blend_%08x_%u_%u.bin", dir, skinAddress, i, p);
                        if (FILE* f = std::fopen(path, "wb")) { if (bytes) std::fwrite(bytes, 1, part->packetSize, f); std::fclose(f); }
                    }
                }
            }
            if (!geo.decoded) continue;
            // The shapes' displacement rows (V4-8 per vertex: shapeCounts[shape] rows of 4 bytes within shapeSizes[shape] quadwords)
            blend.shapes.clear();
            const uint32_t* addresses = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::GuestPointer(part->shapeAddresses, 4 * size_t(part->shapeCount))) : nullptr;
            const uint32_t* counts = part->shapeCount ? reinterpret_cast<const uint32_t*>(te::GuestPointer(part->shapeCounts, 4 * size_t(part->shapeCount))) : nullptr;
            bool ok = true;
            for (int32_t k = 0; k < shapeCount && ok; ++k) {
                const int32_t shape = shapes[k];
                if (shape < 0 || shape >= int32_t(part->shapeCount) || !addresses || !counts) { ok = false; break; }
                SkinGeo::Shape sh;
                sh.weight = weights[k];
                sh.count = counts[shape];
                sh.rows = sh.count ? te::GuestPointer(addresses[shape], 4 * size_t(sh.count)) : nullptr;
                if (!sh.rows) { ok = false; break; }
                blend.shapes.push_back(sh);
            }
            if (!ok) continue;
            draws.clear();
            const SkinGeo::Result r = SkinGeo::BuildBlend(geo.part, in, blend, BuildState(), draws);
            lastResult = int(r);
            if (r != SkinGeo::Result::Ok) {
                static int said = 0;
                if (said++ < 8) std::fprintf(stderr, "[te.gfx] blend skin %#x submodel %u part %u: BuildBlend refused (%d)\n", skinAddress, i, p, int(r));
                continue;
            }
            triangles += EmitSkinDraws(emitter, draws);
        }
        emitter.box = nullptr;
        if (logging) SkinDiag("blendskin", skinAddress, i, skin->count, in, bucket, m, state, triangles, box, lastResult);
        scene.skin_triangles += triangles;
    }
    te::gfx::ClearDrawTag();
    scene.blend_skins++;
}

}  // extern "C"
