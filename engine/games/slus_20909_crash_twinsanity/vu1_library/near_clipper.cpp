// NearClipper -- the family 0x440 clipper + the four fan emitters (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/near_clipper.h"

#include <cmath>
#include "engine/vu/vu_float.h"

using namespace vu1lib;

namespace {

V4 LoadQ(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}
void StoreQ(QwPtr m, u32 qw, const V4& v) {
    for (int k = 0; k < 4; ++k) m.SetW(qw, k, v.b[k]);
}

}  // namespace

// Register spill + triangle init + six Sutherland-Hodgman passes + fan dispatch.
bool NearClipper::Run(QwPtr m, u32 node, u32 top, const V4 clipR[5], u32 j, u32 nverts, const u32 s_hist[3],
                      const u32 g_hist[3], const ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    const u32 vbase = top + 3;
    const u32 vend = vbase + 4 * nverts;
    const u32 vi06 = vbase + 4 * (j + 1);

    auto loadq = [&](u32 qw) { return LoadQ(m, qw); };
    auto storeq = [&](u32 qw, const V4& v) { StoreQ(m, qw, v); };
    auto posq = [&](u32 v) { return loadq(vbase + 4 * v); };
    auto colq = [&](u32 v) { return loadq(vbase + 4 * v + 1); };
    auto Pof = [&](u32 v) {
        const u32 vq = vbase + 4 * v;
        return XformClip(clipR[0], clipR[1], clipR[2], clipR[3], CF(m.F(vq, 0)), CF(m.F(vq, 1)), CF(m.F(vq, 2)));
    };
    auto Gof = [&](u32 v) { return GuardScale(clipR[4], Pof(v)); };

    // ---- register spill: window-invariant vf values (gate prologue) --------
    {
        const V4 col0 = colq(0), col1 = colq(1);
        const V4 pos0 = posq(0), pos1 = posq(1);
        V4 vf06r = pos1;
        vf06r.b[3] = col1.b[3];  // {pos(v1).xyz, col(v1).w}
        V4 vf13r = pos0;
        vf13r.b[3] = col0.b[3];  // {pos(v0).xyz, col(v0).w}
        V4 vf07r = {{m.W(top + 1, 1), m.W(top + 1, 2), m.W(top + 1, 3), m.W(top + 1, 0)}};  // MR32(QW[top+1])
        storeq(14, col1);   // vf05
        storeq(15, vf06r);  // vf06
        storeq(16, vf07r);  // vf07
        for (int r = 0; r < 5; ++r) storeq(17 + r, clipR[r]);  // vf08..vf12
        storeq(22, vf13r);    // vf13
        storeq(23, col0);     // vf14
        storeq(24, Pof(1));   // vf15
        storeq(25, gr.vf16);  // vf16 (pass-through)
        storeq(26, Pof(0));   // vf17
        storeq(27, gr.vf18);  // vf18 (pass-through)
        storeq(28, Gof(1));   // vf19
    }
    // Window-rotating vf23..vf30: "latest vertex of class v%3" with memory as
    // the .w source of truth; only v == j needs the ADC override (its
    // delay-slot MFIR has executed but memory is not yet finalised).
    {
        const int ji = (int)j;
        const int v2 = ClipGate::LatestOfClass(ji, 2, 2);       // guard reg vf23
        const int v0g = ClipGate::LatestOfClass(ji, 0, 3);      // guard reg vf27
        const int v0w = ClipGate::LatestOfClass(ji + 1, 0, 3);  // working group 24/25/26
        const int v1w = ClipGate::LatestOfClass(ji + 1, 1, 4);  // working group 28/29/30
        storeq(29, Gof((u32)v2));                               // vf23 (v2 >= 2 always)
        V4 vf24 = posq((u32)v0w);
        if (v0w == ji) vf24.b[3] = kAdcMark;
        storeq(30, vf24);                                  // vf24
        storeq(31, colq((u32)v0w));                        // vf25
        storeq(32, Pof((u32)v0w));                         // vf26
        storeq(33, v0g >= 0 ? Gof((u32)v0g) : gr.vf27);   // vf27
        if (v1w >= 0) {
            V4 vf28 = posq((u32)v1w);
            if (v1w == ji) vf28.b[3] = kAdcMark;
            storeq(34, vf28);            // vf28
            storeq(35, colq((u32)v1w));  // vf29
            storeq(36, Pof((u32)v1w));   // vf30
        } else {
            storeq(34, gr.vf28);
            storeq(35, gr.vf29);
            storeq(36, gr.vf30);
        }
    }
    // vi spills (ISW: full 32-bit lane = zero-extended u16); the s_hist/g_hist
    // ring layout is the register rotation verbatim in all three windows.
    {
        static const u16 kLink[3] = {0x66, 0x7b, 0x51};  // j%3 = 0/1/2
        m.SetW(37, 0, s_hist[0] & 0xFFFF);  // vi01
        m.SetW(37, 1, g_hist[0] & 0xFFFF);  // vi02
        m.SetW(37, 2, s_hist[1] & 0xFFFF);  // vi03
        m.SetW(37, 3, g_hist[1] & 0xFFFF);  // vi04
        m.SetW(11, 0, m.W(5, 1) & 0xFFFF);  // vi05 = QW5.y raw low16
        m.SetW(11, 1, vi06 & 0xFFFF);       // vi06
        m.SetW(11, 2, vend & 0xFFFF);       // vi07
        m.SetW(11, 3, s_hist[2] & 0xFFFF);  // vi08
        m.SetW(12, 0, gr.vi09);             // vi09 (body value)
        m.SetW(12, 1, g_hist[2] & 0xFFFF);  // vi10
        m.SetW(12, 2, kLink[j % 3]);        // vi11 (window link)
        m.SetW(12, 3, 0x8000);              // vi12
        m.SetW(13, 0, gr.vi13);             // vi13 (body value)
    }

    // ---- triangle init: vj still carries its ORIGINAL .w -------------------
    storeq(39, posq(j - 2));
    storeq(40, {{0x3f800000u, 0, 0, 0x3f800000u}});
    storeq(41, posq(j - 1));
    storeq(42, {{0, 0x3f800000u, 0, 0x3f800000u}});
    storeq(43, posq(j));
    storeq(44, {{0, 0, 0x3f800000u, 0x3f800000u}});

    // ---- six Sutherland-Hodgman passes -------------------------------------
    // dist.w chain (MULA/MADDA/MADDA/MADD, .w lane): the plane row .w is built
    // fresh each pass from clipR (SUB.w/ADD.w = CF(CF(row.w) -/+ CF(lane))).
    // Inside = !signbit(dist.w) (the MADD's raw sign incl. -0.0).
    u32 src = 39, dst_base = 59;
    u32 src_end = 39 + 6;               // vi09 = vi07 + 6
    u32 stale_w = m.W(src_end - 1, 3);  // prime LQ vf06, -1(vi09)
    static const struct {
        int lane;
        float sgn;
    } kPlanes[6] = {{0, -1.0f}, {0, +1.0f}, {1, -1.0f}, {1, +1.0f}, {2, -1.0f}, {2, +1.0f}};
    for (int plane = 0; plane < 6; ++plane) {
        float pw[4];
        for (int r = 0; r < 4; ++r)
            pw[r] = CF(CF(bits2f(clipR[r].b[3])) + kPlanes[plane].sgn * CF(bits2f(clipR[r].b[kPlanes[plane].lane])));
        auto dist = [&](const V4& v) {
            float acc = CF(pw[0] * CF(bits2f(v.b[0])));
            acc = CF(VuFloat::Fma(pw[1], CF(bits2f(v.b[1])), acc));
            acc = CF(VuFloat::Fma(pw[2], CF(bits2f(v.b[2])), acc));
            return CF(VuFloat::Fma(pw[3], 1.0f, acc));  // MADD vf30*vf00w
        };
        auto isect = [&](const V4& prev, const V4& cur, float pd, float cd, float q) {
            // MULA prev*cur_d / MSUB cur*prev_d / MULq -- all four lanes.
            V4 r;
            for (int k = 0; k < 4; ++k) {
                const float acc = CF(CF(bits2f(prev.b[k])) * cd);
                const float sub = CF(VuFloat::Fma(-CF(bits2f(cur.b[k])), pd, acc));
                r.b[k] = f2bits(CF(sub * q));
            }
            return r;
        };
        // Prime prev = LAST vertex (LQ -2/-1(vi09)); NOT emitted.
        V4 prev_pos = loadq(src_end - 2), prev_wt = loadq(src_end - 1);
        stale_w = prev_wt.b[3];
        float prev_d = dist(prev_pos);
        u32 dp = dst_base;
        for (u32 sp = src; sp != src_end; sp += 2) {
            const V4 cur_pos = loadq(sp), cur_wt = loadq(sp + 1);
            stale_w = cur_wt.b[3];
            const float cur_d = dist(cur_pos);
            const bool pin = !std::signbit(prev_d);
            const bool cin = !std::signbit(cur_d);
            if (pin != cin) {
                // MADDA vf30 re-accumulate + MSUB = CF(cur_d - prev_d).
                const float delta = CF(cur_d - prev_d);
                const float q = VuDiv(1.0f, delta);
                if (dp + 2 > dst_base + 20) return false;
                storeq(dp++, isect(prev_pos, cur_pos, prev_d, cur_d, q));
                storeq(dp++, isect(prev_wt, cur_wt, prev_d, cur_d, q));
            }
            if (cin) {
                if (dp + 2 > dst_base + 20) return false;
                storeq(dp++, cur_pos);
                storeq(dp++, cur_wt);
            }
            prev_pos = cur_pos;
            prev_wt = cur_wt;
            prev_d = cur_d;
        }
        if (dp == dst_base) return true;  // empty bail: restore + return, no fan, no kicks
        // Swap: src/dst ping-pong, vi09 = dst end.
        const u32 t = src;
        src = dst_base;
        dst_base = t;
        src_end = dp;
    }

    // ---- fan dispatch: data-driven, accept only this body's emitter entry --
    const u32 fan_pc = (m.W(node, 1) & 0xFFFF) * 8;  // ILW.y vi01, 0(vi14)
    if (fan_pc != gr.fan_pc) return false;
    if (gr.fan_kind == 1) return EmitLitClipFan(m, node, vi06, src, src_end, gr, out);
    if (gr.fan_kind == 2) return EmitFamClipFan(m, node, vi06, src, src_end, gr, out);
    if (gr.fan_kind == 3) return EmitCrysClipFan(m, node, vi06, src, src_end, gr, out);
    return EmitClipFan(m, node, vi06, src, src_end, stale_w, gr, out);
}

