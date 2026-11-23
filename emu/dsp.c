// SH3-DSP parallel-processing unit (written from scratch; instruction FORMATS follow the public SH-DSP encoding
// tables, SEMANTICS (flag rules, rounding, shifts) are best-effort and NOT verified on hardware).
#include "sh3.h"
#include <string.h>

uint64_t dsp_hist[256];          // ALU opcode (hi byte of ALU word) execution counts, for debugging
uint64_t dsp_hist_unimpl[256];

// X/Y memory transfers (addresses 0xA5000000..0xA501FFFF) go straight to the memory window when it is enabled (it is bypassed while a
// watchpoint is active so the bus can see every access)
static inline uint32_t rd16x(SH3 *c, uint32_t a) {
    const FastWin *w = &c->win[2];
    if (w->ptr && (a >> 17) == (0xA5000000u >> 17)) { uint16_t h; memcpy(&h, w->ptr + (a & 0x1FFFEu), 2); return h; }
    return c->bus.read(c->bus.ctx, a, 2);
}
static inline void wr16x(SH3 *c, uint32_t a, uint32_t v) {
    const FastWin *w = &c->win[2];
    if (w->ptr && w->writable && (a >> 17) == (0xA5000000u >> 17)) { uint16_t h = (uint16_t)v; memcpy(w->ptr + (a & 0x1FFFEu), &h, 2); return; }
    c->bus.write(c->bus.ctx, a, v, 2);
}
#define RD16(a) rd16x(c, (a))
#define WR16(a,v) wr16x(c, (a), (v))

static int64_t sx40(int64_t v) { return (v << 24) >> 24; }          // sign-extend 40 bits
/* 40-bit view of a register by Dn/Ds code: A0=7 A1=5 accumulators carry guard bits; others sign-extended 32 */
static int64_t rd40(SH3 *c, int code) {
    switch (code) {
    case 7: return sx40(((int64_t)(int8_t)c->a0g << 32) | c->a0);
    case 5: return sx40(((int64_t)(int8_t)c->a1g << 32) | c->a1);
    case 8: return (int32_t)c->x0; case 9: return (int32_t)c->x1; case 10: return (int32_t)c->y0; case 11: return (int32_t)c->y1;
    case 12: return (int32_t)c->m0; case 14: return (int32_t)c->m1;
    }
    return 0;
}
static int valid_dn(int code) { return code == 5 || code == 7 || (code >= 8 && code <= 12) || code == 14; }
static void wr40(SH3 *c, int code, int64_t v) {
    uint32_t lo = (uint32_t)v; uint8_t g = (uint8_t)(v >> 32);
    switch (code) {
    case 7: c->a0 = lo; c->a0g = g; break; case 5: c->a1 = lo; c->a1g = g; break;
    case 8: c->x0 = lo; break; case 9: c->x1 = lo; break; case 10: c->y0 = lo; break; case 11: c->y1 = lo; break;
    case 12: c->m0 = lo; break; case 14: c->m1 = lo; break;
    }
}
static int64_t sel_x(SH3 *c, int s) { static const int t[4] = {8, 9, 7, 5}; return rd40(c, t[s & 3]); }     // x0 x1 a0 a1
static int64_t sel_y(SH3 *c, int s) { static const int t[4] = {10, 11, 12, 14}; return rd40(c, t[s & 3]); }  // y0 y1 m0 m1

/* ===== ALU core, following the SH-3/SH-3E/SH3-DSP Software Manual (REJ09B0317), sections 5.1-5.9 =====
   DSR: bit0 DC, bits3:1 CS, bit4 V, bit5 N, bit6 Z, bit7 GT.   SR: bit1 S (saturate), bit10 DMX, bit11 DMY, bit12 DSP mode. */
