// SH-3 / SH3-DSP integer core. Semantics follow the manual's per-instruction operation pseudocode; decode legality follows
// the manual's opcode tables exactly (tests/cpu_diff.py checks every one of the 65,536 opcodes against an independent model).
#include "sh3.h"
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the fast memory path assumes a little-endian host"
#endif

#define T (c->sr & SR_T)
#define SETT(x) do { if (x) c->sr |= SR_T; else c->sr &= ~SR_T; } while (0)
#define S8(x)  ((int32_t)(int8_t)(x))
#define S16(x) ((int32_t)(int16_t)(x))
#define ILL(reason) do { c->illegal = 1; c->illegal_reason = (reason); c->fault_pc = pc; return; } while (0)
#define PRIV() do { if (!(c->sr & SR_MD)) ILL(ILL_PRIV); } while (0)
#define DSPON() do { if (!(c->sr & SR_DSP)) ILL(ILL_DSP_OFF); } while (0)

const char *sh3_illegal_name(int r) {
    switch (r) {
    case ILL_DECODE: return "undefined opcode";
    case ILL_SLOT: return "illegal instruction in a delay slot";
    case ILL_PRIV: return "privileged instruction in user mode (SR.MD = 0)";
    case ILL_DSP_OFF: return "DSP instruction while SR.DSP = 0";
    case ILL_DSP_UNIMPL: return "DSP operation not implemented by the emulator";
    }
    return "none";
}

void sh3_set_window(SH3 *c, int idx, uint32_t base, uint32_t size, uint8_t *ptr, int writable) {
    if (idx < 0 || idx >= SH3_NWIN) return;
    c->win[idx] = (FastWin){ base, size, ptr, writable };
    c->code_ptr = 0;
}

// ---- memory access: direct pointer for aligned accesses that hit a window, bus callback otherwise ----
// Windows are indexed by what they hold: [0] RAM, [1] flash (reads only), [2] X/Y memory (mirrored through 0x05xxxxxx). P4 (>= 0xE0000000) always goes to the bus.
static inline uint8_t *fast_ptr(SH3 *c, uint32_t a, int sz, int write) {
    if (a >= 0xE0000000u || (a & (uint32_t)(sz - 1))) return 0;
    uint32_t p = a & 0x1FFFFFFFu;
    const FastWin *w = &c->win[0]; uint32_t o = p - w->base;
    if (o < w->size && w->ptr) return w->ptr + o;
    if ((p >> 24) == 5) { p = 0x05000000u | (p & 0x1FFFFu); w = &c->win[2]; o = p - w->base; return (o < w->size && w->ptr) ? w->ptr + o : 0; }
    if (write) return 0;
    w = &c->win[1]; o = p - w->base;
    return (o < w->size && w->ptr) ? w->ptr + o : 0;
}
static inline uint32_t rdm(SH3 *c, uint32_t a, int sz) {
    const uint8_t *q = fast_ptr(c, a, sz, 0);
    if (q) {
        if (sz == 4) { uint32_t v; memcpy(&v, q, 4); return v; }
        if (sz == 2) { uint16_t h; memcpy(&h, q, 2); return h; }
        return *q;
    }
    return c->bus.read(c->bus.ctx, a, sz);
}
static inline void wrm(SH3 *c, uint32_t a, uint32_t v, int sz) {
    uint8_t *q = fast_ptr(c, a, sz, 1);
    if (q) {
        if (sz == 4) memcpy(q, &v, 4); else if (sz == 2) { uint16_t h = (uint16_t)v; memcpy(q, &h, 2); } else *q = (uint8_t)v;
        return;
    }
    c->bus.write(c->bus.ctx, a, v, sz);
}
// instruction fetch through a cached 64 KB virtual block (linear within a block for RAM, flash and the X/Y mirror)
static inline uint16_t fetch16(SH3 *c, uint32_t pc) {
    uint32_t o = pc - c->code_vbase;
    if (o < 0x10000u && c->code_ptr && !(pc & 1)) { uint16_t h; memcpy(&h, c->code_ptr + o, 2); return h; }
    uint8_t *q = fast_ptr(c, pc & ~0xFFFFu, 2, 0);
    if (q && !(pc & 1)) { c->code_ptr = q; c->code_vbase = pc & ~0xFFFFu; uint16_t h; memcpy(&h, q + (pc & 0xFFFF), 2); return h; }
    return (uint16_t)c->bus.read(c->bus.ctx, pc, 2);
}
#define RD8(a)  rdm(c, (a), 1)
#define RD16(a) rdm(c, (a), 2)
#define RD32(a) rdm(c, (a), 4)
#define WR8(a, v)  wrm(c, (a), (v), 1)
#define WR16(a, v) wrm(c, (a), (v), 2)
#define WR32(a, v) wrm(c, (a), (v), 4)

