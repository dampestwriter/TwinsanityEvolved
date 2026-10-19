// LitTristrip -- vs_lit_tristrip (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/lit_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;    // the material sub (= the variant base)
constexpr u32 kEnv2Pc = 0x1e40;       // the pinned image's env-map second sub
constexpr u32 kExitPc = 0x90;         // the E-bit exit node
constexpr u32 kGateHelperPc = 0xB0;   // the shared clip gate (QW4.y * 8)
constexpr u32 kEnv2GateLink = 0x3DE;  // the env body's gate link
constexpr u32 kFanPc = 0x1b98;        // the lit fan emitter entry
constexpr int kFanKind = 1;           // the lit fan body
constexpr u64 kEnv2SubImage = 0x8d8dc807f8af5673ull;  // the only image with the env sub at kEnv2Pc
// Images whose body and fan are byte-identical to the one the near-clip lift
// was coded from (full clips lift only for these).
constexpr u64 kClipImages[] = {0x59047cf3c9f7b0daull, 0xb9e236eb3a7b8a93ull};

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

}  // namespace

bool LitTristrip::RunLitCall(QwPtr m, u32 node, u32 top, Vu1HleOutput& out, u32 gate_link,
                                    ClipGate::GateRegs* gr) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const V4 payload = load(node + 2);         // LQ vf12, 2(vi14)
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)

    u32 mesh = m.W(2, 0) & 0xFFFF;               // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;  // ILW.y vi10, 0(vi13) (once)

    // Data-driven termination (IBNE mesh == mesh_end below); safety bound only.
    for (int mesh_guard = 0; mesh_guard < kMeshRingBound; ++mesh_guard) {
        const u32 gate_flag = m.W(mesh, 0) & 0xFFFF;  // ILW.x vi11/vi01
        const u32 clip_off = m.W(mesh, 2) & 0xFFFF;   // ILW.z vi03
        V4 obj[4];
        for (int r = 0; r < 4; ++r) obj[r] = load(mesh + 1 + r);

        const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // via MR32+MTIR vf14
        if ((vqw_count & 3) != 0) return false;
        const u32 nverts = vqw_count / 4;
        if (nverts < 3) return false;  // micro pipeline needs >= 3
        const u32 vbase = top + 3;

        if (gate_flag != 0) {
            const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
            if (helper_pc != kGateHelperPc) return false;
            V4 clipR[5];
            for (int r = 0; r < 5; ++r) clipR[r] = load(mesh + clip_off + r);
            if (gr != nullptr) {  // body pins at gate time
                gr->vi09 = (u16)mesh_end;
                gr->vi13 = (u16)mesh;
                for (int r = 0; r < 4; ++r) gr->obj[r] = obj[r];
            }
            if (!ClipGate::ClipGateB0(m, node, top, gate_link, clipR, nverts, gr, gr != nullptr ? &out : nullptr))
                return false;
        }

        // Post-gate constants.
        const u32 out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
        V4 nrmR[3];
        for (int r = 0; r < 3; ++r) nrmR[r] = load(mesh + 5 + r);  // vf05..07
        V4 litR[4];
        for (int r = 0; r < 4; ++r) litR[r] = load(mesh + 9 + r);       // vf08..11
        for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));  // GIFtag

        // Loop-end register capture for the mesh-2+ near-clipper spill:
        //   vf16 = {(stuv(n-1)+payload)*Q(n-1) .xyz (= the final ST store
        //     bytes), stuv(n-1).w raw};
        //   vf18 = {wn(n-1) PRE-CLAMP .xyz, Fmin_raw(ITOF0(color(v1).w), 255.0)
        //     .w (the prologue MINIi on v1's colour; the loop never writes vf18.w)}.
        u32 end_vf16[4] = {0, 0, 0, 0}, end_vf18[4] = {0, 0, 0, 0};
        if (gr != nullptr && nverts >= 2) {
            const float a1 = (float)(int32_t)(m.W(vbase + 7, 3));  // ITOF0
            end_vf18[3] = Fmin_raw(f2bits(CF(a1)), f2bits(255.0f));
        }

        for (u32 j = 0; j < nverts; ++j) {
            const u32 vq = vbase + j * 4;
            const V4 pos = load(vq);         // LQ 0(vi06)
            const V4 stuv = load(vq + 1);    // LQ vf16, 1(vi06) (.w = aux)
            const V4 nrm = load(vq + 2);     // LQ.xyz 2(vi06)
            const V4 st_col = load(vq + 3);  // LQ -1(vi06): static RGBA ints

            // T = ObjMtx * pos; Q = 1/T.w (own quotient per vertex).
            const V4 T = XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                                   CF(bits2f(pos.b[2])));
            const float Q = VuDiv(1.0f, bits2f(T.b[3]));

            // World normal wn = NrmMtx * n (MULAx/MADDAy/MADDz, fused).
            const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
            float wn[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
                acc = CF(std::fmaf(CF(bits2f(nrmR[1].b[k])), ny, acc));
                wn[k] = CF(std::fmaf(CF(bits2f(nrmR[2].b[k])), nz, acc));
            }

            // Lighting -> RGBA (raw-bit MAX vs +0; MULAw/MADDAx/MADDAy/MADDz).
            u32 rgba[4];
            {
                const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
                const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
                const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
                float lit[3];
                for (int k = 0; k < 3; ++k) {
                    float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
                    acc = CF(std::fmaf(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
                    acc = CF(std::fmaf(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
                    lit[k] = CF(std::fmaf(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
                }
                for (int k = 0; k < 3; ++k) {
                    const float s = (float)(int32_t)st_col.b[k];  // ITOF0
                    const float mod = CF(CF(lit[k]) * CF(s));     // MUL.xyz
                    rgba[k] = Ftoi0(Fmin_raw(f2bits(mod), f2bits(255.0f)));
                }
                const float sa = (float)(int32_t)st_col.b[3];
                rgba[3] = Ftoi0(Fmin_raw(f2bits(sa), f2bits(255.0f)));
            }

            // ADC: sext16((low16(pos.w) & flag) | low16(aux.w)).
            const u32 adc16 = ((pos.b[3] & 0xFFFF) & gate_flag) | (stuv.b[3] & 0xFFFF);
            const u32 adc = (adc16 & 0x8000) ? (0xFFFF0000u | adc16) : adc16;

            // Stores: ST_j (xyz only -- .w VIF-prefilled), RGBA_j, XYZ2_j.
            const u32 st_qw = out_base + 1 + 3 * j;
            for (int k = 0; k < 3; ++k) {
                const float stp = CF(CF(bits2f(stuv.b[k])) + CF(bits2f(payload.b[k])));
                m.SetW(st_qw, k, f2bits(CF(CF(stp) * Q)));
            }
            for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, rgba[k]);
            m.SetW(st_qw + 2, 0, Ftoi0(f2bits(CF(CF(bits2f(T.b[0])) * Q))));
            m.SetW(st_qw + 2, 1, Ftoi0(f2bits(CF(CF(bits2f(T.b[1])) * Q))));
            m.SetW(st_qw + 2, 2, Ftoi0(f2bits(CF(CF(bits2f(T.b[2])) * Q))));
            m.SetW(st_qw + 2, 3, adc);
            if (gr != nullptr && j == nverts - 1) {  // loop-end capture
                for (int k = 0; k < 3; ++k) {
                    end_vf16[k] = m.W(st_qw, k);  // the final ST bytes
                    end_vf18[k] = f2bits(wn[k]);  // wn(n-1) PRE-clamp
                }
                end_vf16[3] = stuv.b[3];  // stuv(n-1).w raw
            }
        }

        // Kicks: node packet first when node.w != 0.
        if (node_w != 0) {
            if (!KickAt(m, node + 3, out)) return false;
        }
        if (!KickAt(m, out_base, out)) return false;

        RotateQw1(m);
        // Re-seed the pass-through spill registers with the vertex loop's end
        // state; nverts < 2 shapes are corpus-unobserved -- decline honestly.
        if (gr != nullptr) {
            if (nverts >= 2) {
                for (int k = 0; k < 4; ++k) {
                    gr->vf16.b[k] = end_vf16[k];
                    gr->vf18.b[k] = end_vf18[k];
                }
            } else {
                gr->valid = false;
            }
        }
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0xFFFF;  // IADD vi13
        if (mesh == mesh_end) return true;                  // IBNE
    }
    return false;  // runaway mesh list
}

bool LitTristrip::RunEnv2Call(QwPtr m, u32 node, u32 top, Vu1HleOutput& out) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const V4 payload = load(node + 2);         // LQ vf12, 2(vi14)
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)

    // Invsqrt constant register vf07 (I-immediates; input-independent).
    V4 c7;
    {
        const u32 kMagic = 0x4e7e0000u;
        const float mag = CF(0.0f + CF(bits2f(kMagic)));  // ADDi.xyz
        const float mz = CF(mag * CF(-0.5f));             // MULi.z
        c7.b[0] = f2bits(CF(mag - mz));                   // SUBz.x
        c7.b[1] = f2bits(mag);
        c7.b[2] = f2bits(mz);
        c7.b[3] = f2bits(CF(CF(1.0f) * CF(-0.5f)));  // MULi.w vf00.w*I
    }

    u32 mesh = m.W(2, 0) & 0xFFFF;               // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;  // ILW.y vi10, 0(vi13) (once)

    for (int mesh_guard = 0; mesh_guard < 64; ++mesh_guard) {
        const u32 gate_flag = m.W(mesh, 0) & 0xFFFF;  // ILW.x vi11/vi01
        const u32 clip_off = m.W(mesh, 2) & 0xFFFF;   // ILW.z vi03
        V4 obj[4];
        for (int r = 0; r < 4; ++r) obj[r] = load(mesh + 1 + r);

        const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // via MR32+MTIR vf14
        if ((vqw_count & 3) != 0) return false;
        const u32 nverts = vqw_count / 4;
        if (nverts < 3) return false;  // micro pipeline needs >= 3
        const u32 vbase = top + 3;

        if (gate_flag != 0) {
            const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;
            if (helper_pc != kGateHelperPc) return false;
            V4 clipR[5];
            for (int r = 0; r < 5; ++r) clipR[r] = load(mesh + clip_off + r);
            if (!ClipGate::ClipGateB0(m, node, top, kEnv2GateLink, clipR, nverts)) return false;
        }

        // Post-gate constants.
        const u32 out_base = m.W(1, 0) & 0xFFFF;    // ILW.x vi11, 1(vi00)
        const V4 row5 = load(mesh + clip_off + 5);  // LQ vf06, 5(vi03)
        V4 envR[3];
        for (int r = 0; r < 3; ++r) envR[r] = load(mesh + clip_off + r);
        V4 nrmR[3];
        for (int r = 0; r < 3; ++r) nrmR[r] = load(mesh + 5 + r);  // vf23..25
        V4 litR[4];
        for (int r = 0; r < 4; ++r) litR[r] = load(mesh + 9 + r);       // vf26..29
        for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));  // GIFtag

        for (u32 j = 0; j < nverts; ++j) {
            const u32 vq = vbase + j * 4;
            const V4 pos = load(vq);
            const u32 aux_w = m.W(vq + 1, 3);  // LQ.w vf06, 1(vi06)
            const V4 nrm = load(vq + 2);
            const V4 st_col = load(vq + 3);    // static RGBA ints

            // eye = row5.xyz - pos.xyz; d2e = (ex2 + ez2) + ey2.
            float e[3], esq[3];
            for (int k = 0; k < 3; ++k) e[k] = CF(CF(bits2f(row5.b[k])) - CF(bits2f(pos.b[k])));
            for (int k = 0; k < 3; ++k) esq[k] = CF(CF(e[k]) * CF(e[k]));
            const float d2e = CF(CF(CF(CF(esq[0]) + CF(esq[2]))) + CF(esq[1]));
            const u32 rsq_e = Rsq(f2bits(d2e), c7);

            // World normal wn = nrmR * n (MULAx/MADDAy/MADDz, fused).
            const float nx = CF(bits2f(nrm.b[0])), ny = CF(bits2f(nrm.b[1])), nz = CF(bits2f(nrm.b[2]));
            float wn[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
                acc = CF(std::fmaf(CF(bits2f(nrmR[1].b[k])), ny, acc));
                wn[k] = CF(std::fmaf(CF(bits2f(nrmR[2].b[k])), nz, acc));
            }

            // h = eye*rsq + n; h2 = h*0.5; env = envR * h2 (MULAy/MADDAz/MADDx).
            float h2[3];
            for (int k = 0; k < 3; ++k) {
                const float hn = CF(CF(e[k]) * CF(bits2f(rsq_e)));
                const float h = CF(CF(hn) + CF(bits2f(nrm.b[k])));
                h2[k] = CF(CF(h) * 0.5f);
            }
            float env[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(envR[1].b[k])) * h2[1]);
                acc = CF(std::fmaf(CF(bits2f(envR[2].b[k])), h2[2], acc));
                env[k] = CF(std::fmaf(CF(bits2f(envR[0].b[k])), h2[0], acc));
            }
            float ensq[3];
            for (int k = 0; k < 3; ++k) ensq[k] = CF(CF(env[k]) * CF(env[k]));
            const float d2v = CF(CF(CF(CF(ensq[0]) + CF(ensq[2]))) + CF(ensq[1]));
            const u32 rsq_v = Rsq(f2bits(d2v), c7);

            // Sphere-map ST: u = 0.5*envn.x + 0.5, v = -0.5*envn.y + 0.5,
            // q = 1.0; clamp [0,1] raw-bit; + payload; * Q.
            const float half = CF(0.0f + CF(0.5f));  // ADDi lane
            const float envx = CF(CF(env[0]) * CF(bits2f(rsq_v)));
            const float envy = CF(CF(env[1]) * CF(bits2f(rsq_v)));
            float st[3];
            st[0] = CF(CF(CF(CF(envx) * half)) + half);  // MULz.x/ADDz
            st[1] = CF(CF(CF(CF(envy) * CF(bits2f(c7.b[3])))) + half);
            st[2] = CF(0.0f + CF(1.0f));  // ADDw.z
            u32 uv[3];
            for (int k = 0; k < 3; ++k) {
                const u32 lo = Fmin_raw(f2bits(st[k]), f2bits(1.0f));
                uv[k] = Fmax_raw(lo, 0u);  // vf00.x = +0
            }

            // Position transform + this vertex's quotient.
            const V4 T = XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                                   CF(bits2f(pos.b[2])));
            const float Q = VuDiv(1.0f, bits2f(T.b[3]));

            // ADC: sext16((low16(pos.w) & flag) | low16(aux.w)).
            const u32 adc16 = ((pos.b[3] & 0xFFFF) & gate_flag) | (aux_w & 0xFFFF);
            const u32 adc = (adc16 & 0x8000) ? (0xFFFF0000u | adc16) : adc16;

            // Lighting -> RGBA (raw-bit MAX vs +0; MULAw/MADDAx/MADDAy/MADDz).
            u32 rgba[4];
            {
                float lit[3];
                const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
                const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
                const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
                for (int k = 0; k < 3; ++k) {
                    float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
                    acc = CF(std::fmaf(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
                    acc = CF(std::fmaf(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
                    lit[k] = CF(std::fmaf(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
                }
                for (int k = 0; k < 3; ++k) {
                    const float s = (float)(int32_t)st_col.b[k];  // ITOF0
                    const float mod = CF(CF(lit[k]) * CF(s));     // MUL.xyz
                    rgba[k] = Ftoi0(Fmin_raw(f2bits(mod), f2bits(255.0f)));
                }
                const float sa = (float)(int32_t)st_col.b[3];
                rgba[3] = Ftoi0(Fmin_raw(f2bits(sa), f2bits(255.0f)));
            }

            // Stores: ST_j (xyz only), RGBA_j, XYZ2_j.
            const u32 st_qw = out_base + 1 + 3 * j;
            for (int k = 0; k < 3; ++k) {
                const float stp = CF(CF(bits2f(uv[k])) + CF(bits2f(payload.b[k])));
                m.SetW(st_qw, k, f2bits(CF(CF(stp) * Q)));
            }
            for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, rgba[k]);
            m.SetW(st_qw + 2, 0, Ftoi0(f2bits(CF(CF(bits2f(T.b[0])) * Q))));
            m.SetW(st_qw + 2, 1, Ftoi0(f2bits(CF(CF(bits2f(T.b[1])) * Q))));
            m.SetW(st_qw + 2, 2, Ftoi0(f2bits(CF(CF(bits2f(T.b[2])) * Q))));
            m.SetW(st_qw + 2, 3, adc);
        }

        if (node_w != 0) {
            if (!KickAt(m, node + 3, out)) return false;
        }
        if (!KickAt(m, out_base, out)) return false;

        RotateQw1(m);
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0xFFFF;
        if (mesh == mesh_end) return true;  // IBNE
    }
    return false;
}

bool LitTristrip::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    // Dispatcher prologue beacon (shared family prefix).
    m.SetW(9, 0, 1);

    // Identity-verified for the shader-type-02 program at the emitter pc (the
    // layout analysis), else one of the pinned images (no analysis attached).
    bool clip_image_ok = Vu1LoadedPrograms::At(in, kEmitterPc) == Vu1LoadedPrograms::kType02;
    for (u64 h : kClipImages) clip_image_ok = clip_image_ok || in.image_hash == h;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kFanPc, clip_image_ok);
    gr.fan_kind = kFanKind;

    u32 node = m.W(3, 0) & 0xFFFF;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;  // E-bit exit node
        if (call_pc == kEmitterPc) {
            if (!RunLitCall(m, node, in.top, out, kGateLink, &gr)) return false;
        } else if (call_pc == kEnv2Pc && (Vu1LoadedPrograms::At(in, kEnv2Pc) == Vu1LoadedPrograms::kType0C ||
                                          in.image_hash == kEnv2SubImage)) {
            // The env-map second material sub (the shader-type-0C program
            // loaded right after type 02); other layouts place different subs there.
            if (!RunEnv2Call(m, node, in.top, out)) return false;
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
