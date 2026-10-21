// CrysPrograms -- vs_crys_dual / vs_crys_displace (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/crys_programs.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/displace_prefix.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/lit_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.h"

using namespace vu1lib;

namespace {

constexpr u32 kEnvEmitterPc = 0x1720;  // the env-lit material sub (= the variant base)
constexpr u32 kLitEmitterPc = 0x21b0;  // the relocated lit body
constexpr u32 kExitPc = 0x90;          // the E-bit exit node
constexpr u32 kScaleEntryPc = 0x12a0;  // the shared-prefix scale pass
constexpr u32 kGateHelperPc = 0xB0;    // the shared clip gate (QW4.y * 8)
constexpr u32 kEnvGateLink = 0x306;    // (gate JALR pc 0x1820 + 16) / 8
constexpr u32 kLitGateLink = 0x44B;    // the relocated lit body's gate link
constexpr u32 kEnvFanPc = 0x1D10;      // the crystal env fan entry
constexpr int kEnvFanKind = 3;         // the crystal env fan body
constexpr u32 kLitFanPc = 0x2628;      // the relocated lit fan entry
constexpr int kLitFanKind = 1;         // byte-identical to the lit fan body
// The one image whose fan bodies are identity-verified (full clips lift only for it).
constexpr u64 kClipImage = 0x8980533e9df9f1faull;

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

}  // namespace