// Corner colours vf08/09/10 = col(vj)/col(v_{j-1})/col(v_{j-2}); weights map
// w.x <-> v_{j-2}, w.y <-> v_{j-1}, w.z <-> vj. STQ interpolates the raw
// float lanes of the same QWs whose low bytes unpack into the RGBA corners.
// stale_w = the clipper's final vf06.w: the fan never writes vf06.w.
bool NearClipper::EmitClipFan(QwPtr m, u32 node, u32 vi06, u32 fan_beg, u32 fan_end, u32 stale_w,
                              const ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto loadq = [&](u32 qw) { return LoadQ(m, qw); };
    auto storeq = [&](u32 qw, const V4& v) { StoreQ(m, qw, v); };
    const V4 c_j = loadq(vi06 - 3);    // vf08 = col(vj)
    const V4 c_j1 = loadq(vi06 - 7);   // vf09 = col(v_{j-1})
    const V4 c_j2 = loadq(vi06 - 11);  // vf10 = col(v_{j-2})
    const V4 bias = loadq(node + 2);   // vf23 = ST bias
    auto unpack = [&](const V4& c) {   // MTIR/IAND 0xFF/MFIR + ITOF0.xyzw
        V4 r;
        for (int k = 0; k < 4; ++k) r.b[k] = f2bits((float)(int32_t)(c.b[k] & 0xFF));
        return r;
    };
    const V4 u_j = unpack(c_j);    // vf27
    const V4 u_j1 = unpack(c_j1);  // vf28
    const V4 u_j2 = unpack(c_j2);  // vf29

    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)
    const u32 fan_base = m.W(1, 1) & 0xFFFF;   // ILW.y vi12, 1(vi00)
    const V4 giftag = loadq(6);                // vf11 (pre-kick, both paths)

    // Prelude kick FIRST, then the template store.
    if (!KickAt(m, node_w != 0 ? node + 3 : 6, out)) return false;
    storeq(fan_base, giftag);  // SQ vf11, 0(vi12)

    u32 dst = fan_base + 1;  // vi03 = vi12 + 1
    u32 count = 0;           // vi13
    for (u32 sp = fan_beg; sp != fan_end; sp += 2) {
        const V4 pos = loadq(sp);     // LQI vf25
        const V4 wt = loadq(sp + 1);  // LQI vf26
        const float wx = CF(bits2f(wt.b[0])), wy = CF(bits2f(wt.b[1])), wz = CF(bits2f(wt.b[2]));
        // pos' = MULA vf04*w / MADDA vf01*x / MADDA vf02*y / MADD vf03*z.
        V4 T = XformClip(gr.obj[0], gr.obj[1], gr.obj[2], gr.obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                         CF(bits2f(pos.b[2])));
        // STQ .xyz: MULA.xyz vf10*w.x / MADDA.xyz vf09*w.y / MADD.xyz vf08*w.z.
        V4 stq;
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(c_j2.b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(c_j1.b[k])), wy, acc));
            stq.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(c_j.b[k])), wz, acc)));
        }
        stq.b[3] = stale_w;  // vf06.w never written
        // Color .xyzw: MULA vf29*w.x / MADDA vf28*w.y / MADD vf27*w.z.
        V4 col;
        for (int k = 0; k < 4; ++k) {
            float acc = CF(CF(bits2f(u_j2.b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(u_j1.b[k])), wy, acc));
            col.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(u_j.b[k])), wz, acc)));
        }
        const float q = VuDiv(1.0f, bits2f(T.b[3]));  // DIV Q, vf00w, vf05w
        for (int k = 0; k < 2; ++k)                    // ADD.xy vf06 += vf23
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) + CF(bits2f(bias.b[k]))));
        T.b[3] = 0x3f800000u;        // MOVE.w vf05, vf00 (raw)
        for (int k = 0; k < 3; ++k) {  // MUL.xyz *= Q (new Q, both)
            T.b[k] = f2bits(CF(CF(bits2f(T.b[k])) * q));
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) * q));
        }
        for (int k = 0; k < 4; ++k) col.b[k] = Ftoi0(col.b[k]);  // FTOI0.xyzw
        for (int k = 0; k < 3; ++k) T.b[k] = Ftoi0(T.b[k]);      // FTOI0.xyz
        storeq(dst++, stq);  // SQI vf06
        storeq(dst++, col);  // SQI vf07
        storeq(dst++, T);    // SQI vf05 (delay slot)
        ++count;
    }
    // Giftag NLOOP patch: ISW.x writes the full word0 lane with the
    // zero-extended 0x8000+count (upper 16 bits of word0 cleared).
    m.SetW(fan_base, 0, (0x8000u + count) & 0xFFFFu);
    return KickAt(m, fan_base, out);  // XGKICK vi12
}

