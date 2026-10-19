// platform/nativeee (host side) -- graphics.h's decal entries: the footfalls, ripples and scorches the game keeps
// in DecalData (include/game/decals.h) and ages / draws every frame (decals.cpp UpdateDecalsVU0 + Platform::Graphics::DrawDecals).
// The PS2 renderer (src/platform/ps2/renderer/particles.cpp) is the spec and its two microprograms are RUN here as they are:
//
//  * AgeDecal: the retail decal set's VU0 microprogram (.vutext D_002E5BD0 in the retail image, program 0x1C0 for decals in
//    the camera's chunk, 0x360 for ones placed in another chunk -- their place taken through that chunk's matrix first)
//    on the engine's VU interpreter, over a VU0 data image laid out as LoadDecalView / LoadDecalType filled it: the far
//    frustum's four side planes in the chunk's space (transposed, quadwords 0-3), the near plane four times (4-7), the other
//    chunk's matrix (8-11), each type's variants from quadword 0x10 (eight each: four colour keys, four size keys). The
//    decal in vf01 (its place, w its life left in seconds), vf02 / vf03 (its frame's half words), vi01 the variant's address;
//    out: vf28 its place now, vf29 its colour (bytes), vf31 / vf30 its corners' two half-diagonal offsets in 4.12 (w of the
//    first = non-zero when the decal lies outside the frustum: the VU1 program then draws nothing for it).
//  * DrawDecals: for each key with a draw list, the material's bucket gets the quads the VU1 library's own decal program
//    (vs_zero_kick class A + vs_zero_kick_a_cont: the dispatcher stub at entry 0, then the 0x1750 billboard emitter --
//    ZeroKick::RunA / RunACont) kicks over a host VU image laid out as the retail VIF stream left it: the matrix to the screen
//    at 0x4F, the UV packet (its GIF tag and the types' ST rectangles) at 0x53, the block's places / sizes / colours at TOP,
//    the node list with the four packets' addresses. Each kicked packet (8 decals: ST, RGBAQ, XYZ2 for four corners, ADC on
//    the first two) is lowered as the GS assembles a strip (te::gfx::LowerPackedPacket) into the material's bucket (the
//    particles' 22, where the PS2 put the page's mode materials) -- the decal pass's blend state is the material's.
//
// What the game reads back is kept exactly: the draw lists emptied, `chunk` cleared when there are no decals; the block's
// VIF codes and the UV packet's GIF tag are written as the PS2 wrote them (the game never reads them, the VU1 image does).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/vu/hle/vu1_hle_contract.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/zero_kick.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_gfx_layout.h"
#include "te_host.h"