static void set_sr(SH3 *c, uint32_t v) {
    v &= SH3_SR_MASK;
    if ((v ^ c->sr) & SR_RB) { uint32_t t[8]; memcpy(t, c->r, 32); memcpy(c->r, c->bank, 32); memcpy(c->bank, t, 32); }
    c->sr = v;
}

void sh3_reset(SH3 *c) {
    Bus bus = c->bus; FastWin win[SH3_NWIN]; memcpy(win, c->win, sizeof win);
    memset(c, 0, sizeof *c);
    c->bus = bus; memcpy(c->win, win, sizeof win); c->code_ptr = 0;
    c->sr = SR_MD | SR_RB | SR_BL | 0xF0;
    c->pc = 0xA0000000; c->vbr = 0; c->expevt = 0;
}

void sh3_exception(SH3 *c, uint32_t expevt, uint32_t off) {
    c->spc = c->pc; c->ssr = c->sr; c->expevt = expevt;
    set_sr(c, c->sr | SR_MD | SR_BL | SR_RB);
    c->pc = c->vbr + off;
}

void sh3_interrupt(SH3 *c, uint32_t evt) {
    c->sleeping = 0;
    c->spc = c->pc; c->ssr = c->sr; c->intevt = evt;
    set_sr(c, c->sr | SR_MD | SR_BL | SR_RB);
    c->pc = c->vbr + 0x600;
}

static void exec_slot(SH3 *c, uint16_t op, uint32_t pc);
#define EXEC_INLINE static inline __attribute__((always_inline))
int dsp_exec_parallel(SH3 *c, uint16_t h, uint16_t alu);
int dsp_trace_n; uint32_t dsp_trace_lo = 0xA5008D00, dsp_trace_hi = 0xA5008F00;

/* ---- SH3-DSP register access helpers (Ds/Dn codes: 5=A1 7=A0 8=X0 9=X1 A=Y0 B=Y1 C=M0 E=M1, D=A1G F=A0G) ---- */
static uint32_t *dsp_reg32(SH3 *c, int code) {
    switch (code) { case 5: return &c->a1; case 7: return &c->a0; case 8: return &c->x0; case 9: return &c->x1;
                    case 10: return &c->y0; case 11: return &c->y1; case 12: return &c->m0; case 14: return &c->m1; }
    return 0;
}
static int dsp_valid(int code) { return code == 5 || code == 7 || (code >= 8 && code <= 12) || code == 13 || code == 14 || code == 15; }
static uint32_t dsp_get32(SH3 *c, int code) {
    if (code == 13) return (uint32_t)(int32_t)(int8_t)c->a1g;
    if (code == 15) return (uint32_t)(int32_t)(int8_t)c->a0g;
    return *dsp_reg32(c, code);
}
static void dsp_set32(SH3 *c, int code, uint32_t v) {
    if (code == 13) { c->a1g = (uint8_t)v; return; }
    if (code == 15) { c->a0g = (uint8_t)v; return; }
    *dsp_reg32(c, code) = v;
    if (code == 5) c->a1g = (v & 0x80000000u) ? 0xFF : 0;
    if (code == 7) c->a0g = (v & 0x80000000u) ? 0xFF : 0;
}
static void dsp_set16(SH3 *c, int code, uint16_t v) {   // 16-bit data goes to bits 31:16, low half cleared
    if (code == 13) { c->a1g = (uint8_t)v; return; }
    if (code == 15) { c->a0g = (uint8_t)v; return; }
    *dsp_reg32(c, code) = (uint32_t)v << 16;
    if (code == 5) c->a1g = (v & 0x8000) ? 0xFF : 0;
    if (code == 7) c->a0g = (v & 0x8000) ? 0xFF : 0;
}
// LDS/STS system-register groups (opcode bits 7:4): 6 = DSR, 7 = A0, 8 = X0, 9 = X1, 10 = Y0, 11 = Y1
static uint32_t dsp_sys_get(SH3 *c, int g) {
    switch (g) { case 6: return c->dsr & 0xFF; case 7: return c->a0; case 8: return c->x0; case 9: return c->x1; case 10: return c->y0; default: return c->y1; }
}
static void dsp_sys_set(SH3 *c, int g, uint32_t v) {
    switch (g) {
    case 6: c->dsr = v & 0xFF; break;                                   // SH7727 manual: DSR is GT Z N V CS[2:0] DC
    case 7: c->a0 = v; c->a0g = (v & 0x80000000u) ? 0xFF : 0; break;
    case 8: c->x0 = v; break; case 9: c->x1 = v; break; case 10: c->y0 = v; break; default: c->y1 = v; break;
    }
}
static uint32_t *dsp_ctl(SH3 *c, int g) { return g == 5 ? &c->mod : g == 6 ? &c->rs : &c->re; }