// Corner colours are RECOMPUTED by the lit lighting chain per corner; ST rows
// interpolate the raw vertex ST QWs; the emitted ST .w = QW[mesh+6].w raw.
bool NearClipper::EmitLitClipFan(QwPtr m, u32 node, u32 vi06, u32 fan_beg, u32 fan_end,
                                 const ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto loadq = [&](u32 qw) { return LoadQ(m, qw); };
    auto storeq = [&](u32 qw, const V4& v) { StoreQ(m, qw, v); };
    const u32 mesh = m.W(13, 0) & 0xFFFF;  // ILW.x vi13, 13(vi00)
    V4 nrmR[3], litR[4];
    for (int r = 0; r < 3; ++r) nrmR[r] = loadq(mesh + 5 + r);  // vf05..07
    for (int r = 0; r < 4; ++r) litR[r] = loadq(mesh + 9 + r);  // vf08..11
    const V4 bias = loadq(node + 2);                            // vf23 = ST bias
    const u32 st_w = m.W(mesh + 6, 3);                          // vf06.w = nrmR[1].w raw

    // Corner colour: the lit chain at one window corner (normal QW n_qw,
    // static colour QW c_qw) -- the per-vertex math of the lit emitter.
    auto corner = [&](u32 n_qw, u32 c_qw) {
        const V4 n = loadq(n_qw);
        const float nx = CF(bits2f(n.b[0])), ny = CF(bits2f(n.b[1])), nz = CF(bits2f(n.b[2]));
        float wn[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
            acc = CF(VuFloat::Fma(CF(bits2f(nrmR[1].b[k])), ny, acc));
            wn[k] = CF(VuFloat::Fma(CF(bits2f(nrmR[2].b[k])), nz, acc));
        }
        const float wcx = bits2f(Fmax_raw(f2bits(wn[0]), 0u));
        const float wcy = bits2f(Fmax_raw(f2bits(wn[1]), 0u));
        const float wcz = bits2f(Fmax_raw(f2bits(wn[2]), 0u));
        const V4 c = loadq(c_qw);
        V4 r;
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
            acc = CF(VuFloat::Fma(CF(bits2f(litR[0].b[k])), CF(wcx), acc));
            acc = CF(VuFloat::Fma(CF(bits2f(litR[1].b[k])), CF(wcy), acc));
            const float lt = CF(VuFloat::Fma(CF(bits2f(litR[2].b[k])), CF(wcz), acc));
            const float s = (float)(int32_t)c.b[k];           // ITOF0
            const float mod = CF(CF(lt) * CF(s));             // MUL.xyz vf12,vf12,vf16
            r.b[k] = Fmin_raw(f2bits(mod), f2bits(255.0f));  // MINIi
        }
        const float sa = (float)(int32_t)c.b[3];
        r.b[3] = Fmin_raw(f2bits(sa), f2bits(255.0f));
        return r;
    };
    const V4 corner_j = corner(vi06 - 2, vi06 - 1);    // vf12 (vj)
    const V4 corner_j1 = corner(vi06 - 6, vi06 - 5);   // vf13 (v_{j-1})
    const V4 corner_j2 = corner(vi06 - 10, vi06 - 9);  // vf14 (v_{j-2})
    const V4 st_j = loadq(vi06 - 3);                   // vf27 (vj)
    const V4 st_j1 = loadq(vi06 - 7);                  // vf28
    const V4 st_j2 = loadq(vi06 - 11);                 // vf29

    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)
    const u32 fan_base = m.W(1, 1) & 0xFFFF;   // ILW.y vi12, 1(vi00)
    const V4 giftag = loadq(6);                // vf11 (pre-kick)

    // Prelude kick FIRST, then the template store.
    if (!KickAt(m, node_w != 0 ? node + 3 : 6, out)) return false;
    storeq(fan_base, giftag);  // SQ vf11, 0(vi12)

    u32 dst = fan_base + 1;  // vi03 = vi12 + 1
    u32 count = 0;           // vi13
    for (u32 sp = fan_beg; sp != fan_end; sp += 2) {
        const V4 pos = loadq(sp);     // LQI vf25
        const V4 wt = loadq(sp + 1);  // LQI vf26
        const float wx = CF(bits2f(wt.b[0])), wy = CF(bits2f(wt.b[1])), wz = CF(bits2f(wt.b[2]));
        // MULAw vf04*1 / MADDAx vf01*x / MADDAy vf02*y / MADDz vf03*z.
        V4 T = XformClip(gr.obj[0], gr.obj[1], gr.obj[2], gr.obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                         CF(bits2f(pos.b[2])));
        // ST .xyz: MULAx vf29*w.x / MADDAy vf28*w.y / MADDz vf27*w.z.
        V4 stq;
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(st_j2.b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(st_j1.b[k])), wy, acc));
            stq.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(st_j.b[k])), wz, acc)));
        }
        stq.b[3] = st_w;  // vf06.w pre-loop
        // Color .xyzw: MULAx vf14*w.x / MADDAy vf13*w.y / MADDz vf12*w.z.
        V4 col;
        for (int k = 0; k < 4; ++k) {
            float acc = CF(CF(bits2f(corner_j2.b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(corner_j1.b[k])), wy, acc));
            col.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(corner_j.b[k])), wz, acc)));
        }
        const float q = VuDiv(1.0f, bits2f(T.b[3]));  // DIV Q, vf00w, vf05w
        for (int k = 0; k < 2; ++k)                    // ADD.xy vf06 += vf23
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) + CF(bits2f(bias.b[k]))));
        T.b[3] = 0x3f800000u;        // MOVE.w vf05, vf00
        for (int k = 0; k < 3; ++k) {  // MULq.xyz (both)
            T.b[k] = f2bits(CF(CF(bits2f(T.b[k])) * q));
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) * q));
        }
        for (int k = 0; k < 4; ++k) col.b[k] = Ftoi0(col.b[k]);  // FTOI0.xyzw
        for (int k = 0; k < 3; ++k) T.b[k] = Ftoi0(T.b[k]);      // FTOI0.xyz
        storeq(dst++, stq);  // SQI vf06
        storeq(dst++, col);  // SQI vf07
        storeq(dst++, T);    // SQI vf05 (delay slot)
        ++count;
    }
    m.SetW(fan_base, 0, (0x8000u + count) & 0xFFFFu);
    return KickAt(m, fan_base, out);  // XGKICK vi12
}