namespace {
using te::gfx::Draw;
using te::gfx::MaterialView;

// ---- the game's layouts (include/game/decals.h, view.h) ------------------------------------------------------------------------
constexpr uint32_t kBlockCount = 32, kBlockDecals = 32, kKeyCount = 4, kMostTypes = 16, kMostVariants = 15;
constexpr uint32_t kBlockBytes = 0x6F0, kTypeBytes = 0x890, kVariantBytes = 0x80, kUvPacketBytes = 0x410;
// DecalBlock
constexpr uint32_t kBlockDrawNext = 0x0C, kBlockPlacesUnpack = 0x230, kBlockPlaces = 0x240, kBlockSizesUnpack = 0x440,
                   kBlockSizes = 0x450, kBlockColoursUnpack = 0x650, kBlockColours = 0x660, kBlockStart = 0x6E0;
// DecalData
constexpr uint32_t kDataPage = 0xDE00, kDataUvPacket = 0xDE20, kDataTypeCount = 0xE230, kDataDrawLists = 0xE304,
                   kDataChunk = 0xE314, kDataCount = 0xE318, kDataBytes = 0xE320;
constexpr uint32_t kPageMaterials = 0x0C;   // ParticlePage::materials[4]
// DecalType
constexpr uint32_t kTypeVariants = 0x10, kTypeVariantCount = 0x790;
// RenderView (G_RendRel -> RenderView*; toScreen at 0xC0)
constexpr uint32_t kRenderViewPointer = 0x00309BE0, kViewToScreen = 0xC0;
// g_ViewPlanes / g_FarViewPlanes (six planes: near, four sides, far)
constexpr uint32_t kViewPlanes = 0x003B4900, kFarViewPlanes = 0x003B4960;
constexpr int kNearPlane = 0, kFirstSidePlane = 1, kFarPlane = 5;

// ---- the PS2 renderer's constants (particles.cpp) ------------------------------------------------------------------------------
constexpr uint32_t kVuMatrixAddress = 0x4F, kVuHeadAddress = 0x53;
constexpr uint32_t kVu0VariantsAddress = 0x10, kVariantQuadwords = kVariantBytes / 16;
constexpr uint32_t kSameChunkProgram = 0x1C0, kOtherChunkProgram = 0x360;
constexpr uint32_t kDecalVu0Programs = 0x002E5BD0;   // g_DecalVu0Programs: the DMA chain to VIF0 (.vutext)
// InitDecalBlock / InitDecalUvPacket's words
constexpr uint32_t kPlacesUnpack = 0x6C208000, kSizesUnpack = 0x6D408020, kColoursUnpack = 0x6E208060, kStartProgram = 0x17000000;
constexpr uint64_t kUvGifTag = 0xC002400000008008ull;
constexpr uint32_t kUvRegisters = 0x12512512, kUvMoreRegisters = 0x5125;
// The VU1 image: the node list, the block's double-buffer place (TOP) and the four kicked packets (1 + 8 x 12 quadwords)
constexpr uint32_t kNode = 0x20, kTop = 0x100, kPacketBase = 0x180, kPacketStride = 0x68;   // a packet: 1 + 8 x 12 quadwords
constexpr uint32_t kEmitterPc = 0x1720, kClassATpc = 0x1750;
constexpr uint32_t kQwCount = 1024;

template <typename T>
const T* Guest(uint32_t address) { return reinterpret_cast<const T*>(te::GuestPointer(address, sizeof(T))); }
uint32_t ReadU32(uint32_t address) { const uint32_t* p = Guest<uint32_t>(address); return p ? *p : 0; }

// ---- the VU0 side: the retail decal set on the engine's interpreter ----------------------------------------------------------
struct DecalVu0 {
    bool tried = false, ready = false;
    std::unique_ptr<VuUnit> vu;
    std::unique_ptr<VuInterpreterTier> tier;
    uint32_t program = kSameChunkProgram;
    bool viewLoaded = false;