bool CrysPrograms::RunCrysEnv1720(QwPtr m, u32 node, u32 top, Vu1HleOutput& out, ClipGate::GateRegs* gr) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)
    const V4 p2 = load(node + 2);              // LQ vf15, 2(vi14)
    const V4 p3 = load(node + 3);              // LQ vf16, 3(vi14)
    const V4 p4 = load(node + 4);              // LQ vf17, 4(vi14)

    u32 mesh = m.W(2, 0) & 0xFFFF;               // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;  // ILW.y vi10, 0(vi13) (once)

    // Data-driven termination (IBNE mesh == mesh_end below); safety bound only.
    for (int mesh_guard = 0; mesh_guard < kMeshRingBound; ++mesh_guard) {
        const u32 gate_flag = m.W(mesh, 0) & 0xFFFF;  // ILW.x vi11
        const u32 clip_off = m.W(mesh, 2) & 0xFFFF;   // ILW.z vi03
        V4 obj[4];
        for (int r = 0; r < 4; ++r) obj[r] = load(mesh + 1 + r);  // vf01..04
        if (gr != nullptr) {  // body pins at gate time
            gr->vi09 = (u16)mesh_end;
            gr->vi13 = (u16)mesh;
            for (int r = 0; r < 4; ++r) gr->obj[r] = obj[r];
        }

        const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // via MR32+MTIR vf19
        if ((vqw_count & 3) != 0) return false;
        const u32 nverts = vqw_count / 4;
        if (nverts < 1) return false;  // the vertex loop is do-while

        if (gate_flag != 0) {
            // Conditional clip-gate call (skipped when the mesh flag is 0).
            const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
            if (helper_pc != kGateHelperPc) return false;
            V4 clipR[5];
            for (int r = 0; r < 5; ++r) clipR[r] = load(mesh + clip_off + r);
            if (!ClipGate::ClipGateB0(m, node, top, kEnvGateLink, clipR, nverts, gr, gr != nullptr ? &out : nullptr))
                return false;
        }

        // Post-gate constants.
        const u32 out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
        V4 nrmR[3];
        for (int r = 0; r < 3; ++r) nrmR[r] = load(mesh + 5 + r);  // vf05..07
        V4 litR[4];
        for (int r = 0; r < 4; ++r) litR[r] = load(mesh + 9 + r);  // vf08..11
        V4 stR[3];
        for (int r = 0; r < 3; ++r) stR[r] = load(mesh + clip_off + r);  // vf12..14
        for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));    // GIFtag

        const u32 vbase = top + 3;
        V4 end_stp{};  // the last vertex's 4-lane stp (the vf27 leftover)
        for (u32 j = 0; j < nverts; ++j) {
            const u32 vq = vbase + j * 4;
            const V4 pos = load(vq);       // LQ vf20, 0(vi06)
            const V4 attr = load(vq + 1);  // LQ vf21 (.w = strip ADC)
            const V4 nrm = load(vq + 2);   // LQ vf22
            const V4 col = load(vq + 3);   // LQ vf23 (RGBA ints)

            // T = ObjMtx * pos (MULAw/MADDAy/MADDAz/MADDx); Q = 1/T.w.
            const V4 T = XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                                   CF(bits2f(pos.b[2])));
            const float Q = VuDiv(1.0f, bits2f(T.b[3]));

            // World normal wn = nrmR * n (MULAx/MADDAy/MADDz, fused).
            const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
            float wn[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
                acc = CF(VuFloat::Fma(CF(bits2f(nrmR[1].b[k])), ny, acc));
                wn[k] = CF(VuFloat::Fma(CF(bits2f(nrmR[2].b[k])), nz, acc));
            }

            // Lighting -> RGBA (MAXx vs +0; MULAw/MADDAx/MADDAy/MADDz; MUL.xyz
            // by ITOF0 colour; MINIi 255; FTOI0).
            u32 rgba[4];
            {
                const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
                const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
                const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
                float lit[3];
                for (int k = 0; k < 3; ++k) {
                    float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
                    acc = CF(VuFloat::Fma(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
                    lit[k] = CF(VuFloat::Fma(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
                }
                for (int k = 0; k < 3; ++k) {
                    const float s = (float)(int32_t)col.b[k];  // ITOF0
                    const float mod = CF(CF(lit[k]) * CF(s));  // MUL.xyz
                    rgba[k] = Ftoi0(Fmin_raw(f2bits(mod), f2bits(255.0f)));
                }
                const float sa = (float)(int32_t)col.b[3];
                rgba[3] = Ftoi0(Fmin_raw(f2bits(sa), f2bits(255.0f)));
            }

            // ADC: sext16((low16(pos.w) & flag) | low16(attr.w)).
            const u32 adc16 = ((pos.b[3] & 0xFFFF) & gate_flag) | (attr.b[3] & 0xFFFF);
            const u32 adc = (adc16 & 0x8000) ? (0xFFFF0000u | adc16) : adc16;

            // Screen position scr = MULq.xyz(T, Q) (stored clamped).
            u32 scr[3];
            for (int k = 0; k < 3; ++k) scr[k] = f2bits(CF(CF(bits2f(T.b[k])) * Q));

            // Env-mapped ST (see the header).
            u32 stb[3];
            {
                float st[2];
                for (int k = 0; k < 2; ++k) {
                    const float d = CF(CF(bits2f(scr[k])) - CF(bits2f(p3.b[k])));  // SUB.xy
                    st[k] = CF(CF(d) * CF(bits2f(p4.b[k])));                       // MUL.xy
                }
                float stp[2];
                {
                    // The stp chain (MADDx) is a 4-lane op into vf27: every lane
                    // is computed so the gate-persistent leftover is exact; the
                    // ST math consumes .xy.
                    for (int k = 0; k < 4; ++k) {
                        float acc = CF(CF(bits2f(stR[2].b[k])) * nz);
                        acc = CF(VuFloat::Fma(CF(bits2f(stR[1].b[k])), ny, acc));
                        end_stp.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(stR[0].b[k])), nx, acc)));
                    }
                    stp[0] = CF(bits2f(end_stp.b[0]));
                    stp[1] = CF(bits2f(end_stp.b[1]));
                }
                const float p3z = CF(bits2f(p3.b[2]));
                for (int k = 0; k < 2; ++k) {  // MULAw.xy ACC, st, vf00.w; MADDz.xy st, stp, vf16
                    const float acc = CF(CF(st[k]) * 1.0f);
                    float v = CF(VuFloat::Fma(CF(stp[k]), p3z, acc));
                    u32 vb2 = Fmin_raw(f2bits(v), 0x3f800000u);                     // MINIw
                    vb2 = Fmax_raw(vb2, 0u);                                        // MAXx
                    const float vc = CF(CF(bits2f(vb2)) + CF(bits2f(p2.b[k])));  // ADD.xy
                    stb[k] = f2bits(CF(CF(vc) * Q));                             // MULq
                }
                const float one = CF(0.0f + 1.0f);  // ADDw.z
                stb[2] = f2bits(CF(CF(one) * Q));   // MULq.z
            }

            // Stores: ST xyz only (.w VIF-prefilled), RGBA xyzw, XYZ2 = {FTOI0(scr), adc}.
            const u32 st_qw = out_base + 1 + 3 * j;
            for (int k = 0; k < 3; ++k) m.SetW(st_qw, k, stb[k]);
            for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, rgba[k]);
            for (int k = 0; k < 3; ++k) m.SetW(st_qw + 2, k, Ftoi0(scr[k]));
            m.SetW(st_qw + 2, 3, adc);
        }

        // Kicks: the node header packet at node+5 first when node.w != 0, then the strip.
        if (node_w != 0) {
            if (!KickAt(m, node + 5, out)) return false;
        }
        if (!KickAt(m, out_base, out)) return false;

        RotateQw1(m);

        // Gate-persistent register leftovers for the NEXT gate call (the
        // following relocated-lit node or a second gated mesh): vf16 = the P3
        // row, vf18 = the strip GIFtag template, vf27 = the last vertex's stp
        // chain; vf28..30 are not written by this emitter loop.
        if (gr != nullptr) {
            for (int k = 0; k < 4; ++k) {
                gr->vf16.b[k] = p3.b[k];
                gr->vf18.b[k] = m.W(top, k);
                gr->vf27.b[k] = end_stp.b[k];
            }
        }
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0xFFFF;  // IADD vi13
        if (mesh == mesh_end) return true;                  // IBNE
    }
    return false;  // runaway mesh list
}

