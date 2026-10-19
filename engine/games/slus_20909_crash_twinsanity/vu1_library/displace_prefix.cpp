// DisplacePrefix -- the shared-prefix displacement passes (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/displace_prefix.h"

#include "engine/vu/vu_chop_round_guard.h"

using namespace vu1lib;

namespace {

constexpr u32 kIntExitTpc = 0x1598;    // E-bit @0x1588 + delay slot
constexpr u32 kFloatExitTpc = 0x1720;  // E-bit @0x1710 + delay slot
constexpr int kLoopBound = 4096;       // the vertex walk terminates on its own sentinel

}  // namespace

bool DisplacePrefix::RunDisplaceInt(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;

    // Prologue.
    r.VI[13] = (uint16_t)(in.top & 0xFFFF);           // XTOP vi13
    V4 qw7;                                            // LQ vf07, 7(vi00)
    for (int k = 0; k < 4; ++k) qw7.b[k] = m.W(7, k);
    const u32 aux0 = qw7.b[0] & 0xFFFF;                // MTIR vi12, vf07.x
    const u32 count = m.W(r.VI[13] + 1, 0) & 0xFFFF;   // ILW.x vi10, 1(vi13)
    if ((count & 3) != 0 || count == 0) return false;  // IBNE stride-4 walk
    u32 v = (r.VI[13] + 3) & 0xFFFF;                   // IADDIU vi09, vi13, 3
    const u32 vend = (v + count) & 0xFFFF;             // IADD vi10
    r.VI[10] = (uint16_t)vend;
    // LQ.xy vf11, 2(vi13): lanes zw preserved.
    r.VF[11].I[0] = m.W(r.VI[13] + 2, 0);
    r.VF[11].I[1] = m.W(r.VI[13] + 2, 1);
    V4 row8;  // LQ vf08, 0(vi12)
    for (int k = 0; k < 4; ++k) row8.b[k] = m.W(aux0, k);
    for (int k = 0; k < 4; ++k) r.VF[8].I[k] = row8.b[k];
    // MR32 vf07 (x<-y, y<-z, z<-w, w<-x), then SQ -> QW7.
    r.VF[7].I[0] = qw7.b[1];
    r.VF[7].I[1] = qw7.b[2];
    r.VF[7].I[2] = qw7.b[3];
    r.VF[7].I[3] = qw7.b[0];
    u32 aux = (aux0 + 1) & 0xFFFF;                           // IADDI vi12, vi12, 1
    for (int k = 0; k < 4; ++k) m.SetW(7, k, r.VF[7].I[k]);  // SQ vf07, 7(vi00)

    // FMAC rule: flags from the RAW result, storage clamped.
    const float sx = CF(bits2f(r.VF[11].I[0]));  // vf11.x broadcast (read-clamped)
    const float sy = CF(bits2f(r.VF[11].I[1]));
    const float r8[4] = {CF(bits2f(row8.b[0])), CF(bits2f(row8.b[1])), CF(bits2f(row8.b[2])),
                         CF(bits2f(row8.b[3]))};

    // Loop, do-while on IBNE vi09 != vi10.
    for (int loop_guard = 0; loop_guard < kLoopBound; ++loop_guard) {
        u32 p0[4], p1[4], d0[4];
        for (int k = 0; k < 4; ++k) {
            p0[k] = m.W(v, k);      // LQ vf05, 0(vi09)
            p1[k] = m.W(v + 1, k);  // LQ vf06, 1(vi09)
            d0[k] = m.W(aux, k);    // LQ vf09, 0(vi12)
        }
        float f5[4], f6[4], f9[4];
        for (int k = 0; k < 4; ++k) {  // ITOF0 (raw, no flags)
            f5[k] = (float)(int32_t)p0[k];
            f6[k] = (float)(int32_t)p1[k];
            f9[k] = (float)(int32_t)d0[k];
        }
        u32 mac4[4];
        // MULx.xyzw vf05, vf05, vf11 (broadcast x).
        for (int k = 0; k < 4; ++k) {
            const float res = CF(f5[k]) * sx;
            mac4[k] = f2bits(res);
            f5[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xF);
        // MULy.xyzw vf06, vf06, vf11 (broadcast y).
        for (int k = 0; k < 4; ++k) {
            const float res = CF(f6[k]) * sy;
            mac4[k] = f2bits(res);
            f6[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xF);
        // MUL.xyz vf09, vf09, vf08.
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) * r8[k];
            mac4[k] = f2bits(res);
            f9[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // MULw.xyz vf09, vf09, vf08 (broadcast w).
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) * r8[3];
            mac4[k] = f2bits(res);
            f9[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // ADD.xyz vf05, vf09, vf05.
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) + CF(f5[k]);
            mac4[k] = f2bits(res);
            f5[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // Stores (raw bits of the clamped register lanes).
        for (int k = 0; k < 4; ++k) {
            m.SetW(v, k, f2bits(f5[k]));      // SQ vf05, 0(vi09)
            m.SetW(v + 1, k, f2bits(f6[k]));  // SQ vf06, 1(vi09)
            r.VF[5].I[k] = f2bits(f5[k]);
            r.VF[6].I[k] = f2bits(f6[k]);
            r.VF[9].I[k] = (k < 3) ? f2bits(f9[k]) : f2bits((float)(int32_t)d0[3]);
        }
        v = (v + 4) & 0xFFFF;                         // IADDIU vi09
        aux = (aux + 1) & 0xFFFF;                     // IADDIU vi12
        if (v == vend) break;                         // IBNE
        if (loop_guard == kLoopBound - 1) return false;  // runaway
    }

    r.VI[9] = (uint16_t)v;
    r.VI[12] = (uint16_t)aux;
    r.mac_flag = mac;
    r.status_flag = status;
    r.TPC = kIntExitTpc;
    out.has_regs = true;

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}

bool DisplacePrefix::RunDisplaceFloat(const Vu1HleInput& in, Vu1HleOutput& out) {
    if (in.regs == nullptr) return false;

    VuChopRoundGuard chop_guard;

    std::vector<u8>& s_mem = WorkingCopy(in);
    QwPtr m{s_mem.data()};

    out.regs = *in.regs;
    VUregs& r = out.regs;
    uint16_t mac = r.mac_flag, status = r.status_flag;

    // Prologue; vi13 INHERITED (no XTOP).
    const u32 base = r.VI[13] & 0xFFFF;
    V4 qw7;  // LQ vf07, 7(vi00)
    for (int k = 0; k < 4; ++k) qw7.b[k] = m.W(7, k);
    const u32 aux0 = qw7.b[0] & 0xFFFF;                // MTIR vi12, vf07.x
    const u32 count = m.W(base + 1, 0) & 0xFFFF;       // ILW.x vi10, 1(vi13)
    if ((count & 3) != 0 || count == 0) return false;  // IBNE stride-4 walk
    u32 v = (base + 3) & 0xFFFF;                       // IADDIU vi09, vi13, 3
    const u32 vend = (v + count) & 0xFFFF;             // IADD vi10
    r.VI[10] = (uint16_t)vend;
    // LQ.xy vf11, 2(vi13): lanes zw preserved (loaded, unused downstream).
    r.VF[11].I[0] = m.W(base + 2, 0);
    r.VF[11].I[1] = m.W(base + 2, 1);
    V4 row8;  // LQ vf08, 0(vi12)
    for (int k = 0; k < 4; ++k) row8.b[k] = m.W(aux0, k);
    for (int k = 0; k < 4; ++k) r.VF[8].I[k] = row8.b[k];
    // MR32 vf07 (x<-y, y<-z, z<-w, w<-x), then SQ -> QW7.
    r.VF[7].I[0] = qw7.b[1];
    r.VF[7].I[1] = qw7.b[2];
    r.VF[7].I[2] = qw7.b[3];
    r.VF[7].I[3] = qw7.b[0];
    u32 aux = (aux0 + 1) & 0xFFFF;                           // IADDI vi12, vi12, 1
    for (int k = 0; k < 4; ++k) m.SetW(7, k, r.VF[7].I[k]);  // SQ vf07, 7(vi00)

    const float r8[4] = {CF(bits2f(row8.b[0])), CF(bits2f(row8.b[1])), CF(bits2f(row8.b[2])),
                         CF(bits2f(row8.b[3]))};

    // Loop, do-while on IBNE vi09 != vi10.
    for (int loop_guard = 0; loop_guard < kLoopBound; ++loop_guard) {
        u32 p0[4], d0[4];
        for (int k = 0; k < 4; ++k) {
            p0[k] = m.W(v, k);    // LQ vf05, 0(vi09)
            d0[k] = m.W(aux, k);  // LQ vf09, 0(vi12)
        }
        float f9[4];
        for (int k = 0; k < 4; ++k)  // ITOF0 (raw, no flags)
            f9[k] = (float)(int32_t)d0[k];
        u32 mac4[4];
        // MUL.xyz vf09, vf09, vf08.
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) * r8[k];
            mac4[k] = f2bits(res);
            f9[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // MULw.xyz vf09, vf09, vf08 (broadcast w).
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) * r8[3];
            mac4[k] = f2bits(res);
            f9[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // ADD.xyz vf05, vf09, vf05 (vertex lanes read-clamped).
        float f5[3];
        for (int k = 0; k < 3; ++k) {
            const float res = CF(f9[k]) + CF(bits2f(p0[k]));
            mac4[k] = f2bits(res);
            f5[k] = CF(res);
        }
        PushMac(mac, status, mac4, 0xE);
        // SQ.xyzw vf05, 0(vi09): xyz computed, w = the raw loaded bits.
        for (int k = 0; k < 3; ++k) {
            m.SetW(v, k, f2bits(f5[k]));
            r.VF[5].I[k] = f2bits(f5[k]);
        }
        m.SetW(v, 3, p0[3]);
        r.VF[5].I[3] = p0[3];
        for (int k = 0; k < 4; ++k) r.VF[9].I[k] = (k < 3) ? f2bits(f9[k]) : f2bits((float)(int32_t)d0[3]);
        v = (v + 4) & 0xFFFF;                            // IADDIU vi09
        aux = (aux + 1) & 0xFFFF;                        // IADDIU vi12
        if (v == vend) break;                            // IBNE
        if (loop_guard == kLoopBound - 1) return false;  // runaway
    }

    r.VI[9] = (uint16_t)v;
    r.VI[12] = (uint16_t)aux;
    r.mac_flag = mac;
    r.status_flag = status;
    r.TPC = kFloatExitTpc;
    out.has_regs = true;

    EmitDataWrites(s_mem.data(), in, out);
    return true;
}