// instructions that cannot sit in a delay slot (manual 6.x: Delay_Slot) plus LDC Rm,SR / LDC.L @Rm+,SR
static int slot_illegal(uint16_t op) {
    uint32_t hi = op >> 12;
    if (hi == 0xA || hi == 0xB) return 1;
    switch (op & 0xFF00) { case 0x8900: case 0x8B00: case 0x8D00: case 0x8F00: case 0xC300: return 1; }
    if (op == 0x000B || op == 0x002B) return 1;
    switch (op & 0xF0FF) { case 0x0003: case 0x0023: case 0x400B: case 0x402B: case 0x400E: case 0x4007: return 1; }
    return 0;
}

static void delay(SH3 *c, uint32_t target, uint32_t pc) {
    uint32_t sp = pc + 2; uint16_t sop = fetch16(c, sp);
    if (slot_illegal(sop)) { c->illegal = 1; c->illegal_reason = ILL_SLOT; c->fault_pc = sp; c->last_op = sop; return; }
    c->pc = sp + 2; c->cycles++; c->last_op = sop;
    exec_slot(c, sop, sp);
    if (!c->illegal) c->pc = target;
}

static void div1(SH3 *c, int m, int n) {
    uint32_t old_q = (c->sr & SR_Q) != 0, q, tmp0, tmp1, rn = c->r[n], rm;
    q = rn >> 31; rn = (rn << 1) | (uint32_t)T;
    c->r[n] = rn;                       // the manual's pseudocode updates R[n] first, so with m == n the divisor is the shifted value
    rm = c->r[m];
    tmp0 = rn;
    if (!old_q) {
        if (!(c->sr & SR_M)) { rn -= rm; tmp1 = rn > tmp0; q = q ? (tmp1 == 0) : tmp1; }
        else { rn += rm; tmp1 = rn < tmp0; q = q ? tmp1 : (tmp1 == 0); }
    } else {
        if (!(c->sr & SR_M)) { rn += rm; tmp1 = rn < tmp0; q = q ? (tmp1 == 0) : tmp1; }
        else { rn -= rm; tmp1 = rn > tmp0; q = q ? tmp1 : (tmp1 == 0); }
    }
    c->r[n] = rn;
    c->sr = (c->sr & ~SR_Q) | (q ? SR_Q : 0);
    SETT(((c->sr & SR_Q) != 0) == ((c->sr & SR_M) != 0));
}

static void mac_l(SH3 *c, int m, int n) {
    int32_t x = (int32_t)RD32(c->r[n]); c->r[n] += 4;
    int32_t y = (int32_t)RD32(c->r[m]); c->r[m] += 4;
    int64_t prod = (int64_t)x * y;
    if (c->sr & SR_S) {      // 48-bit saturating accumulate; only MACH[15:0] takes part (manual pseudocode)
        int64_t acc = (int64_t)((((uint64_t)(c->mach & 0xFFFF)) << 32) | c->macl);
        acc = (int64_t)((uint64_t)acc << 16) >> 16;
        acc += prod;
        if (acc > 0x7FFFFFFFFFFFLL) acc = 0x7FFFFFFFFFFFLL;
        if (acc < -0x800000000000LL) acc = -0x800000000000LL;
        c->mach = (uint32_t)((uint64_t)acc >> 32); c->macl = (uint32_t)acc;
    } else {
        uint64_t acc = (((uint64_t)c->mach << 32) | c->macl) + (uint64_t)prod;
        c->mach = (uint32_t)(acc >> 32); c->macl = (uint32_t)acc;
    }
}
static void mac_w(SH3 *c, int m, int n) {
    int32_t x = S16(RD16(c->r[n])); c->r[n] += 2;
    int32_t y = S16(RD16(c->r[m])); c->r[m] += 2;
    int64_t prod = (int64_t)x * y;
    if (c->sr & SR_S) {
        int64_t r = (int64_t)(int32_t)c->macl + prod;
        if (r > 0x7FFFFFFFLL) { c->mach |= 1; r = 0x7FFFFFFFLL; } else if (r < -0x80000000LL) { c->mach |= 1; r = -0x80000000LL; }
        c->macl = (uint32_t)r;
    } else {
        uint64_t acc = (((uint64_t)c->mach << 32) | c->macl) + (uint64_t)prod;
        c->mach = (uint32_t)(acc >> 32); c->macl = (uint32_t)acc;
    }
}