bool CrysPrograms::RunDual(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    // Dispatcher prologue beacon (shared family prefix).
    m.SetW(9, 0, 1);

    u32 node = m.W(3, 0) & 0xFFFF;
    // ONE GateRegs threads through the whole dispatch walk -- the
    // gate-persistent register file is shared VU state across both subs.
    const bool crys_clip_ok = (Vu1LoadedPrograms::At(in, kEnvEmitterPc) == Vu1LoadedPrograms::kType10 &&
                               Vu1LoadedPrograms::At(in, kLitEmitterPc) == Vu1LoadedPrograms::kType02) ||
                              in.image_hash == kClipImage;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kEnvFanPc, crys_clip_ok);
    gr.fan_kind = kEnvFanKind;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;  // E-bit exit node
        if (call_pc == kEnvEmitterPc) {
            gr.fan_pc = kEnvFanPc;
            gr.fan_kind = kEnvFanKind;
            if (!RunCrysEnv1720(m, node, in.top, out, &gr)) return false;
        } else if (call_pc == kLitEmitterPc) {
            gr.fan_pc = kLitFanPc;
            gr.fan_kind = kLitFanKind;
            if (!LitTristrip::RunLitCall(m, node, in.top, out, kLitGateLink, &gr)) return false;
        } else {
            return false;  // other material subs: decline
        }
        const u32 next = m.W(node, 2) & 0xFFFF;
        if (next == node) return false;
        node = next;
    }

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}

bool CrysPrograms::RunDisplace(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc == kScaleEntryPc) {
        // The direct scale-phase entry: shared family prefix code, the skn
        // contract (zero kicks, data writes, no regs).
        VuChopRoundGuard chop_guard;
        std::vector<u8>& s_mem = WorkingCopy(in);
        QwPtr m{s_mem.data()};
        if (!SkinnedLitStrip::RunScalePhase(m, in.top, nullptr)) return false;
        EmitDataWrites(s_mem.data(), in, out);
        return true;
    }
    if (in.start_pc == DisplacePrefix::kIntEntryPc) return DisplacePrefix::RunDisplaceInt(in, out);
    if (in.start_pc == DisplacePrefix::kFloatEntryPc) return DisplacePrefix::RunDisplaceFloat(in, out);
    return false;
}