#define SR_S_BIT 2u
static int sat_on(SH3 *c) { return (c->sr & SR_S_BIT) != 0; }
/* write a 40-bit value: accumulators keep the guard byte, other registers take the low 32 bits */
static void put40(SH3 *c, int dn, int64_t v) { wr40(c, dn, v); }
static void dsr_set(SH3 *c, int N, int Z, int V, int GT, int dc) {
    c->dsr = (c->dsr & ~0xF1u) | (V ? 0x10u : 0) | (N ? 0x20u : 0) | (Z ? 0x40u : 0) | (GT ? 0x80u : 0) | (dc ? 1u : 0);
}
static int dc_select(SH3 *c, int N, int Z, int V, int carry_or_shout, int logical, int shift) {
    switch ((c->dsr >> 1) & 7) {
    case 0: return carry_or_shout;                         // carry/borrow (logical: 0; shifts: last bit out)
    case 1: return N;
    case 2: return Z;
    case 3: return logical ? 0 : V;
    case 4: return (logical || shift) ? 0 : !((N ^ V) | Z);   // PSHA / PSHL: GT and GE modes always give 0 (manual 5.5)
    case 5: return (logical || shift) ? 0 : !(N ^ V);
    }
    return 0;
}
/* arithmetic-class commit (add/sub/copy/abs/neg/clr/inc/dec/rnd/psha).  v = untruncated result.  flags: update DSR (unconditional instrs only).
   dc_override: -1 = by CS bits; 0/1 = force (PADDC/PSUBC carry/borrow).  gt_zero: shifts always clear GT. */
static void arith_commit(SH3 *c, int dn, int64_t v, int carry, int flags, int dc_override, int gt_zero) {
    v = sx40(v);
    int ovf = (v > 0x7FFFFFFFLL || v < -0x80000000LL);
    if (ovf && sat_on(c)) { v = v > 0 ? 0x7FFFFFFFLL : -0x80000000LL; ovf = 0; }
    if (dn >= 0) put40(c, dn, v);
    if (!flags) return;
    int N = (int)((v >> 39) & 1), Z = (v == 0), V = ovf, GT = gt_zero ? 0 : !((N ^ V) | Z);
    int dc = dc_override >= 0 ? dc_override : dc_select(c, N, Z, V, carry, 0, gt_zero);
    dsr_set(c, N, Z, V, GT, dc);
}
/* integer-class commit (PINC/PDEC): 24-bit operation (top word + guard); overflow bounds are the 16-bit top word, low word cleared */
static void int_commit(SH3 *c, int dn, int64_t v, int carry, int flags) {
    v = sx40(v);
    int ovf = (v > 0x7FFF0000LL || v < -0x80000000LL);
    if (ovf && sat_on(c)) { v = v > 0 ? 0x7FFF0000LL : -0x80000000LL; ovf = 0; }
    put40(c, dn, v);
    if (!flags) return;
    int N = (int)((v >> 39) & 1), Z = (v == 0), GT = !((N ^ ovf) | Z);
    dsr_set(c, N, Z, ovf, GT, dc_select(c, N, Z, ovf, carry, 0, 0));
}
/* PDMSB: shift amount that normalises the 40-bit source: +31 (zero/all ones) .. 0 (MSB at bit 30) .. -8 (MSB at bit 38) */
static void pdmsb_commit(SH3 *c, int dn, int64_t src, int flags) {
    int64_t y = src < 0 ? ~src : src; int k = -1;
    for (int b = 38; b >= 0; b--) if ((y >> b) & 1) { k = b; break; }
    int r = 30 - k;
    put40(c, dn, (int64_t)r * 65536);
    if (!flags) return;
    int N = r < 0, Z = r == 0, GT = r > 0, dc = 0;
    switch ((c->dsr >> 1) & 7) { case 1: dc = N; break; case 2: dc = Z; break; case 4: dc = GT; break; case 5: dc = !N; break; default: dc = 0; }
    dsr_set(c, N, Z, 0, GT, dc);
}

/* logical-class commit: 16-bit top-word result r16, guard and low word cleared */
static void logic_commit(SH3 *c, int dn, uint16_t r16, int shout, int flags) {
    int64_t v = (int64_t)r16 << 16;                         // positive 40-bit (guard = 0)
    put40(c, dn, v);
    if (!flags) return;
    int N = (r16 >> 15) & 1, Z = (r16 == 0);
    dsr_set(c, N, Z, 0, 0, dc_select(c, N, Z, 0, shout, 1, 0));
}

