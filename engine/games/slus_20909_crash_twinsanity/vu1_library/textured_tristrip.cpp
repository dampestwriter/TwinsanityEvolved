// TexturedTristrip -- vs_textured_tristrip (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/textured_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;   // the material sub (= the variant base)
constexpr u32 kEnvPc = 0x1e90;       // the pinned image's env-map second sub
constexpr u32 kExitPc = 0x90;        // the E-bit exit node
constexpr u32 kGateHelperPc = 0xB0;  // the shared clip gate (QW4.y * 8)
constexpr u32 kGateLink = 0x2F9;     // (gate JALR pc + 16) / 8 of the emitter body
constexpr u32 kEnvGateLink = 0x3E8;  // the env body's gate link
constexpr u32 kFanPc = 0x1b90;       // the textured fan emitter entry
constexpr u64 kEnvSubImage = 0xe265d357d52464ccull;  // the only image with the env sub at kEnvPc
// Images whose fan region is byte-identical to the one the near-clip lift was
// coded from (full clips lift only for these).
constexpr u64 kClipImages[] = {
    0xe265d357d52464ccull, 0x9c5fd7c3af220d42ull, 0xba0209295d717c28ull, 0x904ca8fcbd6df8a4ull,
    0x5049b51d66a449c4ull, 0xab3a9229dcf50a5full, 0x099701abe8c6ea16ull, 0x32331b59a49fe0bcull,
    0x6fe571db9172b0dfull, 0xe8e10ae990c3258full,
};

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

}  // namespace