// Backface-cull prefilter over the first three clip positions (edge pair
// selected by QW[vq_j+2].x bit 5; a mismatching verdict drops the polygon
// with zero kicks and stores), then an unconditional QW6 prelude kick and a
// flat node-colour fan: xyz = obj rows * pos * Q, w = 0x20, z negated, the
// vf06 slot = the prefilter's stale transformed clip vertex 1 with z = 1.0
// and a cumulative *Q per iteration.
bool NearClipper::EmitFamClipFan(QwPtr m, u32 node, u32 vi06, u32 fan_beg, u32 fan_end,
                                 const ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto loadq = [&](u32 qw) { return LoadQ(m, qw); };
    auto storeq = [&](u32 qw, const V4& v) { StoreQ(m, qw, v); };
    auto xform = [&](const V4& p) {
        return XformClip(gr.obj[0], gr.obj[1], gr.obj[2], gr.obj[3], CF(bits2f(p.b[0])), CF(bits2f(p.b[1])),
                         CF(bits2f(p.b[2])));
    };
    const V4 color = loadq(node + 1);                // vf08 (flat node color)
    const u32 winding = m.W(node + 2, 3) & 0xFFFF;   // ILW.w vi03, 2(vi14)
    const u32 vflag = m.W(vi06 - 2, 0) & 32u;        // ILW.x vi10, -2(vi06) & 32
    const u32 expect = (winding == 0) ? 0u : 32u;    // vi04

    // Transform + divide the first three clip positions (vf05/06/07).
    V4 t[3];
    for (int i = 0; i < 3; ++i) {
        t[i] = xform(loadq(fan_beg + 2 * (u32)i));
        const float qq = VuDiv(1.0f, bits2f(t[i].b[3]));
        for (int k = 0; k < 3; ++k) t[i].b[k] = f2bits(CF(CF(bits2f(t[i].b[k])) * qq));
    }
    auto sub3 = [&](const V4& a, const V4& b) {  // SUB.xyz
        V4 r{};
        for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(a.b[k])) - CF(bits2f(b.b[k]))));
        return r;
    };
    V4 e16, e17;
    if (vflag != 0) {
        e16 = sub3(t[0], t[1]);
        e17 = sub3(t[2], t[0]);
    } else {
        e16 = sub3(t[1], t[0]);
        e17 = sub3(t[2], t[1]);
    }
    if (CrossZSign(e16, e17) != expect) return true;  // culled: JR, no kicks/stores

    const u32 fan_base = m.W(1, 1) & 0xFFFF;  // ILW.y vi12, 1(vi00)
    const V4 giftag = loadq(6);               // vf11
    if (!KickAt(m, 6, out)) return false;     // XGKICK vi02(=6), always
    storeq(fan_base, giftag);                 // SQ vf11, 0(vi12)

    V4 vf06 = t[1];          // stale transformed clip v1
    u32 dst = fan_base + 1;  // vi03 = vi12 + 1
    u32 count = 0;           // vi13
    for (u32 sp = fan_beg; sp != fan_end; sp += 2) {
        const V4 pos = loadq(sp);  // LQI vf25 (vf26 unused)
        V4 T = xform(pos);         // vf05
        const float qq = VuDiv(1.0f, bits2f(T.b[3]));
        for (int k = 0; k < 3; ++k)  // MULq.xyz vf05
            T.b[k] = f2bits(CF(CF(bits2f(T.b[k])) * qq));
        vf06.b[2] = 0x3f800000u;                          // MR32.z vf06, vf00 (z=1.0)
        T.b[3] = 0x20u;                                   // MFIR.w vf05, vi11(32)
        T.b[2] = f2bits(CF(0.0f - CF(bits2f(T.b[2]))));  // SUB.z vf00 - z
        for (int k = 0; k < 3; ++k)                       // MULq.xyz vf06 (cumulative)
            vf06.b[k] = f2bits(CF(CF(bits2f(vf06.b[k])) * qq));
        for (int k = 0; k < 3; ++k) T.b[k] = Ftoi0(T.b[k]);  // FTOI0.xyz
        storeq(dst++, vf06);   // SQI vf06
        storeq(dst++, color);  // SQI vf08
        storeq(dst++, T);      // SQI vf05
        ++count;               // delay-slot IADDIU vi13
    }
    m.SetW(fan_base, 0, (0x8000u + count) & 0xFFFFu);  // NLOOP patch
    return KickAt(m, fan_base, out);                    // XGKICK vi12
}

