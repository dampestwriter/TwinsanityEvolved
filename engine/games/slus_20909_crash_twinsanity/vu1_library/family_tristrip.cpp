// FamilyTristrip -- vs_family_tristrip (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/family_tristrip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;     // the family emitter sub (= the variant base)
constexpr u32 kExitPc = 0x90;          // the E-bit exit node
constexpr u32 kGateHelperPc = 0xB0;    // the shared clip gate (QW4.y * 8)
constexpr u32 kGateLink = 0x2FA;       // (gate JALR pc + 16) / 8 of this body
constexpr u32 kFanPc = 0x1ac0;         // the family fan emitter entry
constexpr int kFanKind = 2;            // the family fan body
// The image whose fan body the full near-clip lift was coded from.
constexpr u64 kClipImage = 0x713e44016d5f4840ull;

}  // namespace

bool FamilyTristrip::RunEmitterCall(QwPtr m, u32 node, u32 top, Vu1HleOutput& out, ClipGate::GateRegs* gr) {
    // --- dispatcher-side node fields --------------------------------------
    const u32 node_color_qw = node + 1;             // LQ vf05, 1(vi14)
    const u32 winding = m.W(node + 2, 3) & 0xFFFF;  // ILW.w vi10, 2(vi14)

    // --- mesh list --------------------------------------------------------
    u32 mesh = m.W(2, 0) & 0xFFFF;                   // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;      // ILW.y vi10, 0(vi13) (once)
    const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
    if (helper_pc != kGateHelperPc) return false;    // unexpected prefix layout

    for (int mesh_guard = 0; mesh_guard < 64; ++mesh_guard) {
        // Per-mesh matrices.
        const u32 clip_off = m.W(mesh, 2) & 0xFFFF;  // ILW.z vi03, 0(vi13)
        V4 obj[4], clipR[5];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) obj[r].b[k] = m.W(mesh + 1 + r, k);
        for (int r = 0; r < 5; ++r)
            for (int k = 0; k < 4; ++k) clipR[r].b[k] = m.W(mesh + clip_off + r, k);

        const u32 vbase = top + 3;
        // Header: vf07 = QW[top+1]; MR32; MTIR .w -> original .x = vertex QWs.
        const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;
        if ((vqw_count & 3) != 0) return false;
        const u32 nverts = vqw_count / 4;
        if (nverts < 3) return false;  // micro loop needs >= 3

        // ---- clip gate: rewrite QW[v].w, verdicts; body pins vi09 = mesh_end,
        // vi13 = mesh for the near-clipper spill ----------------------------
        if (gr != nullptr) {
            gr->vi09 = (u16)mesh_end;
            gr->vi13 = (u16)mesh;
            for (int r = 0; r < 4; ++r) gr->obj[r] = obj[r];
        }
        if (!ClipGate::ClipGateB0(m, node, top, kGateLink, clipR, nverts, gr, gr != nullptr ? &out : nullptr))
            return false;

        // ---- emitter body ----------------------------------------------------
        const u32 out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
        // GIFtag template QW[top] -> QW[out_base]  (SQ vf06, 0(vi11))
        for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));
        const u32 expect = (winding == 0) ? 0u : 32u;  // vi04

        V4 color;
        for (int k = 0; k < 4; ++k) color.b[k] = m.W(node_color_qw, k);

        // Working registers (named after the micro's VF usage).
        auto load = [&](u32 qw) {
            V4 v;
            for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
            return v;
        };
        auto store = [&](u32 qw, const V4& v) {
            for (int k = 0; k < 4; ++k) m.SetW(qw, k, v.b[k]);
        };
        auto xform = [&](const V4& v) {
            return XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(v.b[0])), CF(bits2f(v.b[1])),
                             CF(bits2f(v.b[2])));
        };
        auto sub_xyz = [&](const V4& a, const V4& b) {  // SUB.xyz r, a, b
            V4 r{};
            for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(a.b[k])) - CF(bits2f(b.b[k]))));
            return r;
        };
        auto mulq_xyz = [&](const V4& v, float q, u32 wbits) {  // MULq.xyz + MOVE.w
            V4 r;
            for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(v.b[k])) * q));
            r.b[3] = wbits;
            return r;
        };
        auto negz_ftoi = [&](V4& v) {  // SUB.z vf, vf00, vf then FTOI0.xyz
            v.b[2] = f2bits(CF(0.0f - CF(bits2f(v.b[2]))));
            v.b[0] = Ftoi0(v.b[0]);
            v.b[1] = Ftoi0(v.b[1]);
            v.b[2] = Ftoi0(v.b[2]);
        };

        // Prologue: v0/v1 (never backface-tested).
        u32 vi06 = vbase;                    // input walk (QW index)
        u32 vi11 = out_base + 7;             // after +1 and +6
        const u32 vend = vbase + vqw_count;  // vi07

        V4 raw_a = load(vi06);      // vf08 = v0
        V4 raw_b = load(vi06 + 4);  // vf19 = v1
        V4 t_a = xform(raw_a);      // vf11 = T(v0)
        V4 t_b = xform(raw_b);      // vf20 = T(v1)
        vi06 += 8;

        float Q = VuDiv(1.0f, bits2f(t_a.b[3]));
        V4 s_a = mulq_xyz(t_a, Q, raw_a.b[3]);  // vf21 (s0), .w = v0.w raw
        V4 vf13 = s_a;                          // prev screen (xyz used)
        Q = VuDiv(1.0f, bits2f(t_b.b[3]));
        V4 s_b = mulq_xyz(t_b, Q, raw_b.b[3]);  // vf22 (s1)
        V4 vf14 = s_b;

        raw_a = load(vi06);            // vf08 = v2
        t_a = xform(raw_a);            // vf11 = T(v2)
        negz_ftoi(s_a);                // v0 out int
        V4 e16 = sub_xyz(vf14, vf13);  // vf16 = e01
        store(vi11 - 5, color);        // RGBA v0
        store(vi11 - 4, s_a);          // XYZ v0
        Q = VuDiv(1.0f, bits2f(t_a.b[3]));
        negz_ftoi(s_b);          // v1 out int
        raw_b = load(vi06 + 4);  // vf19 = v3
        t_b = xform(raw_b);      // vf20 = T(v3)
        vf13 = vf14;             // MOVE.xyzw vf13, vf14 (= s1)
        store(vi11 - 2, color);  // RGBA v1
        store(vi11 - 1, s_b);    // XYZ v1

        // Steady loop: pair (a, b) per iteration, a = 2,4,6...
        // Loop-carried state at the boundary: t_a/t_b = T(v_a)/T(v_b),
        // Q = 1/T(v_a).w retired, raw_a/raw_b raw, vf13 = s_{a-1},
        // e16 = edge(a-2 -> a-1). Every DIV feeds the NEXT vertex's MULq.
        V4 e17{};
        V4 last_cross{};                        // the final OPMSUB vf18
        V4 cur = mulq_xyz(t_a, Q, raw_a.b[3]);  // vf21 = s_a
        Q = VuDiv(1.0f, bits2f(t_b.b[3]));
        e17 = sub_xyz(cur, vf13);  // vf17 = edge(a-1 -> a)
        bool ended = false;
        while (!ended) {
            V4 raw_n1 = load(vi06 + 8);  // next pair's first raw vertex
            V4 prev13 = cur;             // MOVE.xyz vf13
            vf13.b[0] = prev13.b[0];
            vf13.b[1] = prev13.b[1];
            vf13.b[2] = prev13.b[2];
            const u32 verdict_a = CrossZSign(e16, e17);  // OPMULA/OPMSUB -> FMAND
            V4 nxt = mulq_xyz(t_b, Q, raw_b.b[3]);       // vf22 = s_b
            vi11 += 6;
            vi06 += 4;
            e16 = sub_xyz(nxt, vf13);                        // vf16 = edge(a -> b)
            V4 raw_n2 = load(vi06 + 8);                      // vf19' (v_{b+2})
            V4 t_n1 = xform(raw_n1);                         // -> vf11
            if (verdict_a != expect) cur.b[3] = kAdcMark;
            negz_ftoi(cur);                                  // SUB.z + FTOI0 (.w set before)
            const u32 verdict_b = CrossZSign(e16, e17);      // OPMULA/OPMSUB -> FMAND
            if (gr != nullptr) last_cross = CrossXYZ(e16, e17);  // vf18
            vf13.b[0] = nxt.b[0];
            vf13.b[1] = nxt.b[1];
            vf13.b[2] = nxt.b[2];                // MOVE.xyz vf13, vf22
            store(vi11 - 5, color);              // RGBA a
            Q = VuDiv(1.0f, bits2f(t_n1.b[3]));  // DIV (for a+2)
            store(vi11 - 4, cur);                // XYZ a (delay slot -- always)
            if (vi06 == vend) {                  // IBEQ (odd exit)
                ended = true;
                break;
            }
            V4 t_n2 = xform(raw_n2);  // -> vf20
            if (verdict_b != expect) nxt.b[3] = kAdcMark;
            negz_ftoi(nxt);                         // SUB.z + FTOI0
            vi06 += 4;                              // (delay slot -- always)
            cur = mulq_xyz(t_n1, Q, raw_n1.b[3]);   // vf21 = s_{a+2}
            store(vi11 - 2, color);                 // RGBA b
            Q = VuDiv(1.0f, bits2f(t_n2.b[3]));     // DIV (for b+2)
            const bool loop_back = (vi06 != vend);  // IBNE
            e17 = sub_xyz(cur, vf13);               // vf17 (delay slot -- always)
            store(vi11 - 1, nxt);                   // XYZ b (always)
            if (!loop_back) {
                ended = true;
                break;
            }
            raw_a = raw_n1;
            raw_b = raw_n2;
            t_a = t_n1;
            t_b = t_n2;
        }

        // XGKICK vi05 (= out_base): packet size from the FIRST GIFtag only.
        if (!KickAt(m, out_base, out)) return false;

        // QW1 double-buffer rotation (LQ vf06,1(vi00); MR32; SQ 1(vi00)).
        RotateQw1(m);

        // Re-seed the pass-through spill registers with the emitter loop's end
        // state: vf16 = the final edge SUB.xyz with .w = 0 (the body prologue
        // zero-fills all four lanes each mesh and SUB.xyz never writes .w);
        // vf18 = the final OPMSUB cross (.w never written by the body).
        if (gr != nullptr) {
            for (int k = 0; k < 3; ++k) {
                gr->vf16.b[k] = e16.b[k];
                gr->vf18.b[k] = last_cross.b[k];
            }
            gr->vf16.b[3] = 0;
        }
        // Next mesh: vi13 += QW[vi13].w; loop while vi13 != end.
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0x3FF;
        if (mesh == mesh_end) return true;
    }
    return false;  // runaway mesh list
}

bool FamilyTristrip::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != 0) return false;

    // The VU rounds toward zero; the whole body runs under the same mode.
    VuChopRoundGuard chop_guard;

    // Working copy of data memory: the program mutates it (vertex .w flags,
    // GIF packet slots, QW1 rotation, QW10 spills) and later kicks read the
    // accumulated state.
    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    // Dispatcher: walk the node list from QW3.x; only emitter calls and the
    // exit terminator are lifted -- anything else declines. The full near
    // clip is lifted only for the image whose fan body was verified.
    const bool clip_image_ok =
        Vu1LoadedPrograms::At(in, kEmitterPc) == Vu1LoadedPrograms::kType0B || in.image_hash == kClipImage;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kFanPc, clip_image_ok);
    gr.fan_kind = kFanKind;

    u32 node = m.W(3, 0) & 0xFFFF;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;              // E-bit exit node
        if (call_pc != kEmitterPc) return false;    // not this program
        if (!RunEmitterCall(m, node, in.top, out, &gr)) return false;
        const u32 next = m.W(node, 2) & 0xFFFF;
        if (next == node) return false;
        node = next;
    }

    // Post-run data-memory state for the runtime: every mutated span.
    EmitDataWrites(s_mem.data(), in, out);
    return true;
}