bool TexturedTristrip::RunTexCall(QwPtr m, u32 node, u32 top, ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto load = [&](u32 qw) { return Load(m, qw); };
    auto store = [&](u32 qw, const V4& v) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, v.b[k]);
    };
    auto store_xyz = [&](u32 qw, const V4& v) {
        for (int k = 0; k < 3; ++k) m.SetW(qw, k, v.b[k]);
    };
    auto add_xyz = [&](const V4& a, const V4& b, u32 wbits) {  // ADD.xyz
        V4 r;
        for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(a.b[k])) + CF(bits2f(b.b[k]))));
        r.b[3] = wbits;
        return r;
    };
    auto mulq_xyz = [&](const V4& v, float q, u32 wbits) {  // MULq.xyz
        V4 r;
        for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(v.b[k])) * q));
        r.b[3] = wbits;
        return r;
    };
    auto ftoi_xyz = [&](const V4& v, u32 wbits) {  // FTOI0.xyz
        V4 r;
        for (int k = 0; k < 3; ++k) r.b[k] = Ftoi0(v.b[k]);
        r.b[3] = wbits;
        return r;
    };

    const V4 payload = load(node + 2);         // LQ vf05, 2(vi14)
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)

    u32 mesh = m.W(2, 0) & 0xFFFF;               // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;  // ILW.y vi10, 0(vi13) (once)

    for (int mesh_guard = 0; mesh_guard < 64; ++mesh_guard) {
        const u32 gate_flag = m.W(mesh, 0) & 0xFFFF;  // ILW.x vi11, 0(vi13)
        const u32 clip_off = m.W(mesh, 2) & 0xFFFF;   // ILW.z vi03, 0(vi13)
        V4 obj[4];
        for (int r = 0; r < 4; ++r) obj[r] = load(mesh + 1 + r);
        gr.vi09 = (u16)mesh_end;  // body pins at gate time
        gr.vi13 = (u16)mesh;
        for (int r = 0; r < 4; ++r) gr.obj[r] = obj[r];
        auto xform = [&](const V4& v) {
            return XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(v.b[0])), CF(bits2f(v.b[1])),
                             CF(bits2f(v.b[2])));
        };

        // Header (both paths): count = low16 of QW[top+1].x via MR32+MTIR.
        const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;
        if ((vqw_count & 3) != 0) return false;
        const u32 nverts = vqw_count / 4;
        const u32 vbase = top + 3;
        const u32 vend = vbase + vqw_count;
        u32 out_base = 0;

        if (gate_flag != 0) {
            // ---- gated path: clip gate then the emitter --------------------
            const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
            if (helper_pc != kGateHelperPc) return false;    // unexpected prefix
            V4 clipR[5];
            for (int r = 0; r < 5; ++r) clipR[r] = load(mesh + clip_off + r);
            if (!ClipGate::ClipGateB0(m, node, top, kGateLink, clipR, nverts, &gr, &out)) return false;
            if (nverts < 2) return false;  // micro loop shape needs >= 2

            // Post-gate header reads + GIFtag template write.
            out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
            for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));

            // Prologue.
            V4 c0 = load(vbase + 1);   // vf08
            V4 p0 = load(vbase + 0);   // vf06
            V4 c_g = load(vbase + 5);  // vf07 = color1
            store(out_base + 2, c0);   // RGBAQ v0 (old vf09)
            V4 T_prev = xform(p0);     // vf09 = T(v0)
            // vf08 = {c0.xyz+payload, p0.w}
            V4 stq_in = add_xyz(c0, payload, p0.b[3]);
            float Q = VuDiv(1.0f, bits2f(T_prev.b[3]));  // Q0
            V4 p1 = load(vbase + 4);                     // vf06 = pos1
            store(out_base + 5, c_g);                    // RGBAQ v1 (old vf09)
            V4 T_g = xform(p1);                          // vf09 = T(v1)
            // vf06 = {T(v0).xyz*Q0, p1.w}
            V4 scr = mulq_xyz(T_prev, Q, p1.b[3]);
            // vf10 = STQ(v0) = (c0+payload)*Q0
            V4 stq = mulq_xyz(stq_in, Q, 0);
            u32 vi06 = vbase + 4;  // v1
            u32 vi11 = out_base + 1;

            // Loop g = 1..N-1; iteration g stores v_{g-1} and RGBAQ(v_{g+1}).
            for (u32 loop_guard = 0;; ++loop_guard) {
                if (loop_guard > 1024) return false;  // runaway (bad count)
                const u32 adc_prev = stq_in.b[3];     // vf08.w = p_{g-1}.w
                V4 stq_in_cur = add_xyz(c_g, payload, adc_prev);
                V4 c_next = load(vi06 + 5);
                V4 xyz_out = ftoi_xyz(scr, adc_prev);  // vf12
                stq_in_cur.b[3] = scr.b[3];            // vf08.w = p_g.w
                V4 p_next = load(vi06 + 4);
                const float Qg = VuDiv(1.0f, bits2f(T_g.b[3]));
                vi11 += 3;
                store_xyz(vi11 - 3, stq);            // STQ v_{g-1}
                store(vi11 - 1, xyz_out);            // XYZ2 v_{g-1}
                vi06 += 4;
                store(vi11 + 4, c_next);             // RGBAQ v_{g+1}
                V4 T_next = xform(p_next);
                scr = mulq_xyz(T_g, Qg, p_next.b[3]);  // vf11 = T_g.xyz
                stq = mulq_xyz(stq_in_cur, Qg, 0);     // (delay)
                stq_in = stq_in_cur;
                c_g = c_next;
                T_g = T_next;
                if (vi06 == vend) break;  // IBNE
            }

            // Epilogue.
            scr.b[3] = stq_in.b[3];                 // MOVE.w (= p_{N-1}.w)
            V4 last_xyz = ftoi_xyz(scr, scr.b[3]);
            vi11 += 3;
            store_xyz(vi11 - 3, stq);       // STQ v_{N-1}
            store(vi11 - 1, last_xyz);      // XYZ2 v_{N-1}
        } else {
            // ---- no-gate path ------------------------------------------------
            if (nverts < 3) return false;  // micro loop shape needs >= 3

            // Header reads + GIFtag template write.
            out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
            for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));

            // Prologue.
            u32 vi06 = vbase;
            V4 p0 = load(vi06);      // vf08 (.xyz used)
            V4 c0 = load(vi06 + 1);  // vf06
            vi06 += 4;
            V4 T_prev = xform(p0);   // vf08 = T(v0)
            V4 p1 = load(vi06);      // vf09 (.xyz used)
            float Q_arch = VuDiv(1.0f, bits2f(T_prev.b[3]));  // DIV Q0
            V4 c_k = load(vi06 + 1);                          // vf08 = color1
            V4 T_k = xform(p1);                               // vf09 = T(v1)
            store(out_base + 2, c0);                          // RGBAQ v0 (old vf06)
            V4 st_in0 = add_xyz(c0, payload, c0.b[3]);        // vf06 = {c0+pay, c0.w}
            // MULq|DIV: the fdiv-pipe lower commits pending Q0 pre-exec.
            V4 scrf = mulq_xyz(T_prev, Q_arch, 0);            // vf11 = screen0 floats
            float Q_pend = VuDiv(1.0f, bits2f(T_k.b[3]));     // DIV Q1
            // 3 bundles after the DIV -> reads the ARCHITECTURAL Q0.
            V4 stq = mulq_xyz(st_in0, Q_arch, 0);   // vf10 = STQ(v0)
            V4 xyzq = ftoi_xyz(scrf, c0.b[3]);      // (vf06, .w = c0.w)
            vi06 += 4;                              // v2
            store(out_base + 5, c_k);               // RGBAQ v1 (old vf08)
            V4 st_in = add_xyz(c_k, payload, 0);    // vf07 = c1+pay
            Q_arch = Q_pend;                        // retires >= 7 before next read
            u32 vi11 = out_base + 1;

            // Loop k = 1..N-2; iteration k stores v_{k-1} and RGBAQ(v_{k+1}).
            for (u32 loop_guard = 0;; ++loop_guard) {
                if (loop_guard > 1024) return false;
                V4 p_next = load(vi06);        // (.xyz)
                V4 out_xyz = xyzq;             // MOVE vf12 (old vf06)
                const u32 adc_k = c_k.b[3];    // vf06.w = c_k.w
                V4 c_next = load(vi06 + 1);
                vi11 += 3;
                store_xyz(vi11 - 3, stq);      // STQ v_{k-1}
                store(vi11 - 1, out_xyz);      // XYZ2 v_{k-1}
                V4 T_next = xform(p_next);
                store(vi11 + 4, c_next);       // RGBAQ v_{k+1}
                V4 scrq = mulq_xyz(T_k, Q_arch, 0);  // (>=7 after DIV)
                stq = mulq_xyz(st_in, Q_arch, 0);    // STQ(v_k)
                vi06 += 4;
                st_in = add_xyz(c_next, payload, 0);  // (vf07)
                xyzq = ftoi_xyz(scrq, adc_k);         // (vf06, .w = c_k.w)
                Q_pend = VuDiv(1.0f, bits2f(T_next.b[3]));  // (delay, always)
                c_k = c_next;
                T_k = T_next;
                if (vi06 == vend) break;  // IBNE
                Q_arch = Q_pend;          // natural retire (8 bundles)
            }

            // Epilogue. WAITQ commits the pending quotient before its
            // same-bundle MULq executes.
            Q_arch = Q_pend;                             // WAITQ flush
            V4 scrlast = mulq_xyz(T_k, Q_arch, 0);       // (vf09)
            vi11 += 3;
            V4 stq_last = mulq_xyz(st_in, Q_arch, 0);    // (vf10 new)
            store_xyz(vi11 - 3, stq);                    // STQ v_{N-2} (old vf10)
            store(vi11 - 1, xyzq);                       // XYZ2 v_{N-2}
            V4 xyz_last = ftoi_xyz(scrlast, c_k.b[3]);   // (.w = c_{N-1}.w)
            vi11 += 3;
            store_xyz(vi11 - 3, stq_last);  // STQ v_{N-1}
            store(vi11 - 1, xyz_last);      // (delay, always)
        }

        // Kicks: node-embedded state packet first (node.w != 0), then main.
        if (node_w != 0) {
            if (!KickAt(m, node + 3, out)) return false;
        }
        if (!KickAt(m, out_base, out)) return false;

        // Tail: QW1 MR32 rotation + mesh advance.
        RotateQw1(m);
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0xFFFF;
        if (mesh == mesh_end) return true;
    }
    return false;  // runaway mesh list
}

