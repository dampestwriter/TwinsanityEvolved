// IndexedDisplace -- vs_indexed_displace (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/indexed_displace.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"
#include "engine/vu/vu_float.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;           // the material sub (= the variant base)
constexpr u32 kExitPc = 0x90;                // the E-bit exit node
constexpr u32 kGateHelperPc = 0xB0;          // the shared clip gate (QW4.y * 8)
constexpr u32 kBroadcastGateLink = 0x335;    // (gate JALR pc + 16) / 8 of the broadcast body
constexpr u32 kComponentGateLink = 0x336;    // ... of the componentwise body
constexpr u32 kFanPc = 0x1be8;               // the relocated shared fan emitter entry
constexpr u32 kScaleImmediate = 0x39c38f36u;  // ADDi.xyz vf13, vf00, I (~ 1/2681)
// Images whose fan region is byte-identical to the one the near-clip lift
// was coded from (full clips lift only for these).
constexpr u64 kClipImages[] = {0x71a31d9ebcf1e05dull, 0x89e7fa6dec83cf88ull};

V4 Load(QwPtr m, u32 qw) {
    V4 v;
    for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
    return v;
}

}  // namespace

void IndexedDisplace::RunIndexPrepass(QwPtr m, u32 node, u32 top, u32 nverts, bool componentwise) {
    const V4 payload = Load(m, node + 2);  // LQ vf05, 2(vi14)
    const V4 nscale = Load(m, node + 3);   // LQ vf16, 3(vi14) (componentwise only)
    // ADDi.xyz vf13, vf00, I -- CF(0 + CF(I)).
    const float scale_i = CF(0.0f + CF(bits2f(kScaleImmediate)));
    const u32 vbase = top + 3;

    // 1-deep pending store: the micro stores result_{j-1} AFTER gather_j.
    bool have_pending = false;
    u32 pending_qw = 0;
    u32 pending[3] = {0, 0, 0};
    for (u32 j = 0; j < nverts; ++j) {
        const u32 vq = vbase + j * 4;
        V4 pos;
        for (int k = 0; k < 4; ++k) pos.b[k] = m.W(vq, k);
        // scaled = pos.xyz * I; idx.L = low4(low16(rawbits)); lane-matched gather.
        u32 gather[3];
        for (int k = 0; k < 3; ++k) {
            const float sc = CF(CF(bits2f(pos.b[k])) * scale_i);
            const u32 idx_l = f2bits(sc) & 15u;    // MTIR low16 then IAND 15
            gather[k] = m.W(node + 3 + idx_l, k);  // LQ.<lane> 3(vi)
        }
        if (have_pending)
            for (int k = 0; k < 3; ++k) m.SetW(pending_qw, k, pending[k]);
        // ACC = gather * scale (write-clamped); result = CF(fma(pos, 1, ACC)).
        for (int k = 0; k < 3; ++k) {
            const float s = componentwise ? CF(bits2f(nscale.b[k]))    // MULA vf16
                                          : CF(bits2f(payload.b[3]));  // MULAw vf05w
            const float acc = CF(CF(bits2f(gather[k])) * s);
            pending[k] = f2bits(CF(VuFloat::Fma(CF(bits2f(pos.b[k])), 1.0f, acc)));
        }
        pending_qw = vq;
        have_pending = true;
    }
    if (have_pending)
        for (int k = 0; k < 3; ++k) m.SetW(pending_qw, k, pending[k]);
}