    // The set's DMA chain (CNT tags; VIF codes in the tag's upper words then the body) parsed for its MPG payloads
    bool Load() {
        if (tried) return ready;
        tried = true;
        std::array<uint8_t, 4096> image{};
        bool any = false;
        uint32_t at = kDecalVu0Programs;
        for (int tags = 0; tags < 16; ++tags) {
            const uint32_t* tag = reinterpret_cast<const uint32_t*>(te::GuestPointer(at, 16));
            if (!tag) break;
            const uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7;
            const uint8_t* body = te::GuestPointer(at + 16, size_t(qwc) * 16);
            if (!body) break;
            std::vector<uint8_t> stream(8 + size_t(qwc) * 16);
            std::memcpy(stream.data(), tag + 2, 8);
            std::memcpy(stream.data() + 8, body, size_t(qwc) * 16);
            for (size_t p = 0; p + 4 <= stream.size();) {
                uint32_t code;
                std::memcpy(&code, stream.data() + p, 4);
                p += 4;
                const uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
                if (cmd == 0x4A) {   // MPG
                    const size_t n = num ? num : 256, a = size_t(imm) * 8;
                    if (a + n * 8 <= image.size() && p + n * 8 <= stream.size()) { std::memcpy(image.data() + a, stream.data() + p, n * 8); any = true; }
                    p += n * 8;
                }
            }
            at += 16 + qwc * 16;
            if (id == 6 || id == 7) break;
        }
        // The two programs' first bundles (LQ.w vf28, 5(vi01) / ITOF15 vf02 + LQ.xyz vf31, 8(vi00)) must be there
        uint32_t first1c0, first360;
        std::memcpy(&first1c0, image.data() + kSameChunkProgram, 4);
        std::memcpy(&first360, image.data() + kOtherChunkProgram, 4);
        if (!any || first1c0 != 0x003C0805u || first360 != 0x01DF0008u) {
            std::fprintf(stderr, "[te.gfx] decals: the retail VU0 decal set is not in the image (%#x: %08x / %#x: %08x); AgeDecal keeps every decal unaged\n",
                         kSameChunkProgram, first1c0, kOtherChunkProgram, first360);
            return false;
        }
        vu = std::make_unique<VuUnit>(0);
        tier = std::make_unique<VuInterpreterTier>();
        vu->WriteMicroMem(0, image.data(), image.size());
        ready = true;
        return true;
    }
} g_vu0;

// The game's TransformPlane (math.cpp): the normal through the three rotation rows, d less the normal at the translation
void TransformPlane(const float plane[4], const float (*m)[4], float out[4]) {
    for (int c = 0; c < 3; ++c) out[c] = plane[0] * m[0][c] + plane[1] * m[1][c] + plane[2] * m[2][c];
    out[3] = plane[3] - (out[0] * m[3][0] + out[1] * m[3][1] + out[2] * m[3][2]);
}

// TransposeQuadwords: four quadwords' words transposed in place (rows made columns)
void Transpose(float q[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) std::swap(q[i][j], q[j][i]);
}

// ---- the VU1 side: one block through the class-A program -----------------------------------------------------------------------
struct Image {
    std::vector<uint8_t> memory;
    Image() : memory(kQwCount * 16) {}
    void Set(uint32_t qw, unsigned lane, uint32_t value) { std::memcpy(&memory[qw * 16 + lane * 4], &value, 4); }
    void SetQw(uint32_t qw, const void* bytes16) { std::memcpy(&memory[qw * 16], bytes16, 16); }
};

// The block's quadwords as the VIF UNPACKs left them at TOP: V4-32 places (32), V4-16 signed sizes then moreSizes (64), V4-8
// signed colours (32)
void UnpackBlock(Image& img, const uint8_t* block) {
    for (uint32_t i = 0; i < kBlockDecals; ++i) img.SetQw(kTop + i, block + kBlockPlaces + i * 16);
    for (uint32_t i = 0; i < kBlockDecals * 2; ++i) {
        int16_t h[4];
        std::memcpy(h, block + kBlockSizes + i * 8, 8);
        for (unsigned k = 0; k < 4; ++k) img.Set(kTop + 32 + i, k, uint32_t(int32_t(h[k])));
    }
    for (uint32_t i = 0; i < kBlockDecals; ++i) {
        int8_t b[4];
        std::memcpy(b, block + kBlockColours + i * 4, 4);
        for (unsigned k = 0; k < 4; ++k) img.Set(kTop + 96 + i, k, uint32_t(int32_t(b[k])));
    }
}

bool RunBlock(const uint8_t* block, const uint8_t* uvPacket, uint32_t uvQuadwords, const float (*matrix)[4], std::vector<std::vector<uint8_t>>& kicks) {
    Image img;
    for (uint32_t r = 0; r < 4; ++r) img.SetQw(kVuMatrixAddress + r, matrix[r]);
    for (uint32_t q = 0; q < uvQuadwords; ++q) img.SetQw(kVuHeadAddress + q, uvPacket + q * 16);
    UnpackBlock(img, block);
    // the dispatcher's node list: QW3.x = the node; the node's call (the emitter), then the four packets' addresses the
    // program walks from node + 4 down to node + 1
    img.Set(3, 0, kNode);
    img.Set(kNode, 0, kEmitterPc / 8);
    for (uint32_t p = 0; p < 4; ++p) img.Set(kNode + 1 + p, 0, kPacketBase + p * kPacketStride);
    VUregs regs{};
    regs.VF[0].I[3] = 0x3f800000u;   // vf00 = (0, 0, 0, 1)
    Vu1HleInput in{};
    in.micro = nullptr;
    in.data = img.memory.data();
    in.start_pc = 0;
    in.top = in.tops = kTop;
    in.itop = in.itops = 0;
    in.regs = &regs;
    Vu1HleOutput stub;
    if (!ZeroKick::RunA(in, stub) || !stub.has_regs || stub.regs.TPC != kClassATpc) return false;
    for (const auto& w : stub.data_writes)
        if (w.addr + w.bytes.size() <= img.memory.size()) std::memcpy(&img.memory[w.addr], w.bytes.data(), w.bytes.size());
    VUregs entry = stub.regs;
    in.regs = &entry;
    in.start_pc = kClassATpc;
    Vu1HleOutput run;
    if (!ZeroKick::RunACont(in, run)) return false;
    kicks = std::move(run.kicks);
    return true;
}
}  // namespace