EXEC_INLINE void exec(SH3 *c, uint16_t op, uint32_t pc) {
    const int n = (op >> 8) & 15, m = (op >> 4) & 15, d = op & 15;
    const uint32_t i = op & 255;
    uint32_t *R = c->r, a, v;
    switch (op >> 12) {
    case 0x0:
        switch (op & 15) {
        case 2:   // STC <ctl>,Rn
            if (m & 8) { PRIV(); R[n] = c->bank[m & 7]; return; }
            switch (m) {
            case 0: PRIV(); R[n] = c->sr; return;
            case 1: R[n] = c->gbr; return;
            case 2: PRIV(); R[n] = c->vbr; return;
            case 3: PRIV(); R[n] = c->ssr; return;
            case 4: PRIV(); R[n] = c->spc; return;
            case 5: case 6: case 7: DSPON(); PRIV(); R[n] = *dsp_ctl(c, m); return;
            }
            ILL(ILL_DECODE);
        case 3:
            if (m == 0) { c->pr = pc + 4; delay(c, pc + 4 + R[n], pc); return; }          // BSRF
            if (m == 2) { delay(c, pc + 4 + R[n], pc); return; }                          // BRAF
            if (m == 8) return;                                                            // PREF
            ILL(ILL_DECODE);
        case 4: WR8(R[0] + R[n], R[m]); return;
        case 5: WR16(R[0] + R[n], R[m]); return;
        case 6: WR32(R[0] + R[n], R[m]); return;
        case 7: c->macl = R[n] * R[m]; return;                                             // MUL.L
        case 8:
            if (n != 0) ILL(ILL_DECODE);
            switch (m) {
            case 0: c->sr &= ~SR_T; return;                                                // CLRT
            case 1: c->sr |= SR_T; return;                                                 // SETT
            case 2: c->mach = c->macl = 0; return;                                         // CLRMAC
            case 3: PRIV(); return;                                                        // LDTLB (no TLB modelled)
            case 4: c->sr &= ~SR_S; return;                                                // CLRS
            case 5: c->sr |= SR_S; return;                                                 // SETS
            }
            ILL(ILL_DECODE);
        case 9:
            if (n == 0 && m == 0) return;                                                  // NOP
            if (n == 0 && m == 1) { c->sr &= ~(SR_M | SR_Q | SR_T); return; }              // DIV0U
            if (m == 2) { R[n] = (uint32_t)T; return; }                                    // MOVT
            ILL(ILL_DECODE);
        case 10:  // STS <sys>,Rn
            if (m == 0) { R[n] = c->mach; return; }
            if (m == 1) { R[n] = c->macl; return; }
            if (m == 2) { R[n] = c->pr; return; }
            if (m >= 6 && m <= 11) { DSPON(); R[n] = dsp_sys_get(c, m); return; }
            ILL(ILL_DECODE);
        case 11:
            if (op == 0x000B) { delay(c, c->pr, pc); return; }                             // RTS
            if (op == 0x001B) { PRIV(); c->sleeping = 1; return; }                         // SLEEP
            if (op == 0x002B) {                                                             // RTE: the delay slot runs with the restored SR
                PRIV(); uint32_t tgt = c->spc; set_sr(c, c->ssr); delay(c, tgt, pc); return; }
            ILL(ILL_DECODE);
        case 12: R[n] = (uint32_t)S8(RD8(R[0] + R[m])); return;
        case 13: R[n] = (uint32_t)S16(RD16(R[0] + R[m])); return;
        case 14: R[n] = RD32(R[0] + R[m]); return;
        case 15: mac_l(c, m, n); return;
        }
        ILL(ILL_DECODE);
    case 0x1: WR32(R[n] + (uint32_t)d * 4, R[m]); return;
    case 0x2:
        switch (d) {
        case 0: WR8(R[n], R[m]); return;
        case 1: WR16(R[n], R[m]); return;
        case 2: WR32(R[n], R[m]); return;
        case 4: { uint32_t val = R[m]; R[n] -= 1; WR8(R[n], val); return; }     // the stored value is Rm before the decrement
        case 5: { uint32_t val = R[m]; R[n] -= 2; WR16(R[n], val); return; }
        case 6: { uint32_t val = R[m]; R[n] -= 4; WR32(R[n], val); return; }
        case 7: { uint32_t q = R[n] >> 31, mm = R[m] >> 31; c->sr = (c->sr & ~(SR_Q | SR_M)) | (q ? SR_Q : 0) | (mm ? SR_M : 0); SETT(q != mm); return; }
        case 8: SETT((R[n] & R[m]) == 0); return;
        case 9: R[n] &= R[m]; return;
        case 10: R[n] ^= R[m]; return;
        case 11: R[n] |= R[m]; return;
        case 12: { uint32_t x = R[n] ^ R[m]; SETT(!(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u)); return; }
        case 13: R[n] = (R[m] << 16) | (R[n] >> 16); return;
        case 14: c->macl = (R[n] & 0xFFFF) * (R[m] & 0xFFFF); return;
        case 15: c->macl = (uint32_t)(S16(R[n]) * S16(R[m])); return;
        }
        ILL(ILL_DECODE);
    case 0x3:
        switch (d) {
        case 0: SETT(R[n] == R[m]); return;
        case 2: SETT(R[n] >= R[m]); return;
        case 3: SETT((int32_t)R[n] >= (int32_t)R[m]); return;
        case 4: div1(c, m, n); return;
        case 5: { uint64_t p = (uint64_t)R[n] * R[m]; c->mach = (uint32_t)(p >> 32); c->macl = (uint32_t)p; return; }
        case 6: SETT(R[n] > R[m]); return;
        case 7: SETT((int32_t)R[n] > (int32_t)R[m]); return;
        case 8: R[n] -= R[m]; return;
        case 10: { uint32_t tmp0 = R[n], tmp1 = tmp0 - R[m], t = (uint32_t)T; R[n] = tmp1 - t; uint32_t cf = (tmp0 < tmp1); if (tmp1 < R[n]) cf = 1; SETT(cf); return; }
        case 11: { int64_t r = (int64_t)(int32_t)R[n] - (int32_t)R[m]; R[n] = (uint32_t)r; SETT(r > 0x7FFFFFFFLL || r < -0x80000000LL); return; }
        case 12: R[n] += R[m]; return;
        case 13: { int64_t p = (int64_t)(int32_t)R[n] * (int32_t)R[m]; c->mach = (uint32_t)((uint64_t)p >> 32); c->macl = (uint32_t)p; return; }
        case 14: { uint32_t tmp0 = R[n], tmp1 = tmp0 + R[m], t = (uint32_t)T; R[n] = tmp1 + t; uint32_t cf = (tmp0 > tmp1); if (tmp1 > R[n]) cf = 1; SETT(cf); return; }
        case 15: { int64_t r = (int64_t)(int32_t)R[n] + (int32_t)R[m]; R[n] = (uint32_t)r; SETT(r > 0x7FFFFFFFLL || r < -0x80000000LL); return; }
        }
        ILL(ILL_DECODE);
    case 0x4:
        if (d == 15) { mac_w(c, m, n); return; }
        if (d == 12) {                                                                      // SHAD
            uint32_t sh = R[m];
            if (!(sh & 0x80000000u)) R[n] <<= (sh & 31);
            else { uint32_t cnt = (sh & 31) ? 32 - (sh & 31) : 32; R[n] = cnt < 32 ? (uint32_t)((int32_t)R[n] >> cnt) : ((int32_t)R[n] < 0 ? 0xFFFFFFFFu : 0); }
            return;
        }
        if (d == 13) {                                                                      // SHLD
            uint32_t sh = R[m];
            if (!(sh & 0x80000000u)) R[n] <<= (sh & 31);
            else { uint32_t cnt = (sh & 31) ? 32 - (sh & 31) : 32; R[n] = cnt < 32 ? R[n] >> cnt : 0; }
            return;
        }
        switch (op & 0xFF) {
        case 0x00: SETT(R[n] >> 31); R[n] <<= 1; return;                                    // SHLL
        case 0x01: SETT(R[n] & 1); R[n] >>= 1; return;                                      // SHLR
        case 0x04: SETT(R[n] >> 31); R[n] = (R[n] << 1) | (R[n] >> 31); return;             // ROTL
        case 0x05: SETT(R[n] & 1); R[n] = (R[n] >> 1) | (R[n] << 31); return;               // ROTR
        case 0x08: R[n] <<= 2; return;
        case 0x09: R[n] >>= 2; return;
        case 0x18: R[n] <<= 8; return;
        case 0x19: R[n] >>= 8; return;
        case 0x28: R[n] <<= 16; return;
        case 0x29: R[n] >>= 16; return;
        case 0x20: SETT(R[n] >> 31); R[n] <<= 1; return;                                    // SHAL
        case 0x21: SETT(R[n] & 1); R[n] = (uint32_t)((int32_t)R[n] >> 1); return;           // SHAR
        case 0x24: { uint32_t t = (uint32_t)T; SETT(R[n] >> 31); R[n] = (R[n] << 1) | t; return; }          // ROTCL
        case 0x25: { uint32_t t = (uint32_t)T; SETT(R[n] & 1); R[n] = (R[n] >> 1) | (t << 31); return; }    // ROTCR
        case 0x10: R[n]--; SETT(R[n] == 0); return;                                         // DT
        case 0x11: SETT((int32_t)R[n] >= 0); return;                                        // CMP/PZ
        case 0x15: SETT((int32_t)R[n] > 0); return;                                         // CMP/PL
        case 0x1B: v = RD8(R[n]); SETT(v == 0); WR8(R[n], v | 0x80); return;                // TAS.B
        case 0x2B: delay(c, R[n], pc); return;                                              // JMP
        case 0x0B: c->pr = pc + 4; delay(c, R[n], pc); return;                              // JSR
        case 0x14: DSPON(); c->sr = (c->sr & ~0x0FFF0000u) | ((R[n] & 0xFFFu) << 16); return;   // SETRC Rm
        }
        {   // control / system register loads and stores, selected by the low nibble; bits 7:4 pick the register
            const int g = m, lsb = op & 15;
            switch (lsb) {
            case 0xE:   // LDC Rm,<ctl>
                if (g & 8) { PRIV(); c->bank[g & 7] = R[n]; return; }
                if (g >= 5) { DSPON(); PRIV(); *dsp_ctl(c, g) = R[n]; return; }
                switch (g) { case 0: PRIV(); set_sr(c, R[n]); return; case 1: c->gbr = R[n]; return; case 2: PRIV(); c->vbr = R[n]; return;
                             case 3: PRIV(); c->ssr = R[n]; return; case 4: PRIV(); c->spc = R[n]; return; }
                ILL(ILL_DECODE);
            case 0x7:   // LDC.L @Rm+,<ctl>
                if (g & 8) { PRIV(); v = RD32(R[n]); c->bank[g & 7] = v; R[n] += 4; return; }
                if (g >= 5) { DSPON(); PRIV(); v = RD32(R[n]); R[n] += 4; *dsp_ctl(c, g) = v; return; }
                if (g > 4) ILL(ILL_DECODE);
                if (g != 1) PRIV();
                v = RD32(R[n]); R[n] += 4;
                switch (g) { case 0: set_sr(c, v); return; case 1: c->gbr = v; return; case 2: c->vbr = v; return; case 3: c->ssr = v; return; default: c->spc = v; return; }
            case 0x3:   // STC.L <ctl>,@-Rn
                if (g & 8) { PRIV(); R[n] -= 4; WR32(R[n], c->bank[g & 7]); return; }
                if (g >= 5) { DSPON(); PRIV(); R[n] -= 4; WR32(R[n], *dsp_ctl(c, g)); return; }
                if (g > 4) ILL(ILL_DECODE);
                if (g != 1) PRIV();
                R[n] -= 4;
                switch (g) { case 0: WR32(R[n], c->sr); return; case 1: WR32(R[n], c->gbr); return; case 2: WR32(R[n], c->vbr); return;
                             case 3: WR32(R[n], c->ssr); return; default: WR32(R[n], c->spc); return; }
            case 0xA:   // LDS Rm,<sys>
                if (g == 0) { c->mach = R[n]; return; }
                if (g == 1) { c->macl = R[n]; return; }
                if (g == 2) { c->pr = R[n]; return; }
                if (g >= 6 && g <= 11) { DSPON(); dsp_sys_set(c, g, R[n]); return; }
                ILL(ILL_DECODE);
            case 0x6:   // LDS.L @Rm+,<sys>
                if (g <= 2) { v = RD32(R[n]); R[n] += 4; if (g == 0) c->mach = v; else if (g == 1) c->macl = v; else c->pr = v; return; }
                if (g >= 6 && g <= 11) { DSPON(); v = RD32(R[n]); R[n] += 4; dsp_sys_set(c, g, v); return; }
                ILL(ILL_DECODE);
            case 0x2:   // STS.L <sys>,@-Rn
                if (g <= 2) { R[n] -= 4; WR32(R[n], g == 0 ? c->mach : g == 1 ? c->macl : c->pr); return; }
                if (g >= 6 && g <= 11) { DSPON(); R[n] -= 4; WR32(R[n], dsp_sys_get(c, g)); return; }
                ILL(ILL_DECODE);
            }
        }
        ILL(ILL_DECODE);
    case 0x5: R[n] = RD32(R[m] + (uint32_t)d * 4); return;
    case 0x6:
        switch (d) {
        case 0: R[n] = (uint32_t)S8(RD8(R[m])); return;
        case 1: R[n] = (uint32_t)S16(RD16(R[m])); return;
        case 2: R[n] = RD32(R[m]); return;
        case 3: R[n] = R[m]; return;
        case 4: v = (uint32_t)S8(RD8(R[m])); if (n != m) R[m] += 1; R[n] = v; return;
        case 5: v = (uint32_t)S16(RD16(R[m])); if (n != m) R[m] += 2; R[n] = v; return;
        case 6: v = RD32(R[m]); if (n != m) R[m] += 4; R[n] = v; return;
        case 7: R[n] = ~R[m]; return;
        case 8: R[n] = (R[m] & 0xFFFF0000u) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); return;
        case 9: R[n] = (R[m] << 16) | (R[m] >> 16); return;
        case 10: { uint32_t t = 0 - R[m], r = t - (uint32_t)T; uint32_t cf = (0 < t); if (t < r) cf = 1; SETT(cf); R[n] = r; return; }   // NEGC
        case 11: R[n] = 0 - R[m]; return;
        case 12: R[n] = R[m] & 0xFF; return;
        case 13: R[n] = R[m] & 0xFFFF; return;
        case 14: R[n] = (uint32_t)S8(R[m]); return;
        case 15: R[n] = (uint32_t)S16(R[m]); return;
        }
        ILL(ILL_DECODE);
    case 0x7: R[n] += (uint32_t)S8(i); return;
    case 0x8:
        switch (n) {
        case 0: WR8(R[m] + (uint32_t)d, R[0]); return;
        case 1: WR16(R[m] + (uint32_t)d * 2, R[0]); return;
        case 2: DSPON(); c->sr = (c->sr & ~0x0FFF0000u) | (i << 16); return;                               // SETRC #imm
        case 4: R[0] = (uint32_t)S8(RD8(R[m] + (uint32_t)d)); return;
        case 5: R[0] = (uint32_t)S16(RD16(R[m] + (uint32_t)d * 2)); return;
        case 8: SETT(R[0] == (uint32_t)S8(i)); return;
        case 9: if (T) c->pc = pc + 4 + (uint32_t)(S8(i) * 2); return;                                      // BT
        case 11: if (!T) c->pc = pc + 4 + (uint32_t)(S8(i) * 2); return;                                    // BF
        case 13: if (T) delay(c, pc + 4 + (uint32_t)(S8(i) * 2), pc); return;                               // BT/S
        case 15: if (!T) delay(c, pc + 4 + (uint32_t)(S8(i) * 2), pc); return;                              // BF/S
        case 12: DSPON(); c->rs = pc + 4 + (uint32_t)(S8(i) * 2); return;                                   // LDRS
        case 14: DSPON(); c->re = pc + 4 + (uint32_t)(S8(i) * 2); return;                                   // LDRE
        }
        ILL(ILL_DECODE);
    case 0x9: R[n] = (uint32_t)S16(RD16(pc + 4 + i * 2)); return;                                           // MOV.W @(disp,PC)
    case 0xA: { int32_t dd = (int32_t)((uint32_t)(op & 0xFFF) << 20) >> 20; delay(c, pc + 4 + (uint32_t)(dd * 2), pc); return; }               // BRA
    case 0xB: { int32_t dd = (int32_t)((uint32_t)(op & 0xFFF) << 20) >> 20; c->pr = pc + 4; delay(c, pc + 4 + (uint32_t)(dd * 2), pc); return; } // BSR
    case 0xC:
        switch (n) {
        case 0: WR8(c->gbr + i, R[0]); return;
        case 1: WR16(c->gbr + i * 2, R[0]); return;
        case 2: WR32(c->gbr + i * 4, R[0]); return;
        case 3: c->tra = i << 2; c->pc = pc + 2; sh3_exception(c, 0x160, 0x100); return;                    // TRAPA
        case 4: R[0] = (uint32_t)S8(RD8(c->gbr + i)); return;
        case 5: R[0] = (uint32_t)S16(RD16(c->gbr + i * 2)); return;
        case 6: R[0] = RD32(c->gbr + i * 4); return;
        case 7: R[0] = ((pc + 4) & ~3u) + i * 4; return;                                                    // MOVA
        case 8: SETT((R[0] & i) == 0); return;
        case 9: R[0] &= i; return;
        case 10: R[0] ^= i; return;
        case 11: R[0] |= i; return;
        case 12: a = c->gbr + R[0]; SETT((RD8(a) & i) == 0); return;
        case 13: a = c->gbr + R[0]; WR8(a, RD8(a) & i); return;
        case 14: a = c->gbr + R[0]; WR8(a, RD8(a) ^ i); return;
        case 15: a = c->gbr + R[0]; WR8(a, RD8(a) | i); return;
        }
        ILL(ILL_DECODE);
    case 0xD: R[n] = RD32(((pc + 4) & ~3u) + i * 4); return;                                                // MOV.L @(disp,PC)
    case 0xE: R[n] = (uint32_t)S8(i); return;
    case 0xF: {
        DSPON();
        if (op == 0xF000) return;                                   // NOPX NOPY
        if ((op & 0xFC00) == 0xF400) {                              // single data transfer (MOVS)
            static const int asel[4] = {4, 5, 2, 3};
            int as = asel[(op >> 8) & 3], ds = (op >> 4) & 15, f = op & 15;
            int isl = f & 2, st = f & 1, md = (f >> 2) & 3;          // md: 0 = -As, 1 = @As, 2 = @As+, 3 = @As+Is(R8)
            int sz = isl ? 4 : 2;
            if (!dsp_valid(ds)) { c->dsp_unimpl++; c->illegal = 2; c->illegal_reason = ILL_DSP_UNIMPL; c->fault_pc = pc; return; }
            if (md == 0) R[as] -= (uint32_t)sz;
            uint32_t addr = R[as];
            if (st) { uint32_t val = dsp_get32(c, ds); if (isl) WR32(addr, val); else WR16(addr, (ds == 13 || ds == 15) ? (val & 0xFFFF) : (val >> 16)); }
            else { if (isl) dsp_set32(c, ds, RD32(addr)); else dsp_set16(c, ds, (uint16_t)RD16(addr)); }
            if (md == 2) R[as] += (uint32_t)sz; else if (md == 3) R[as] += R[8];
            return;
        }
        if ((op & 0xF400) == 0xF000) {                              // double data transfer / parallel processing (header)
            uint16_t alu = 0;
            if (op & 0x800) { alu = (uint16_t)RD16(pc + 2); c->pc = pc + 4; }
            if (dsp_trace_n > 0 && pc >= dsp_trace_lo && pc < dsp_trace_hi) { dsp_trace_n--; printf("DSP pc=%08x hdr=%04x alu=%04x | a0=%02x.%08x a1=%02x.%08x x0=%08x x1=%08x y0=%08x y1=%08x m0=%08x m1=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n", pc, op, alu, c->a0g, c->a0, c->a1g, c->a1, c->x0, c->x1, c->y0, c->y1, c->m0, c->m1, c->r[4], c->r[5], c->r[6], c->r[7]); }
            int rc = dsp_exec_parallel(c, op, alu);
            if (rc) { c->dsp_unimpl++; c->illegal = 2; c->illegal_reason = ILL_DSP_UNIMPL; c->fault_pc = pc; c->last_op = (op & 0x800) ? (uint32_t)((op << 16) | alu) : op; }
            return;
        }
        c->dsp_unimpl++; c->illegal = 2; c->illegal_reason = ILL_DSP_UNIMPL; c->fault_pc = pc; return;
    }
    }
}