bool IndexedDisplace::RunIdxCall(QwPtr m, u32 node, u32 top, bool componentwise, u32 gate_link,
                                 Vu1HleOutput& out, ClipGate::GateRegs* gr) {
    auto load = [&](u32 qw) { return Load(m, qw); };

    const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // via MR32+MTIR vf07
    if ((vqw_count & 3) != 0) return false;
    const u32 nverts = vqw_count / 4;
    if (nverts < 3) return false;  // micro pipeline needs >= 3
    const u32 vbase = top + 3;

    u32 mesh = m.W(2, 0) & 0xFFFF;               // ILW.x vi13, 2(vi00)
    const u32 mesh_end = m.W(mesh, 1) & 0xFFFF;  // ILW.y vi10, 0(vi13) (once)

    // Pre-pass: first 0x1720 call of the MSCAL (dispatcher beacon QW9.x = 1).
    if ((m.W(9, 0) & 0xFFFF) != 0) {  // ILW.x vi08, 9(vi00)
        RunIndexPrepass(m, node, top, nverts, componentwise);
        m.SetW(9, 0, 0);  // ISW.x vi01(=0), 9(vi00)
    }

    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)

    for (int mesh_guard = 0; mesh_guard < 64; ++mesh_guard) {
        const u32 gate_flag = m.W(mesh, 0) & 0xFFFF;  // ILW.x vi11/vi01
        V4 obj[4];
        for (int r = 0; r < 4; ++r) obj[r] = load(mesh + 1 + r);

        if (gate_flag != 0) {
            const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
            if (helper_pc != kGateHelperPc) return false;
            const u32 clip_off = m.W(mesh, 2) & 0xFFFF;  // ILW.z vi03
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

        const u32 out_base = m.W(1, 0) & 0xFFFF;                       // ILW.x vi05, 1(vi00)
        const V4 payload = load(node + 2);                             // LQ vf05, 2(vi14)
        for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));  // GIFtag

        for (u32 j = 0; j < nverts; ++j) {
            const u32 vq = vbase + j * 4;
            const V4 pos = load(vq);      // gate-rewritten .w
            const V4 aux = load(vq + 1);  // RGBA colour, .w aux

            const V4 T = XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(pos.b[0])), CF(bits2f(pos.b[1])),
                                   CF(bits2f(pos.b[2])));
            const float Q = VuDiv(1.0f, bits2f(T.b[3]));

            // ADC: sext16((low16(pos.w) & flag) | low16(aux.w)).
            const u32 adc16 = ((pos.b[3] & 0xFFFF) & gate_flag) | (aux.b[3] & 0xFFFF);
            const u32 adc = (adc16 & 0x8000) ? (0xFFFF0000u | adc16) : adc16;

            const u32 st_qw = out_base + 1 + 3 * j;
            for (int k = 0; k < 3; ++k) {
                const float stp = CF(CF(bits2f(aux.b[k])) + CF(bits2f(payload.b[k])));
                m.SetW(st_qw, k, f2bits(CF(CF(stp) * Q)));
            }
            for (int k = 0; k < 4; ++k) m.SetW(st_qw + 1, k, aux.b[k]);  // raw
            m.SetW(st_qw + 2, 0, Ftoi0(f2bits(CF(CF(bits2f(T.b[0])) * Q))));
            m.SetW(st_qw + 2, 1, Ftoi0(f2bits(CF(CF(bits2f(T.b[1])) * Q))));
            m.SetW(st_qw + 2, 2, Ftoi0(f2bits(CF(CF(bits2f(T.b[2])) * Q))));
            m.SetW(st_qw + 2, 3, adc);
        }

        // Kicks: node packet first when node.w != 0.
        if (node_w != 0) {
            if (!KickAt(m, node + 3, out)) return false;
        }
        if (!KickAt(m, out_base, out)) return false;

        RotateQw1(m);
        mesh = (mesh + (m.W(mesh, 3) & 0xFFFF)) & 0xFFFF;
        if (mesh == mesh_end) return true;
    }
    return false;  // runaway mesh list
}

bool IndexedDisplace::Run(const Vu1HleInput& in, Vu1HleOutput& out, bool componentwise, u32 gate_link) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    m.SetW(9, 0, 1);  // dispatcher prologue beacon (gates the pre-pass)

    // The body never writes vf16/vf18, so the entry-seeded pass-through holds
    // for the whole run; the fan is the shared textured fan relocated (kind 0).
    // Identity-verified for the shader-type-17 (broadcast) program at the
    // emitter pc -- the body the lift was coded from; the componentwise
    // program (type 0x1A) keeps the pinned-image rule only.
    bool clip_image_ok = !componentwise && Vu1LoadedPrograms::At(in, kEmitterPc) == Vu1LoadedPrograms::kType17;
    for (u64 h : kClipImages) clip_image_ok = clip_image_ok || in.image_hash == h;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kFanPc, clip_image_ok);

    u32 node = m.W(3, 0) & 0xFFFF;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;
        if (call_pc != kEmitterPc) return false;
        if (!RunIdxCall(m, node, in.top, componentwise, gate_link, out, &gr)) return false;
        const u32 next = m.W(node, 2) & 0xFFFF;
        if (next == node) return false;
        node = next;
    }

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}

bool IndexedDisplace::RunBroadcast(const Vu1HleInput& in, Vu1HleOutput& out) {
    return Run(in, out, /*componentwise=*/false, kBroadcastGateLink);
}

bool IndexedDisplace::RunComponentwise(const Vu1HleInput& in, Vu1HleOutput& out) {
    return Run(in, out, /*componentwise=*/true, kComponentGateLink);
}
