// PoscolorStrip -- vs_poscolor_strip (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/poscolor_strip.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

#include "engine/vu/vu_chop_round_guard.h"

using namespace vu1lib;

namespace {

constexpr u32 kEmitterPc = 0x1720;   // the material sub (= the variant base)
constexpr u32 kExitPc = 0x90;        // the E-bit exit node
constexpr u32 kGateHelperPc = 0xB0;  // the shared clip gate (QW4.y * 8)
constexpr u32 kGateLink = 0x2F2;     // (gate JALR pc + 16) / 8 of this body
constexpr u16 kBodyVi13 = 0x2E4;     // the dispatcher JALR target in 8-byte units (never rewritten)
constexpr u32 kFanPc = 0x19b8;       // the pos-colour fan emitter entry
// Images whose fan region is byte-identical to the one the near-clip lift was
// coded from (full clips lift only for these).
constexpr u64 kClipImages[] = {
    0x96723bb41f7ab94eull, 0xf22fc9e6caa37878ull, 0x0ad0b3ea144db975ull, 0x8a89004303f71085ull,
    0x0dfd559dbcd2da4eull, 0x198f0b7cf0758c73ull, 0xf1fbb7b64349d200ull, 0x03bf7b610b28fafcull,
    0x5a96fb13a9285441ull, 0xdb4d58203745c760ull, 0x3690a7b6331df29full,
};

}  // namespace

bool PoscolorStrip::RunPosColorCall(QwPtr m, u32 node, u32 top, ClipGate::GateRegs& gr, Vu1HleOutput& out) {
    auto load = [&](u32 qw) {
        V4 v;
        for (int k = 0; k < 4; ++k) v.b[k] = m.W(qw, k);
        return v;
    };
    auto store = [&](u32 qw, const V4& v) {
        for (int k = 0; k < 4; ++k) m.SetW(qw, k, v.b[k]);
    };
    auto store_xyz = [&](u32 qw, const V4& v) {
        for (int k = 0; k < 3; ++k) m.SetW(qw, k, v.b[k]);
    };

    // Matrix base + rows + unconditional clip-gate call.
    const u32 base = m.W(3, 0) & 0xFFFF;             // ILW.x vi09, 3(vi00)
    const u32 helper_pc = (m.W(4, 1) & 0xFFFF) * 8;  // ILW.y vi12, 4(vi00)
    if (helper_pc != kGateHelperPc) return false;    // unexpected prefix layout
    V4 obj[4], clipR[5];
    for (int r = 0; r < 4; ++r) obj[r] = load(base + 78 + r);    // vf01..vf04
    for (int r = 0; r < 5; ++r) clipR[r] = load(base + 82 + r);  // vf08..vf12

    const u32 vqw_count = m.W(top + 1, 0) & 0xFFFF;  // via MR32+MTIR vf07
    if ((vqw_count & 3) != 0) return false;
    const u32 nverts = vqw_count / 4;
    if (nverts < 3) return false;  // micro do-while needs >= 3
    const u32 vbase = top + 3;

    gr.vi09 = (u16)base;  // body pins at gate time
    gr.vi13 = kBodyVi13;
    for (int r = 0; r < 4; ++r) gr.obj[r] = obj[r];
    if (!ClipGate::ClipGateB0(m, node, top, kGateLink, clipR, nverts, &gr, &out)) return false;

    // Header reads + GIFtag template write.
    const u32 out_base = m.W(1, 0) & 0xFFFF;  // ILW.x vi11, 1(vi00)
    const V4 payload = load(node + 2);        // LQ vf05, 2(vi14)
    for (int k = 0; k < 4; ++k) m.SetW(out_base, k, m.W(top, k));

    auto xform = [&](const V4& v) {
        return XformEmit(obj[0], obj[1], obj[2], obj[3], CF(bits2f(v.b[0])), CF(bits2f(v.b[1])), CF(bits2f(v.b[2])));
    };
    auto adc_of = [&](const V4& p, const V4& c) {  // MTIR/MTIR/IOR/MFIR.w
        const u32 a16 = (p.b[3] & 0xFFFF) | (c.b[3] & 0xFFFF);
        return (a16 & 0x8000) ? (0xFFFF0000u | a16) : a16;
    };
    auto stq_of = [&](const V4& c, float q) {  // ADD.xyz vf05, then MULq.xyz
        V4 r{};
        for (int k = 0; k < 3; ++k) {
            const float s = CF(CF(bits2f(c.b[k])) + CF(bits2f(payload.b[k])));
            r.b[k] = f2bits(CF(s * q));
        }
        return r;
    };
    auto mulq3 = [&](const V4& v, float q) {  // MULq.xyz (xyz lanes only)
        V4 r{};
        for (int k = 0; k < 3; ++k) r.b[k] = f2bits(CF(CF(bits2f(v.b[k])) * q));
        return r;
    };
    auto xyz2_of = [&](const V4& scr, u32 adc) {  // FTOI0.xyz + ADC word
        V4 r;
        for (int k = 0; k < 3; ++k) r.b[k] = Ftoi0(scr.b[k]);
        r.b[3] = adc;
        return r;
    };

    // Prologue: v0 and v1.
    V4 p_a = load(vbase), c_a = load(vbase + 1);      // vf09 / vf08
    u32 adc_a = adc_of(p_a, c_a);
    V4 T_a = xform(p_a);                              // -> vf09
    V4 p_b = load(vbase + 4), c_b = load(vbase + 5);  // vf07 / vf06
    float Q_a = VuDiv(1.0f, bits2f(T_a.b[3]));        // DIV Q0
    V4 T_b = xform(p_b);                              // -> vf09
    u32 adc_b = adc_of(p_b, c_b);
    store(out_base + 2, c_a);  // RGBAQ v0 (raw)
    // MULq|DIV: the fdiv-pipe lower commits pending Q0 pre-exec.
    V4 scr_a = mulq3(T_a, Q_a);                 // vf10 = screen0 floats
    float Q_b = VuDiv(1.0f, bits2f(T_b.b[3]));  // DIV Q1
    V4 st_a = stq_of(c_a, Q_a);                 // (3 after DIV -> arch Q0)
    store(out_base + 5, c_b);                   // RGBAQ v1 (raw)

    // Loop i = 0..N-3: loads v_{i+2}, stores v_i outputs and RGBAQ(v_{i+2}).
    u32 vi06 = vbase + 8;
    const u32 vend = vbase + vqw_count;
    u32 oslot = out_base + 4;  // vi11 at loop entry
    for (u32 loop_guard = 0;; ++loop_guard) {
        if (loop_guard > 1024) return false;        // runaway (bad count)
        V4 p_n = load(vi06), c_n = load(vi06 + 1);  // vf11 / vf10
        const u32 adc_n = adc_of(p_n, c_n);
        store(oslot - 1, xyz2_of(scr_a, adc_a));    // XYZ2 v_i
        store_xyz(oslot - 3, st_a);                 // ST v_i
        V4 scr_b = mulq3(T_b, Q_b);                 // screen v_{i+1}
        store(oslot + 4, c_n);                      // RGBAQ v_{i+2}
        V4 st_b = stq_of(c_b, Q_b);
        V4 T_n = xform(p_n);                        // -> vf09
        const float Q_n = VuDiv(1.0f, bits2f(T_n.b[3]));  // (always)
        vi06 += 4;
        oslot += 3;  // (delay -- always)
        adc_a = adc_b;
        adc_b = adc_n;
        st_a = st_b;
        scr_a = scr_b;
        c_b = c_n;
        T_b = T_n;
        Q_b = Q_n;
        if (vi06 == vend) break;  // IBNE
    }

    // Epilogue. WAITQ commits the pending Q_{N-1}.
    V4 scr_last = mulq3(T_b, Q_b);               // screen v_{N-1}
    V4 st_last = stq_of(c_b, Q_b);
    store_xyz(oslot - 3, st_a);                  // ST v_{N-2}
    store(oslot - 1, xyz2_of(scr_a, adc_a));     // XYZ2 v_{N-2}
    oslot += 3;
    store_xyz(oslot - 3, st_last);               // ST v_{N-1}
    store(oslot - 1, xyz2_of(scr_last, adc_b));  // XYZ2 v_{N-1}

    // Kicks: node-embedded state packet first (node.w != 0), then the main packet.
    const u32 node_w = m.W(node, 3) & 0xFFFF;  // ILW.w vi03, 0(vi14)
    if (node_w != 0) {
        if (!KickAt(m, node + 3, out)) return false;
    }
    if (!KickAt(m, out_base, out)) return false;

    // Tail: QW1 MR32 output double-buffer rotation.
    RotateQw1(m);
    return true;
}