/* returns 0 ok, 1 = unimplemented.  mode: 1 = unconditional, 2 = DCT, 3 = DCF */
static int ppi(SH3 *c, uint16_t f) {
    int hi = f >> 8;
    int xx = (f >> 6) & 3, yy = (f >> 4) & 3, dn = f & 15;
    // ---- PSHA/PSHL with immediate ----
    if ((f & 0xE800) == 0) {
        int arith = (f & 0x1000) != 0;
        if (!valid_dn(dn)) return 1;
        int64_t v = rd40(c, dn);
        if (arith) {                                                   // PSHA #imm7 (-32..+32), unconditional
            int imm = (f >> 4) & 127; if (imm & 64) imm -= 128;
            int64_t r; int shout = 0;
            if (imm >= 0) { r = (int64_t)((uint64_t)v << imm); shout = imm ? (int)((v >> (40 - imm)) & 1) : 0; } else { int k = -imm; r = v >> k; shout = (int)((v >> (k - 1)) & 1); }
            arith_commit(c, dn, r, shout, 1, -1, 1);
        } else {                                                       // PSHL #imm6 (-16..+16)
            int imm = (f >> 4) & 63; if (imm & 32) imm -= 64;
            uint16_t t = (uint16_t)(v >> 16); uint32_t r; int shout = 0;
            if (imm >= 0) { r = imm >= 16 ? 0 : ((uint32_t)t << imm) & 0xFFFF; shout = imm ? ((imm <= 16) ? (t >> (16 - imm)) & 1 : 0) : 0; }
            else { int k = -imm; r = k >= 16 ? 0 : t >> k; shout = (t >> (k - 1)) & 1; }
            logic_commit(c, dn, (uint16_t)r, shout, 1);
        }
        return 0;
    }
    // ---- PMULS (optionally with a parallel PADD/PSUB) ----
    if ((f & 0xC000) == 0x4000 && (f & 0x3000) != 0x1000) {
        static const int du_t[4] = {8, 10, 7, 5}, se_t[4] = {8, 9, 10, 5}, sf_t[4] = {10, 11, 8, 5}, sg_t[4] = {12, 14, 7, 5};
        int64_t se = rd40(c, se_t[(f >> 10) & 3]), sf = rd40(c, sf_t[(f >> 8) & 3]);
        int64_t a = sel_x(c, xx), b = sel_y(c, yy); int du = du_t[f & 3], par = (f & 0x2000) != 0, add = (f & 0x1000) != 0;
        int64_t p = ((int64_t)(int16_t)(se >> 16) * (int16_t)(sf >> 16)) << 1;
        if (p == 0x80000000LL) p = sat_on(c) ? 0x7FFFFFFFLL : -0x80000000LL;      // only H'8000 x H'8000 overflows
        if (par) { int64_t r = add ? a + b : a - b; int carry = add ? (((uint64_t)(uint32_t)a + (uint32_t)b) >> 32) & 1 : ((uint32_t)a < (uint32_t)b); arith_commit(c, du, r, carry, 1, -1, 0); }
        wr40(c, sg_t[(f >> 2) & 3], (int32_t)p);                                  // 32-bit result, sign-extended into guard bits
        return 0;
    }
    int nib3 = hi & 3;
    int key = (f >> 8) & 0xFF;
    int64_t a = sel_x(c, xx), b = sel_y(c, yy); int cond;
    // ---- unconditional-only group (CS bits field 00 in the opcode) ----
    if (nib3 == 0) {
        switch (key) {
        case 0xA0: if (!valid_dn(dn)) return 1; { int bw = (c->dsr & 1); int64_t r = a - b - bw; int borrow = ((uint64_t)(uint32_t)a < (uint64_t)(uint32_t)b + bw); arith_commit(c, dn, r, borrow, 1, borrow, 0); } return 0;   // PSUBC
        case 0xB0: if (!valid_dn(dn)) return 1; { int cw = (c->dsr & 1); int64_t r = a + b + cw; int carry = (int)((((uint64_t)(uint32_t)a + (uint32_t)b + cw) >> 32) & 1); arith_commit(c, dn, r, carry, 1, carry, 0); } return 0;   // PADDC
        case 0x84: { int64_t r = sx40(a - b); int borrow = ((uint32_t)a < (uint32_t)b); arith_commit(c, -1, r, borrow, 1, -1, 0); } return 0;                                                                     // PCMP
        case 0x88: case 0xA8: { int64_t s = (key == 0x88) ? a : b; if (!valid_dn(dn)) return 1; arith_commit(c, dn, s < 0 ? -s : s, (s < 0 && (uint32_t)s != 0), 1, -1, 0); } return 0;                                              // PABS
        case 0x98: case 0xB8: { int64_t s = (key == 0x98) ? a : b; if (!valid_dn(dn)) return 1; int64_t r = s + 0x8000; if (sx40(r) > 0x7FFFFFFFLL && sat_on(c)) r = 0x7FFFFFFFLL; arith_commit(c, dn, r & ~0xFFFFLL, (int)((((uint64_t)(uint32_t)s + 0x8000u) >> 32) & 1), 1, -1, 0); } return 0;   // PRND
        default: return 1;
        }
    }
    // ---- conditional-capable group ----
    cond = nib3;                                                        // 1 = unconditional, 2 = DCT, 3 = DCF
    if (cond == 2 && !(c->dsr & 1)) return 0;
    if (cond == 3 && (c->dsr & 1)) return 0;
    int flags = (cond == 1);                                            // conditional instructions never update DC/N/Z/V/GT
    key = ((f >> 12) << 4) | (((f >> 8) & 0xF) & ~3) | 1;
    switch (key) {
    case 0xB1: if (!valid_dn(dn)) return 1; arith_commit(c, dn, a + b, (int)((((uint64_t)(uint32_t)a + (uint32_t)b) >> 32) & 1), flags, -1, 0); return 0;   // PADD
    case 0xA1: if (!valid_dn(dn)) return 1; arith_commit(c, dn, a - b, (uint32_t)a < (uint32_t)b, flags, -1, 0); return 0;                                        // PSUB
    case 0x95: case 0xA5: case 0xB5: {                                                                                                                        // PAND PXOR POR
        if (!valid_dn(dn)) return 1; uint16_t ta = (uint16_t)(a >> 16), tb = (uint16_t)(b >> 16);
        logic_commit(c, dn, key == 0x95 ? (ta & tb) : key == 0xA5 ? (ta ^ tb) : (ta | tb), 0, flags); return 0; }
    case 0x81: {                                                                                                                                              // PSHL Sx,Sy,Dz
        if (!valid_dn(dn)) return 1; int sh = (int)((sel_y(c, yy) >> 16) & 63); if (sh & 32) sh -= 64; uint16_t t = (uint16_t)(a >> 16); uint32_t r; int shout = 0;
        if (sh >= 0) { r = sh >= 16 ? 0 : ((uint32_t)t << sh) & 0xFFFF; shout = (sh && sh <= 16) ? (t >> (16 - sh)) & 1 : 0; } else { int k = -sh; r = k >= 16 ? 0 : t >> k; shout = (t >> (k - 1)) & 1; }
        logic_commit(c, dn, (uint16_t)r, shout, flags); return 0; }
    case 0x91: {                                                                                                                                              // PSHA Sx,Sy,Dz
        if (!valid_dn(dn)) return 1; int sh = (int)((sel_y(c, yy) >> 16) & 127); if (sh & 64) sh -= 128; int64_t r; int shout = 0;
        if (sh >= 0) { r = (int64_t)((uint64_t)a << sh); shout = sh ? (int)((a >> (40 - sh)) & 1) : 0; } else { int k = -sh; r = a >> k; shout = (int)((a >> (k - 1)) & 1); }
        arith_commit(c, dn, r, shout, flags, -1, 1); return 0; }
    case 0x89: if (!valid_dn(dn)) return 1; int_commit(c, dn, (((a >> 16) - 1) * 65536), ((uint32_t)a < 0x10000u), flags); return 0;                                              // PDEC X
    case 0xA9: if (!valid_dn(dn)) return 1; int_commit(c, dn, (((b >> 16) - 1) * 65536), ((uint32_t)b < 0x10000u), flags); return 0;                                              // PDEC Y
    case 0x99: if (!valid_dn(dn)) return 1; int_commit(c, dn, (((a >> 16) + 1) * 65536), (int)((((uint64_t)(uint32_t)a + 0x10000u) >> 32) & 1), flags); return 0;                                              // PINC X
    case 0xB9: if (!valid_dn(dn)) return 1; int_commit(c, dn, (((b >> 16) + 1) * 65536), (int)((((uint64_t)(uint32_t)b + 0x10000u) >> 32) & 1), flags); return 0;                                              // PINC Y
    case 0x8D: if (!valid_dn(dn)) return 1; arith_commit(c, dn, 0, 0, flags, -1, 0); return 0;                                                                    // PCLR
    case 0xC9: if (!valid_dn(dn)) return 1; arith_commit(c, dn, -a, ((uint32_t)a != 0), flags, -1, 0); return 0;                                                                   // PNEG X
    case 0xE9: if (!valid_dn(dn)) return 1; arith_commit(c, dn, -b, ((uint32_t)b != 0), flags, -1, 0); return 0;                                                                   // PNEG Y
    case 0xD9: if (!valid_dn(dn)) return 1; arith_commit(c, dn, a, 0, flags, -1, 0); return 0;                                                                    // PCOPY X
    case 0xF9: if (!valid_dn(dn)) return 1; arith_commit(c, dn, b, 0, flags, -1, 0); return 0;                                                                    // PCOPY Y
    case 0x9D: if (!valid_dn(dn)) return 1; pdmsb_commit(c, dn, a, flags); return 0;                                                                         // PDMSB Sx
    case 0xBD: if (!valid_dn(dn)) return 1; pdmsb_commit(c, dn, b, flags); return 0;                                                                         // PDMSB Sy
    case 0xCD: if (!valid_dn(dn)) return 1; wr40(c, dn, (int32_t)c->mach); return 0;                                                                              // PSTS MACH,Dz
    case 0xDD: if (!valid_dn(dn)) return 1; wr40(c, dn, (int32_t)c->macl); return 0;                                                                              // PSTS MACL,Dz
    case 0xED: if (!valid_dn(dn)) return 1; c->mach = (uint32_t)rd40(c, dn); return 0;                                                                            // PLDS Dz,MACH
    case 0xFD: if (!valid_dn(dn)) return 1; c->macl = (uint32_t)rd40(c, dn); return 0;                                                                            // PLDS Dz,MACL
    default: return 1;                                                                                                                                            // anything else is reserved/unimplemented
    }
}

