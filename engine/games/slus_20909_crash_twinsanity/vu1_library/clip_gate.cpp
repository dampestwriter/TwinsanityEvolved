// ClipGate -- the shared 0xB0 clip gate (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/clip_gate.h"

#include "games/slus_20909_crash_twinsanity/vu1_library/near_clipper.h"

using namespace vu1lib;

ClipGate::GateRegs ClipGate::SeedGateRegs(const Vu1HleInput& in, u32 fan_pc, bool image_ok) {
    GateRegs gr;
    gr.fan_pc = fan_pc;
    gr.valid = image_ok && in.regs != nullptr;
    if (in.regs != nullptr) {
        auto cp = [&](V4& d, int r) {
            for (int k = 0; k < 4; ++k) d.b[k] = in.regs->VF[r].I[k];
        };
        cp(gr.vf16, 16);
        cp(gr.vf18, 18);
        cp(gr.vf27, 27);
        cp(gr.vf28, 28);
        cp(gr.vf29, 29);
        cp(gr.vf30, 30);
    }
    return gr;
}

int ClipGate::LatestOfClass(int limit, int c, int minv) {
    if (limit < minv) return -1;
    const int r = ((limit - c) % 3 + 3) % 3;
    const int v = limit - r;
    return v >= minv ? v : -1;
}

// Persistent vf27..vf30 at gate-loop end: the next call's early windows spill
// these as the "entry" values. Memory reads give the finalised pos .w for
// v <= n-1 and the original .w for v == n -- exactly the register contents.
void ClipGate::UpdateGateRegsAtEnd(QwPtr m, u32 top, const V4 clipR[5], u32 nverts, GateRegs& gr) {
    const u32 vbase = top + 3;
    const int n = (int)nverts;
    auto posq = [&](int v) {
        V4 r;
        for (int k = 0; k < 4; ++k) r.b[k] = m.W(vbase + 4 * (u32)v, k);
        return r;
    };
    auto colq = [&](int v) {
        V4 r;
        for (int k = 0; k < 4; ++k) r.b[k] = m.W(vbase + 4 * (u32)v + 1, k);
        return r;
    };
    auto Pof = [&](int v) {
        const u32 vq = vbase + 4 * (u32)v;
        return XformClip(clipR[0], clipR[1], clipR[2], clipR[3], CF(m.F(vq, 0)), CF(m.F(vq, 1)), CF(m.F(vq, 2)));
    };
    if (int v = LatestOfClass(n - 1, 0, 3); v >= 0) gr.vf27 = GuardScale(clipR[4], Pof(v));
    if (int v = LatestOfClass(n, 1, 4); v >= 0) {
        gr.vf28 = posq(v);
        gr.vf29 = colq(v);
        gr.vf30 = Pof(v);
    }
}

bool ClipGate::ClipGateB0(QwPtr m, u32 node, u32 top, u32 link_vi12, const V4 clipR[5], u32 nverts, GateRegs* gr,
                          Vu1HleOutput* out) {
    m.SetW(10, 0, node & 0xFFFF);       // ISW.x vi14, 10(vi00)
    m.SetW(10, 1, 0x0A);                // ISW.y vi15 (dispatcher link)
    m.SetW(10, 2, link_vi12 & 0xFFFF);  // ISW.z vi12 (gate-call link)
    const u32 vbase = top + 3;
    u32 s_hist[3] = {0, 0, 0}, g_hist[3] = {0, 0, 0};
    for (u32 j = 0; j < nverts; ++j) {
        const u32 vq = vbase + j * 4;
        const float vx = CF(m.F(vq, 0)), vy = CF(m.F(vq, 1)), vz = CF(m.F(vq, 2));
        const V4 p = XformClip(clipR[0], clipR[1], clipR[2], clipR[3], vx, vy, vz);
        const V4 g = GuardScale(clipR[4], p);
        s_hist[j % 3] = ClipNf(p.b[0], p.b[1], p.b[2], p.b[3]);
        g_hist[j % 3] = ClipNf(g.b[0], g.b[1], g.b[2], g.b[3]);
        u32 wbits = m.W(vq + 1, 3);  // default: QW[v+1].w raw
        if (j >= 2) {
            const u32 hard = s_hist[0] & s_hist[1] & s_hist[2];
            const u32 gor = g_hist[0] | g_hist[1] | g_hist[2];
            if (hard != 0) {
                wbits = kAdcMark;
            } else if (gor != 0) {
                // Near-clipper call: must be the family clipper at QW5.y.
                const u32 clipper_pc = (m.W(5, 1) & 0xFFFF) * 8;
                if (clipper_pc != NearClipper::kEntryPc) return false;
                if ((m.W(vq + 1, 3) & 0x8000u) == 0) {
                    // Full interpolating clipper (bit15 clear).
                    if (gr == nullptr || !gr->valid || out == nullptr) return false;
                    if (!NearClipper::Run(m, node, top, clipR, j, nverts, s_hist, g_hist, *gr, *out)) return false;
                }
                wbits = kAdcMark;  // delay-slot MFIR.w (both paths)
            }
        }
        m.SetW(vq, 3, wbits);
    }
    if (gr != nullptr && gr->valid) UpdateGateRegsAtEnd(m, top, clipR, nverts, *gr);
    return true;
}