static __attribute__((noinline)) void loop_step(SH3 *c) {   // DSP repeat loop (manual 5.12/Table 5.23): loops of >= 4 instructions have RE = (instruction n-3)+4 and loop back after the last instruction
    uint32_t rc = (c->sr >> 16) & 0xFFF;
    if (c->loop_pending > 0) {
        if (--c->loop_pending == 0) { if (rc > 1) { c->sr = (c->sr & ~0x0FFF0000u) | ((rc - 1) << 16); c->pc = c->loop_target; } else c->sr &= ~0x0FFF0000u; }
    } else if (rc && (c->pc == c->re || c->pc + 4 == c->re)) {
        int32_t d = (int32_t)(c->rs - c->re);                   // Table 5.23: RS-RE = 4 -> 1-instruction loop, 2 -> 2 instructions, 0 -> 3, RS<RE -> >=4
        if (d == 4 || d == 2 || d == 0) {
            if (c->pc == c->re) {
                if (d == 4) { c->loop_pending = 1; c->loop_target = c->re; }
                else if (d == 2) { c->loop_pending = 2; c->loop_target = c->re; }
                else { c->loop_pending = 3; c->loop_target = c->rs; }
            }
        } else if (c->pc + 4 == c->re) { c->loop_pending = 4; c->loop_target = c->rs; }
    }
}