/* double data transfer + optional PPI. `h` = header word (0xF000-0xF3FF or 0xF800-0xFBFF). `alu` valid if (h & 0x800). */
int dsp_exec_parallel(SH3 *c, uint16_t h, uint16_t alu) {
    unsigned insn = h & 0xFFF;
    unsigned ix = (insn >> 2) & 0xB, iy = (insn & 3) | ((insn >> 1) & 8);
    // capture store data and addresses with the pre-ALU register state
    uint32_t xaddr = 0, yaddr = 0; uint16_t xst = 0, yst = 0;
    int xr = ((insn >> 9) & 1) + 4, yr = ((insn >> 8) & 1) + 6;
    int xm = (insn >> 7) & 1, ym = (insn >> 6) & 1;
    if (ix) { xaddr = 0xA5000000u | (c->r[xr] & 0xFFFEu); if (ix & 8) xst = (uint16_t)(rd40(c, xm ? 5 : 7) >> 16); }
    if (iy) { yaddr = 0xA5010000u | (c->r[yr] & 0xFFFEu); if (iy & 8) yst = (uint16_t)(rd40(c, ym ? 5 : 7) >> 16); }
    if (h & 0x800) { int rc = ppi(c, alu); if (rc) { dsp_hist_unimpl[alu >> 8]++; return 1; } dsp_hist[alu >> 8]++; }
    if (ix) {
        if (ix & 8) WR16(xaddr, xst);
        else { uint16_t v = (uint16_t)RD16(xaddr); if (xm) c->x1 = (uint32_t)v << 16; else c->x0 = (uint32_t)v << 16; }
        { int m = ix & 3; int dmx = (c->sr >> 10) & 1, dmy = (c->sr >> 11) & 1; uint32_t inc = m == 2 ? 2 : m == 3 ? c->r[8] : 0;
          if (dmx && !dmy && m >= 2) { if ((c->r[xr] & 0xFFFF) == ((c->mod >> 16) & 0xFFFF)) c->r[xr] = (c->r[xr] & 0xFFFF0000u) | (c->mod & 0xFFFF); else c->r[xr] += inc; } else c->r[xr] += inc; }
    }
    if (iy) {
        if (iy & 8) WR16(yaddr, yst);
        else { uint16_t v = (uint16_t)RD16(yaddr); if (ym) c->y1 = (uint32_t)v << 16; else c->y0 = (uint32_t)v << 16; }
        { int m = iy & 3; int dmy = (c->sr >> 11) & 1; uint32_t inc = m == 2 ? 2 : m == 3 ? c->r[9] : 0;
          if (dmy && m >= 2) { if ((c->r[yr] & 0xFFFF) == ((c->mod >> 16) & 0xFFFF)) c->r[yr] = (c->r[yr] & 0xFFFF0000u) | (c->mod & 0xFFFF); else c->r[yr] += inc; } else c->r[yr] += inc; }
    }
    return 0;
}