// Every vertex attribute runs the crystal env-lit chain on weight-interpolated
// inputs; the prelude is node+5 when node.w != 0 (else QW6); vf29's .zw lanes
// carry the previous iteration's FTOI0'd ints through the four-lane clamp
// (loop-entry value = the corner phase's lit(j-2)).
bool NearClipper::EmitCrysClipFan(QwPtr m, u32 node, u32 vi06, u32 fan_beg, u32 fan_end,
                                  const ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto loadq = [&](u32 qw) { return LoadQ(m, qw); };
    auto storeq = [&](u32 qw, const V4& v) { StoreQ(m, qw, v); };
    const u32 mesh = m.W(13, 0) & 0xFFFF;  // ILW.x vi13, 13(vi00)
    // Corner loads: colors QW[vi06-1/-5/-9], normals QW[vi06-2/-6/-10]
    // (index 0 = vj, 1 = v_{j-1}, 2 = v_{j-2}).
    V4 colc[3] = {loadq(vi06 - 1), loadq(vi06 - 5), loadq(vi06 - 9)};
    const V4 nrmc[3] = {loadq(vi06 - 2), loadq(vi06 - 6), loadq(vi06 - 10)};
    // ITOF0.xyzw the static colors (vf12/13/14).
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 4; ++k) colc[c].b[k] = f2bits((float)(int32_t)colc[c].b[k]);
    V4 nrmR[3], litR[4];
    for (int r = 0; r < 3; ++r) nrmR[r] = loadq(mesh + 5 + r);  // vf05..07
    for (int r = 0; r < 4; ++r) litR[r] = loadq(mesh + 9 + r);  // vf08..11
    // Per-corner lighting (vf27/28/29): wn = nrmR*n (4-lane), MAX +0,
    // lit = litR3 + litR0*wn.x + litR1*wn.y + litR2*wn.z (4-lane).
    V4 litc[3];
    for (int c = 0; c < 3; ++c) {
        const float nx = CF(bits2f(nrmc[c].b[0])), ny = CF(bits2f(nrmc[c].b[1])), nz = CF(bits2f(nrmc[c].b[2]));
        V4 wn;
        for (int k = 0; k < 4; ++k) {
            float acc = CF(CF(bits2f(nrmR[0].b[k])) * nx);
            acc = CF(VuFloat::Fma(CF(bits2f(nrmR[1].b[k])), ny, acc));
            wn.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(nrmR[2].b[k])), nz, acc)));
        }
        for (int k = 0; k < 4; ++k) wn.b[k] = Fmax_raw(wn.b[k], 0u);
        const float wx = CF(bits2f(wn.b[0])), wy = CF(bits2f(wn.b[1])), wz = CF(bits2f(wn.b[2]));
        for (int k = 0; k < 4; ++k) {
            float acc = CF(CF(bits2f(litR[3].b[k])) * 1.0f);
            acc = CF(VuFloat::Fma(CF(bits2f(litR[0].b[k])), wx, acc));
            acc = CF(VuFloat::Fma(CF(bits2f(litR[1].b[k])), wy, acc));
            litc[c].b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(litR[2].b[k])), wz, acc)));
        }
    }
    // Color modulate (MUL.xyz vf12/13/14 by lit) + MINIi.xyzw 255.
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) colc[c].b[k] = f2bits(CF(CF(bits2f(colc[c].b[k])) * CF(bits2f(litc[c].b[k]))));
        for (int k = 0; k < 4; ++k) colc[c].b[k] = Fmin_raw(colc[c].b[k], f2bits(255.0f));
    }
    const u32 clip_off = m.W(mesh, 2) & 0xFFFF;  // ILW.z vi03, 0(vi13)
    V4 stR[3];
    for (int r = 0; r < 3; ++r) stR[r] = loadq(mesh + clip_off + r);
    const V4 p2 = loadq(node + 2);             // LQ vf08, 2(vi14)
    const V4 p3 = loadq(node + 3);             // LQ vf09, 3(vi14)
    const V4 p4 = loadq(node + 4);             // LQ vf10, 4(vi14)
    const V4 giftag = loadq(6);                // LQ vf11, 6(vi00)
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)
    const u32 fan_base = m.W(1, 1) & 0xFFFF;   // ILW.y vi12, 1(vi00)

    // Prelude kick (node+5 header when node.w != 0, else QW6), then the
    // giftag template store.
    if (!KickAt(m, node_w != 0 ? node + 5 : 6, out)) return false;
    storeq(fan_base, giftag);  // SQ vf11, 0(vi12)

    u32 dst = fan_base + 1;  // vi03 = vi12 + 1
    u32 count = 0;           // vi13
    V4 vf29 = litc[2];       // loop-entry live register: the corner phase's last vf29 write
    for (u32 sp = fan_beg; sp != fan_end; sp += 2) {
        const V4 pos = loadq(sp);     // LQI vf25
        const V4 wt = loadq(sp + 1);  // LQI vf26
        const float wx = CF(bits2f(wt.b[0])), wy = CF(bits2f(wt.b[1])), wz = CF(bits2f(wt.b[2]));
        // T = MULAw vf04 / MADDAx vf01 / MADDAy vf02 / MADDz vf03.
        V4 T = XformClip(gr.obj[0], gr.obj[1], gr.obj[2], gr.obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                         CF(bits2f(pos.b[2])));
        const float q = VuDiv(1.0f, bits2f(T.b[3]));  // DIV Q,vf00w,vf27w
        V4 scr = T;                                    // MULq.xyz vf27 (w = T.w)
        for (int k = 0; k < 3; ++k) scr.b[k] = f2bits(CF(CF(bits2f(T.b[k])) * q));
        // Interpolated normal (MULAx vf18/MADDAy vf17/MADDz vf16, xyz).
        float ni[3];
        for (int k = 0; k < 3; ++k) {
            float acc = CF(CF(bits2f(nrmc[2].b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(nrmc[1].b[k])), wy, acc));
            ni[k] = CF(VuFloat::Fma(CF(bits2f(nrmc[0].b[k])), wz, acc));
        }
        // st.xy = (scr.xy - p3.xy) * p4.xy  (SUB.xy / MUL.xy into vf29.xy;
        // vf29.zw keep the previous iteration's FTOI0 ints -- live register).
        for (int k = 0; k < 2; ++k) {
            const float d = CF(CF(bits2f(scr.b[k])) - CF(bits2f(p3.b[k])));
            vf29.b[k] = f2bits(CF(CF(d) * CF(bits2f(p4.b[k]))));
        }
        // stp = stR-projected n' (MULAz/MADDAy/MADDx; xy consumed).
        float stp[2];
        for (int k = 0; k < 2; ++k) {
            float acc = CF(CF(bits2f(stR[2].b[k])) * ni[2]);
            acc = CF(VuFloat::Fma(CF(bits2f(stR[1].b[k])), ni[1], acc));
            stp[k] = CF(VuFloat::Fma(CF(bits2f(stR[0].b[k])), ni[0], acc));
        }
        // v.xy = st + stp*p3.z (MULAw.xy ACC, vf29, vf00.w; MADDz.xy).
        const float p3z = CF(bits2f(p3.b[2]));
        for (int k = 0; k < 2; ++k) {
            const float acc = CF(CF(bits2f(vf29.b[k])) * 1.0f);
            vf29.b[k] = f2bits(CF(VuFloat::Fma(CF(stp[k]), p3z, acc)));
        }
        // vf28 = MINIw(vf29, vf00.w) then MAXx(vf28, vf00.x) -- FOUR lanes.
        V4 stq;
        for (int k = 0; k < 4; ++k) stq.b[k] = Fmax_raw(Fmin_raw(vf29.b[k], 0x3f800000u), 0u);
        stq.b[2] = f2bits(CF(0.0f + 1.0f));  // ADDw.z vf28, vf00, vf00
        for (int k = 0; k < 2; ++k)          // ADD.xy vf28 += p2
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) + CF(bits2f(p2.b[k]))));
        for (int k = 0; k < 3; ++k)  // MULq.xyz
            stq.b[k] = f2bits(CF(CF(bits2f(stq.b[k])) * q));
        // Color interp (MULAx vf14/MADDAy vf13/MADD vf12, xyzw) -> vf29.
        for (int k = 0; k < 4; ++k) {
            float acc = CF(CF(bits2f(colc[2].b[k])) * wx);
            acc = CF(VuFloat::Fma(CF(bits2f(colc[1].b[k])), wy, acc));
            vf29.b[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(colc[0].b[k])), wz, acc)));
        }
        scr.b[3] = 0x3f800000u;                                    // MOVE.w vf27, vf00
        for (int k = 0; k < 4; ++k) vf29.b[k] = Ftoi0(vf29.b[k]);  // FTOI0
        for (int k = 0; k < 3; ++k) scr.b[k] = Ftoi0(scr.b[k]);    // FTOI0.xyz
        storeq(dst++, stq);   // SQI vf28 (ST)
        storeq(dst++, vf29);  // SQI vf29 (RGBA)
        storeq(dst++, scr);   // SQI vf27 (XYZ2)
        ++count;
    }
    m.SetW(fan_base, 0, (0x8000u + count) & 0xFFFFu);
    return KickAt(m, fan_base, out);  // XGKICK vi12
}
