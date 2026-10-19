// SpriteText -- vs_sprite_text (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/sprite_text.h"

#include "engine/vu/vu_chop_round_guard.h"

using namespace vu1lib;

namespace {

constexpr u32 kEntryPc = 0x1748;    // the direct MSCAL entry
constexpr u32 kReentryPc = 0x1980;  // the MSCNT continuation (B 0x1748)
constexpr u32 kExitTpc = 0x1980;    // E-bit 0x1970 + delay
constexpr u32 kGlyphTableQw = 54;   // glyph records QW[54 + idx]
constexpr u16 kPacketBudget = 20;

}  // namespace

bool SpriteText::Run(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.start_pc != kEntryPc && in.start_pc != kReentryPc) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;
    auto W = [&](int vf, int k) -> u32& { return r.VF[vf].I[k]; };
    auto vi = [&](int n) -> u16& { return r.VI[n]; };

    // Header.
    vi(6) = (u16)in.top;                                  // XTOP vi06
    vi(3) = (u16)(vi(13) + vi(12));                       // IADD (base ptr slot)
    for (int k = 0; k < 4; ++k) W(3, k) = m.W(vi(6), k);  // LQI vf03 (RGBAQ tpl)
    vi(6)++;
    vi(3) = (u16)(m.W(vi(3), 0) & 0xFFFF);                // ILW.x (out base)
    for (int k = 0; k < 3; ++k) W(4, k) = m.W(vi(6), k);  // LQI.xyz vf04
    vi(6)++;
    vi(2) = kPacketBudget;                // packet budget
    vi(5) = vi(6);                        // list A ptr (top+2)
    vi(1) = vi(3);                        // packet base
    vi(4) = (u16)(W(4, 2) & 0xFFFF);      // MTIR vf04.z (outer n)
    vi(6) = (u16)(vi(6) + vi(4));         // list B ptr
    vi(3)++;                              // out write ptr

    int kicks = 0, glyph_guard = 0;
    bool first_entry = true;
    for (int entry_guard = 0; entry_guard < 512; ++entry_guard) {
        // Per-A-entry prologue. On re-entry from the outer IBNE the LQI vf05
        // is SKIPPED (the group-exhaust path already loaded the next B QW).
        if (first_entry) {
            for (int k = 0; k < 4; ++k) W(5, k) = m.W(vi(6), k);  // LQI vf05
            vi(6)++;
            first_entry = false;
        }
        for (int k = 0; k < 3; ++k) W(7, k) = m.W(vi(5), k);  // LQI.xyz vf07
        vi(5)++;
        r.R = 0x3F800000u;  // RINIT R, vf00.x (mantissa of +0)
        vi(8) = 255;
        vi(9) = 3;
        vi(7) = (u16)(W(7, 2) & 0xFFFF);  // MTIR vf07.z (group count)
        W(6, 2) = r.R;                    // RGET.z vf06
        // MINIx.w vf06, vf00, vf00x = fp_min(1.0, +0) = +0 (no flags).
        W(6, 3) = Fmin_raw(0x3F800000u, 0u);
        vi(10) = 4;
        // ADD.xy vf06 = vf07.xy + vf01.xy [FLAGS xy]; MTIR vi11 = vf05.x.
        W(6, 0) = f2bits(CF(CF(bits2f(W(7, 0))) + CF(bits2f(W(1, 0)))));
        W(6, 1) = f2bits(CF(CF(bits2f(W(7, 1))) + CF(bits2f(W(1, 1)))));
        PushMac(mac, status, r.VF[6].I, 0xC);
        vi(11) = (u16)(W(5, 0) & 0xFFFF);

        // Glyph loop + countdown block + kick block.
        bool next_entry = false, stopped = false;
        while (true) {
            if (++glyph_guard > 8192) return false;
            // --- glyph body ---
            W(7, 3) = Fmin_raw(0x3F800000u, 0u);  // MINIx.w vf07
            vi(11) = (u16)(vi(11) & vi(8));       // IAND 255
            V4 g;                                 // vf08 = glyph record
            for (int k = 0; k < 4; ++k) g.b[k] = m.W(kGlyphTableQw + vi(11), k);
            for (int k = 0; k < 4; ++k) W(8, k) = g.b[k];
            for (int k = 0; k < 4; ++k) W(9, k) = Ftoi0(W(6, k));  // FTOI0 vf09
            // MULx.z vf04 / MULy.w vf04 [FLAGS z, w]; RGET.z vf07.
            W(4, 2) = f2bits(CF(CF(bits2f(g.b[2])) * CF(bits2f(W(4, 0)))));
            PushMac(mac, status, r.VF[4].I, 0x2);
            W(4, 3) = f2bits(CF(CF(bits2f(g.b[3])) * CF(bits2f(W(4, 1)))));
            PushMac(mac, status, r.VF[4].I, 0x1);
            W(7, 2) = r.R;
            // SUBw.y vf10 = vf08.y - vf08.w [FLAGS y]; MR32 vf05.
            W(10, 1) = f2bits(CF(CF(bits2f(g.b[1])) - CF(bits2f(g.b[3]))));
            PushMac(mac, status, r.VF[10].I, 0x4);
            {
                const u32 a = W(5, 0), b = W(5, 1), c = W(5, 2), d = W(5, 3);
                W(5, 0) = b;
                W(5, 1) = c;
                W(5, 2) = d;
                W(5, 3) = a;
            }
            // ADDz.x vf10 = vf08.x + vf08.z [FLAGS x]; MTIR vi11 = vf08.x.
            W(10, 0) = f2bits(CF(CF(bits2f(g.b[0])) + CF(bits2f(g.b[2]))));
            PushMac(mac, status, r.VF[10].I, 0x8);
            vi(11) = (u16)(g.b[0] & 0xFFFF);
            // ADDz.x vf07 = vf06.x + vf04.z [FLAGS x]; vi11 = 3 & vi11.
            W(7, 0) = f2bits(CF(CF(bits2f(W(6, 0))) + CF(bits2f(W(4, 2)))));
            PushMac(mac, status, r.VF[7].I, 0x8);
            vi(11) = (u16)(vi(9) & vi(11));
            // ADDw.y vf07 = vf06.y + vf04.w [FLAGS y]; vi11 += vi14.
            W(7, 1) = f2bits(CF(CF(bits2f(W(6, 1))) + CF(bits2f(W(4, 3)))));
            PushMac(mac, status, r.VF[7].I, 0x4);
            vi(11) = (u16)(vi(14) + vi(11));
            vi(15) = (u16)(m.W(vi(11) + 5, 1) & 0xFFFF);  // ILW.y (cost)
            // FTOI0.xy vf11 = int(vf08.xy); SQ vf03 -> +1 (RGBAQ tpl).
            W(11, 0) = Ftoi0(g.b[0]);
            W(11, 1) = Ftoi0(g.b[1]);
            for (int k = 0; k < 4; ++k) m.SetW(vi(3) + 1, k, W(3, k));
            // The lower SQ reads the OLD vf09 (XYZ2 A) while the upper rewrites .xy.
            for (int k = 0; k < 4; ++k) m.SetW(vi(3) + 2, k, W(9, k));
            W(9, 0) = Ftoi0(W(10, 0));
            W(9, 1) = Ftoi0(W(10, 1));
            // FTOI0.xyzw vf08 = int(vf07); ILW.x vi11 = stride (overwrites).
            for (int k = 0; k < 4; ++k) W(8, k) = Ftoi0(W(7, k));
            vi(11) = (u16)(m.W(vi(11) + 5, 0) & 0xFFFF);
            vi(2) = (u16)(vi(2) - vi(15));  // ISUB budget
            m.SetW(vi(3), 0, W(11, 0));
            m.SetW(vi(3), 1, W(11, 1));  // UV0
            m.SetW(vi(3) + 3, 0, W(9, 0));
            m.SetW(vi(3) + 3, 1, W(9, 1));                              // UV1
            for (int k = 0; k < 4; ++k) m.SetW(vi(3) + 4, k, W(8, k));  // XYZ2 B
            const bool kick_now = (vi(2) == 0);                         // IBEQ
            // Delay slot: cursor advance [FLAGS x] + out stride.
            W(6, 0) = f2bits(CF(CF(bits2f(W(6, 0))) + CF(bits2f(W(4, 2)))));
            PushMac(mac, status, r.VF[6].I, 0x8);
            vi(3) = (u16)(vi(3) + vi(11));

            if (!kick_now) {
                // --- countdown block ---
            countdown:
                vi(10) = (u16)(vi(10) - 1);
                if (vi(10) != 0) {                     // IBNE -> the glyph body
                    vi(11) = (u16)(W(5, 0) & 0xFFFF);  // delay MTIR
                    continue;
                }
                for (int k = 0; k < 4; ++k) W(5, k) = m.W(vi(6), k);  // LQI vf05
                vi(6)++;
                vi(7) = (u16)(vi(7) - 1);
                vi(10) = 4;
                if (vi(7) != 0) {                      // IBNE -> the glyph body
                    vi(11) = (u16)(W(5, 0) & 0xFFFF);  // delay MTIR
                    continue;
                }
                vi(4) = (u16)(vi(4) - 1);
                if (vi(4) != 0) {  // IBNE -> the next A entry
                    next_entry = true;
                    break;
                }
                // fall through to the kick block (all data consumed)
            }
            // --- kick block ---
            {
                if (++kicks > 128) return false;
                vi(11) = kPacketBudget;
                u16 tag = 16384;
                vi(12) = (u16)(vi(12) + 1);
                vi(2) = (u16)(vi(11) - vi(2));  // nloop
                vi(11) = 1;
                tag = (u16)(tag + tag);            // 0x8000
                vi(12) = (u16)(vi(12) & vi(11));   // phase &= 1
                tag = (u16)(tag + vi(2));          // EOP | nloop
                vi(3) = tag;                       // (vi03 as temp)
                W(2, 0) = (tag & 0x8000) ? (0xFFFF0000u | tag) : tag;  // MFIR.x
                vi(2) = (u16)(vi(13) + vi(12));
                vi(3) = (u16)(m.W(vi(2), 0) & 0xFFFF);  // next out base
                vi(2) = kPacketBudget;                  // budget reset
                for (int k = 0; k < 4; ++k) m.SetW(vi(1), k, W(2, k));  // GIFtag
                if (!KickAt(m, vi(1), out)) return false;               // XGKICK
                vi(1) = vi(3);
                const bool more = (vi(4) != 0);  // IBNE
                vi(3) = (u16)(vi(3) + 1);        // delay slot
                if (more) goto countdown;        // resume the countdown block
                stopped = true;                  // E-bit
                break;
            }
        }
        if (stopped) break;
        if (!next_entry) return false;  // unreachable
    }

    r.mac_flag = mac;
    r.status_flag = status;
    r.TPC = kExitTpc;
    out.has_regs = true;

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}