bool PoscolorStrip::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != 0) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    // Dispatcher prologue beacon (shared family prefix).
    m.SetW(9, 0, 1);

    // Identity-verified for the shader-type-0A program at the emitter pc (the
    // layout analysis), else one of the pinned images (no analysis attached).
    bool clip_image_ok = Vu1LoadedPrograms::At(in, kEmitterPc) == Vu1LoadedPrograms::kType0A;
    for (u64 h : kClipImages) clip_image_ok = clip_image_ok || in.image_hash == h;
    ClipGate::GateRegs gr = ClipGate::SeedGateRegs(in, kFanPc, clip_image_ok);

    u32 node = m.W(3, 0) & 0xFFFF;
    for (int guard = 0; guard < 16; ++guard) {
        const u32 call_pc = (m.W(node, 0) & 0xFFFF) * 8;
        if (call_pc == kExitPc) break;  // E-bit exit node
        if (call_pc != kEmitterPc) {
            // Stale dispatch into the zero-fill tail past the image extent:
            // zero words decode as architectural no-ops, the PC slides off the
            // end of micro memory, wraps into the dispatcher, re-writes the
            // idempotent QW9.x beacon and re-JALRs the same node forever -- the
            // interpreter spins to its cycle backstop having kicked and written
            // NOTHING further. Lift = stop the node walk here, but only when
            // every micro byte from the call target to the end is zero AND
            // nothing was kicked yet (a prior emitter node would be re-walked
            // after the wrap, re-emitting kicks forever -- decline that shape).
            if (!out.kicks.empty()) return false;
            if (call_pc <= kEmitterPc) return false;
            for (u32 b = call_pc; b < kVu1HleMicroSize; ++b)
                if (in.micro[b] != 0) return false;
            break;  // spin-to-backstop
        }
        if (!RunPosColorCall(m, node, in.top, gr, out)) return false;
        const u32 next = m.W(node, 2) & 0xFFFF;
        if (next == node) return false;
        node = next;
    }

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}