static __attribute__((noinline)) void exec_slot(SH3 *c, uint16_t op, uint32_t pc) { exec(c, op, pc); }

// Runs up to n instructions (stopping early on an illegal instruction) recording each pc in ring; the decoder is inlined here so a batch
// costs one call instead of one per instruction.
void sh3_run(SH3 *c, int n, uint32_t *ring, unsigned *ring_i) {
    for (int k = 0; k < n; k++) {
        if (__builtin_expect(c->sleeping, 0)) { c->cycles += (uint64_t)(n - k); return; }
        uint32_t pc = c->pc;
        ring[(*ring_i)++ & 63] = pc;
        uint16_t op = fetch16(c, pc);
        c->pc = pc + 2; c->cycles++; c->last_op = op;
        exec(c, op, pc);
        if (__builtin_expect((c->sr & 0x0FFF0000u) || c->loop_pending, 0)) loop_step(c);
        if (__builtin_expect(c->illegal, 0)) return;
    }
}

void sh3_step(SH3 *c) {
    if (c->sleeping) { c->cycles++; return; }
    uint32_t pc = c->pc;
    uint16_t op = fetch16(c, pc);
    c->pc = pc + 2; c->cycles++; c->last_op = op;
    exec(c, op, pc);
    if (__builtin_expect((c->sr & 0x0FFF0000u) || c->loop_pending, 0)) loop_step(c);
}