extern "C" {

// InitDecalBlock (particles.cpp): the block's VIF codes -- UNPACKs of the places (V4-32), sizes (V4-16) and colours (V4-8) to
// their VU1 addresses, and the MSCNT. The VU1 image is built from the game's arrays directly; the words ride in the block for
// the layout's (and a save state's) sake
void ps2e_graphics_init_decal_block(ps2e_u32 blockAddress) {
    uint8_t* block = te::GuestPointer(blockAddress, kBlockBytes);
    if (!block) return;
    auto word = [&](uint32_t at, uint32_t value) { std::memcpy(block + at, &value, 4); };
    word(kBlockStart + 12, kStartProgram);
    word(kBlockPlacesUnpack + 12, kPlacesUnpack);
    word(kBlockSizesUnpack + 12, kSizesUnpack);
    word(kBlockColoursUnpack + 12, kColoursUnpack);
}

// InitDecalUvPacket: the GIF tag of the UV packet's strips (8 loops of a tri strip, PRE with PRIM 4, PACKED, the 12 registers
// ST, RGBAQ, XYZ2 of four corners) -- the VU1 program copies this quadword to the head of every packet it kicks
void ps2e_graphics_init_decal_uv_packet(ps2e_u32 packetAddress) {
    uint8_t* packet = te::GuestPointer(packetAddress, kUvPacketBytes);
    if (!packet) return;
    std::memcpy(packet, &kUvGifTag, 8);
    std::memcpy(packet + 8, &kUvRegisters, 4);
    std::memcpy(packet + 12, &kUvMoreRegisters, 4);
}

// LoadDecalView: the view's quadwords into VU0's memory -- the far frustum's four side planes in the chunk's space as rows and
// four copies of the near plane, each four transposed; then the other chunk's matrix when there is one; the program to run on
// each decal (0x1C0 same chunk / 0x360 other chunk)
void ps2e_graphics_load_decal_view(ps2e_u32 chunkMatrices, ps2e_u32 fromChunk) {
    if (!g_vu0.Load()) return;
    const auto* chunk = reinterpret_cast<const float(*)[4]>(te::GuestPointer(chunkMatrices, 64));
    const float* far = reinterpret_cast<const float*>(te::GuestPointer(kFarViewPlanes, 6 * 16));
    const float* view = reinterpret_cast<const float*>(te::GuestPointer(kViewPlanes, 6 * 16));
    if (!chunk || !far || !view) return;
    float rows[4][4], copies[4][4];
    for (int plane = kFirstSidePlane; plane < kFarPlane; ++plane) TransformPlane(far + plane * 4, chunk, rows[plane - kFirstSidePlane]);
    float near[4];
    TransformPlane(view + kNearPlane * 4, chunk, near);
    for (auto& copy : copies) std::memcpy(copy, near, 16);
    Transpose(rows);
    Transpose(copies);
    g_vu0.vu->WriteDataMem(0, reinterpret_cast<const uint8_t*>(rows), 64);
    g_vu0.vu->WriteDataMem(64, reinterpret_cast<const uint8_t*>(copies), 64);
    g_vu0.program = kSameChunkProgram;
    if (fromChunk) {
        if (const uint8_t* from = te::GuestPointer(fromChunk, 64)) {
            g_vu0.vu->WriteDataMem(128, from, 64);
            g_vu0.program = kOtherChunkProgram;
        }
    }
    g_vu0.viewLoaded = true;
    if (std::getenv("TE_DECAL_DIAG")) {
        static int said = 0;
        if (said++ < 8) {
            std::fprintf(stderr, "[te.gfx.diag] decal view: chunk rows (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) from %#x\n",
                         chunk[0][0], chunk[0][1], chunk[0][2], chunk[0][3], chunk[1][0], chunk[1][1], chunk[1][2], chunk[1][3],
                         chunk[2][0], chunk[2][1], chunk[2][2], chunk[2][3], chunk[3][0], chunk[3][1], chunk[3][2], chunk[3][3], fromChunk);
            for (int plane = 0; plane < 6; ++plane)
                std::fprintf(stderr, "[te.gfx.diag]   far plane %d (%.3f %.3f %.3f %.2f) view plane (%.3f %.3f %.3f %.2f)\n", plane, far[plane * 4], far[plane * 4 + 1], far[plane * 4 + 2], far[plane * 4 + 3],
                             view[plane * 4], view[plane * 4 + 1], view[plane * 4 + 2], view[plane * 4 + 3]);
            for (int i = 0; i < 4; ++i)
                std::fprintf(stderr, "[te.gfx.diag]   qw%d (%.3f %.3f %.3f %.3f) qw%d (%.3f %.3f %.3f %.3f)\n", i, rows[i][0], rows[i][1], rows[i][2], rows[i][3], 4 + i, copies[i][0], copies[i][1], copies[i][2], copies[i][3]);
        }
    }
}

// LoadDecalType: the type's variants (eight quadwords each: the colour keys then the size keys) into VU0's memory from 0x10
void ps2e_graphics_load_decal_type(ps2e_u32 typeAddress) {
    if (!g_vu0.Load()) return;
    const uint8_t* type = te::GuestPointer(typeAddress, kTypeBytes);
    if (!type) return;
    int32_t count;
    std::memcpy(&count, type + kTypeVariantCount, 4);
    if (count <= 0) return;
    const uint32_t variants = std::min<uint32_t>(uint32_t(count), kMostVariants);
    g_vu0.vu->WriteDataMem(kVu0VariantsAddress * 16, type + kTypeVariants, size_t(variants) * kVariantBytes);
}

// AgeDecal: the decal in vf01-vf03 and its variant's address in vi01 through the view's program; its place in vf28, its colour
// in vf29 and its corners' offsets in vf31 / vf30 (the half words are the lanes' low ones)
void ps2e_graphics_age_decal(ps2e_u32 placeAddress, ps2e_u32 frameAddress, ps2e_s32 variant, ps2e_u32 lookAddress) {
    const float* place = reinterpret_cast<const float*>(te::GuestPointer(placeAddress, 16));
    const int32_t* frame = reinterpret_cast<const int32_t*>(te::GuestPointer(frameAddress, 32));
    uint8_t* look = te::GuestPointer(lookAddress, 32);
    if (!place || !frame || !look) return;
    auto& scene = te::gfx::Scene();
    scene.decals_aged++;
    if (!g_vu0.Load() || !g_vu0.viewLoaded) {
        // No microprogram: the decal keeps its place, drawn white and full size (the game still ages its life)
        std::memcpy(look, place, 16);
        std::memset(look + 16, 0x80, 4);
        std::memset(look + 20, 0, 12);
        return;
    }
    VUregs& r = g_vu0.vu->regs();
    r.Reset();
    for (int k = 0; k < 4; ++k) {
        std::memcpy(&r.VF[1].I[k], &place[k], 4);
        r.VF[2].I[k] = uint32_t(frame[k]);
        r.VF[3].I[k] = uint32_t(frame[4 + k]);
    }
    const uint32_t address = uint32_t(std::max(variant, 0)) * kVariantQuadwords + kVu0VariantsAddress;
    r.VI[1] = uint16_t(address);
    g_vu0.tier->Run(*g_vu0.vu, g_vu0.program);
    // DecalLook: f32 place[4]; u8 colour[4]; s16 sizes[4]; s16 moreSizes[4]
    for (int k = 0; k < 4; ++k) {
        std::memcpy(look + k * 4, &r.VF[28].I[k], 4);
        look[16 + k] = uint8_t(r.VF[29].I[k]);
        const int16_t size = int16_t(r.VF[31].I[k] & 0xFFFF), more = int16_t(r.VF[30].I[k] & 0xFFFF);
        std::memcpy(look + 20 + k * 2, &size, 2);
        std::memcpy(look + 28 + k * 2, &more, 2);
    }
    if (r.VF[31].I[3] & 0xFFFF) scene.decals_culled++;
    if (std::getenv("TE_DECAL_DIAG")) {
        extern uint64_t te_graphics_presents();
        static int said = 0;
        const char* window = std::getenv("TE_DECAL_DIAG_FROM");
        const bool in = window && te_graphics_presents() + 1 >= uint64_t(std::atol(window));
        if (said++ < 64 || in)
            std::fprintf(stderr, "[te.gfx.diag] present %llu decal aged: place (%.2f %.2f %.2f life %.3f) frame (%d %d %d | %d %d %d) variant %d prog %#x -> place (%.2f %.2f %.2f) colour (%u %u %u %u) sizes (%d %d %d w %d) more (%d %d %d)\n",
                         (unsigned long long)te_graphics_presents() + 1, place[0], place[1], place[2], place[3], frame[0], frame[1], frame[2], frame[4], frame[5], frame[6], variant, g_vu0.program,
                         *reinterpret_cast<float*>(look), *reinterpret_cast<float*>(look + 4), *reinterpret_cast<float*>(look + 8),
                         look[16], look[17], look[18], look[19], int16_t(r.VF[31].I[0]), int16_t(r.VF[31].I[1]), int16_t(r.VF[31].I[2]), int16_t(r.VF[31].I[3]),
                         int16_t(r.VF[30].I[0]), int16_t(r.VF[30].I[1]), int16_t(r.VF[30].I[2]));
    }
}

// DrawDecals (particles.cpp): no decals -> no chunk they're in; else each key's draw list (emptied) through its material: the
// view's matrix to the screen, the UV packet, each block's quadwords, the VU1 program's quads into the material's bucket
void ps2e_graphics_draw_decals(ps2e_u32 decalsAddress) {
    uint8_t* decals = te::GuestPointer(decalsAddress, kDataBytes);
    if (!decals) return;
    int32_t count;
    std::memcpy(&count, decals + kDataCount, 4);
    if (count == 0) {
        std::memset(decals + kDataChunk, 0, 4);
        return;
    }
    const uint32_t viewAddress = ReadU32(kRenderViewPointer);
    const auto* toScreen = viewAddress ? reinterpret_cast<const float(*)[4]>(te::GuestPointer(viewAddress + kViewToScreen, 64)) : nullptr;
    if (!toScreen) return;
    static const bool noDraw = std::getenv("TE_GFX_NO_DRAW") != nullptr || std::getenv("TE_GFX_NO_DECALS") != nullptr;
    uint32_t typeCount;
    std::memcpy(&typeCount, decals + kDataTypeCount, 4);
    const uint32_t uvQuadwords = std::min<uint32_t>((std::min<uint32_t>(typeCount, kMostTypes) + 1) * 2 + 1, kUvPacketBytes / 16);
    const uint8_t* uvPacket = decals + kDataUvPacket;
    const float offX = te::gfx::GsOriginX(), offY = te::gfx::GsOriginY();
    auto& scene = te::gfx::Scene();
    static std::vector<std::vector<uint8_t>> kicks;
    static std::vector<ps2e_draw_vertex> verts;
    const bool device = te::gfx::EnsureDevice();
    for (uint32_t key = 0; key < kKeyCount; ++key) {
        uint32_t block;
        std::memcpy(&block, decals + kDataDrawLists + key * 4, 4);
        std::memset(decals + kDataDrawLists + key * 4, 0, 4);
        if (!block) continue;
        const uint32_t materialAddress = ReadU32(decalsAddress + kDataPage + kPageMaterials + key * 4);
        if (!materialAddress || !device || noDraw) continue;
        const MaterialView m = te::gfx::ReadMaterial(materialAddress);
        ps2e_draw_state state = te::gfx::StateOf(m);
        static const bool noDepth = std::getenv("TE_DECAL_NO_DEPTH") != nullptr;   // diagnosis: the quads regardless of the depth test
        if (noDepth) state.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        const uint32_t bucket = m.material ? m.bucket : TE_GFX_BUCKET_UI;
        const bool textured = state.texture != 0 && m.texture;
        const float tw = textured ? float(1 << m.texture->texture.widthPower) : 1.f;
        const float th = textured ? float(1 << m.texture->texture.heightPower) : 1.f;
        for (int guard = 0; guard < int(kBlockCount) && block; ++guard) {
            const uint8_t* bytes = te::GuestPointer(block, kBlockBytes);
            if (!bytes) break;
            kicks.clear();
            if (!RunBlock(bytes, uvPacket, uvQuadwords, toScreen, kicks)) {
                scene.decal_refused++;
                static int said = 0;
                if (said++ < 4) std::fprintf(stderr, "[te.gfx] decal block %#x: the program declined\n", block);
            } else {
                verts.clear();
                for (const auto& packet : kicks) te::gfx::LowerPackedPacket(packet, offX, offY, tw, th, textured, verts);
                if (!verts.empty()) {
                    // the interpolator: decals are the NEWEST picture's as they are -- they lie on static ground and the game
                    // re-sorts each key's list every frame, so a block's quads do not correspond one to one between pictures
                    // (a matched lerp would pair different decals); a camera move re-projects nothing here, which on a ground
                    // stamp is a sub-pixel wobble at most
                    Draw& d = te::gfx::OpenDraw(bucket, state);
                    d.verts.insert(d.verts.end(), verts.begin(), verts.end());
                    te::gfx::CloseDraw(bucket);
                    scene.decal_quads += verts.size() / 6;
                    if (std::getenv("TE_DECAL_DIAG")) {
                        extern uint64_t te_graphics_presents();
                        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                        double a = 0;
                        for (const auto& v : verts) { x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); a += v.a; }
                        std::fprintf(stderr, "[te.gfx.diag] present %llu decals key %u block %#x: %zu quads box (%.0f %.0f)-(%.0f %.0f) alpha %.0f bucket %u tex %u blend %d(%u %u %u %u)\n",
                                     (unsigned long long)te_graphics_presents() + 1, key, block, verts.size() / 6, x0, y0, x1, y1, a / double(verts.size()), bucket, state.texture,
                                     state.blend, state.blend_a, state.blend_b, state.blend_c, state.blend_d);
                    }
                }
                scene.decal_blocks++;
                if (const char* diag = std::getenv("TE_GFX_DIAG_PRESENT")) {
                    extern uint64_t te_graphics_presents();
                    if (te_graphics_presents() + 1 == uint64_t(std::atol(diag))) {
                        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                        double r = 0, g = 0, b = 0, a = 0;
                        for (const auto& v : verts) { x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); r += v.r; g += v.g; b += v.b; a += v.a; }
                        const size_t n = verts.size() ? verts.size() : 1;
                        int16_t blockCount;
                        std::memcpy(&blockCount, bytes, 2);
                        std::fprintf(stderr, "[te.gfx.diag] present %s decals key %u block %#x (%d decals) bucket %u shader %#x tex %u (%gx%g) blend %d(%u %u %u %u) depth %u/%u tris %zu box (%.0f %.0f)-(%.0f %.0f) rgba (%.0f %.0f %.0f %.0f) kicks %zu\n",
                                     diag, key, block, blockCount, bucket, m.shader ? unsigned(m.shader->type) : 0u, state.texture, tw, th, state.blend, state.blend_a, state.blend_b, state.blend_c, state.blend_d,
                                     state.depth_test, state.depth_write, verts.size() / 3, x0, y0, x1, y1, r / n, g / n, b / n, a / n, kicks.size());
                    }
                }
            }
            std::memcpy(&block, bytes + kBlockDrawNext, 4);
        }
    }
}

}  // extern "C"