// The env-map second material sub: per vertex eye vector / VU fast inverse
// square root / half vector / env rows / sphere-map ST clamped [0,1] raw-bit /
// normal-matrix lighting modulating static RGBA ints. The post-gate light
// matrix loads overwrite the gate-persistent vf27/28/29.
bool TexturedTristrip::RunEnvCall(QwPtr m, u32 node, u32 top, ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const V4 payload = load(node + 2);         // LQ vf12, 2(vi14)
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)

    // Invsqrt constant register vf07 (built from I-immediates each mesh).
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
        gr.vi09 = (u16)mesh_end;  // body pins at gate time
        gr.vi13 = (u16)mesh;
        for (int r = 0; r < 4; ++r) gr.obj[r] = obj[r];

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
            if (!ClipGate::ClipGateB0(m, node, top, kEnvGateLink, clipR, nverts, &gr, &out)) return false;
        }

        // Post-gate constants.
        const u32 out_base = m.W(1, 0) & 0xFFFF;    // ILW.x vi11, 1(vi00)
        const V4 row5 = load(mesh + clip_off + 5);  // LQ vf06, 5(vi03)
        V4 envR[3];
        for (int r = 0; r < 3; ++r) envR[r] = load(mesh + clip_off + r);
        V4 nrmR[3];
        for (int r = 0; r < 3; ++r) nrmR[r] = load(mesh + 5 + r);
        V4 litR[4];
        for (int r = 0; r < 4; ++r) litR[r] = load(mesh + 9 + r);
        gr.vf27 = litR[1];  // post-gate row loads clobber the persistent
        gr.vf28 = litR[2];  // vf27/28/29 (vf26 = litR[0] is dead: the next
        gr.vf29 = litR[3];  // gate call rewrites vf23-26 before any spill)
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
                acc = CF(VuFloat::Fma(CF(bits2f(nrmR[1].b[k])), ny, acc));
                wn[k] = CF(VuFloat::Fma(CF(bits2f(nrmR[2].b[k])), nz, acc));
            }

            // h = eye*rsq + n_in; h2 = h*0.5; env = envR * h2 (MULAy/MADDAz/MADDx).
            float h2[3];
            for (int k = 0; k < 3; ++k) {
                const float hn = CF(CF(e[k]) * CF(bits2f(rsq_e)));
                const float h = CF(CF(hn) + CF(bits2f(nrm.b[k])));
                h2[k] = CF(CF(h) * 0.5f);
            }
            float env[3];
            for (int k = 0; k < 3; ++k) {
                float acc = CF(CF(bits2f(envR[1].b[k])) * h2[1]);
                acc = CF(VuFloat::Fma(CF(bits2f(envR[2].b[k])), h2[2], acc));
                env[k] = CF(VuFloat::Fma(CF(bits2f(envR[0].b[k])), h2[0], acc));
            }
            float ensq[3];
            for (int k = 0; k < 3; ++k) ensq[k] = CF(CF(env[k]) * CF(env[k]));
            const float d2v = CF(CF(CF(CF(ensq[0]) + CF(ensq[2]))) + CF(ensq[1]));
            const u32 rsq_v = Rsq(f2bits(d2v), c7);

            // Sphere-map ST: envn.xy = env.xy * rsq_v; u = 0.5*envn.x + 0.5,
            // v = -0.5*envn.y + 0.5, q = 1.0; clamp [0,1] raw-bit; + payload.
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
                    acc = CF(VuFloat::Fma(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
                    acc = CF(VuFloat::Fma(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
                    lit[k] = CF(VuFloat::Fma(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
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
        if (mesh == mesh_end) return true;
    }
    return false;
}

bool TexturedTristrip::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    // Dispatcher prologue: ISW.x vi12(=1), 9(vi00) -- the "program ran"
    // beacon the EE polls, written once per MSCAL before the node walk.
    m.SetW(9, 0, 1);

    // The near-clip lift is identity-verified for the body + fan the lift was
    // coded from: the shader-type-01 program at the emitter pc (the layout
    // analysis), else one of the pinned images (no analysis attached).
    bool clip_image_ok = Vu1LoadedPrograms::At(in, kEmitterPc) == Vu1LoadedPrograms::kType01;
    for (u64 h : kClipImages) clip_image_ok = clip_image_ok || in.image_hash == h;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kFanPc, clip_image_ok);

    u32 node = m.W(3, 0) & 0xFFFF;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;  // E-bit exit node
        if (call_pc == kEmitterPc) {
            if (!RunTexCall(m, node, in.top, gr, out)) return false;
        } else if (call_pc == kEnvPc && (Vu1LoadedPrograms::At(in, kEnvPc) == Vu1LoadedPrograms::kType0C ||
                                         in.image_hash == kEnvSubImage)) {
            // The env-map second material sub (the shader-type-0C program
            // loaded right after type 01); other layouts place a different sub there.
            if (!RunEnvCall(m, node, in.top, gr, out)) return false;
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
