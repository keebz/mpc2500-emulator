// SH-3 / SH3-DSP (SH7727) CPU core, written from scratch from the Renesas SH-3/SH3-DSP software manual
// (REJ09B0317) and the SH7727 hardware manual (REJ09B0254).
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct SH3 SH3;
typedef struct {
    uint32_t (*read)(void *ctx, uint32_t addr, int size);
    void (*write)(void *ctx, uint32_t addr, uint32_t val, int size);
    void *ctx;
} Bus;

// Direct-memory window used instead of the bus callbacks for aligned accesses (RAM, flash reads, X/Y memory).
// base/size are in the physical address space after sh3_phys(); ptr == NULL disables the window.
typedef struct { uint32_t base, size; uint8_t *ptr; int writable; } FastWin;
enum { SH3_NWIN = 3 };

enum { ILL_NONE = 0, ILL_DECODE, ILL_SLOT, ILL_PRIV, ILL_DSP_OFF, ILL_DSP_UNIMPL };

struct SH3 {
    uint32_t r[16];      // active R0-R7 are r[0..7]
    uint32_t bank[8];    // inactive bank R0-R7
    uint32_t pc, pr, gbr, vbr, mach, macl, sr, ssr, spc;
    uint32_t expevt, intevt, tra;
    // SH3-DSP unit (A0/A1 = 32-bit + 8-bit guard)
    uint32_t a0, a1, x0, x1, y0, y1, m0, m1, dsr, mod, rs, re;
    uint8_t a0g, a1g;
    uint32_t loop_target;
    int loop_pending;     // instructions left until the DSP repeat loop-back
    uint64_t dsp_unimpl;  // count of DSP opcodes hit that are not implemented
    uint64_t cycles;      // executed instructions
    int sleeping;
    Bus bus;
    FastWin win[SH3_NWIN];   // [0] = RAM, [1] = flash (read only), [2] = X/Y memory
    uint8_t *code_ptr; uint32_t code_vbase;   // cached 64 KB virtual block for instruction fetch (code_ptr NULL = empty)
    int illegal;          // 0 = ok, 1 = illegal instruction (see illegal_reason), 2 = unimplemented DSP operation
    int illegal_reason;   // ILL_*
    uint32_t last_op, fault_pc;
};

enum { SR_T = 1, SR_S = 2, SR_Q = 0x100, SR_M = 0x200, SR_DSP = 0x1000, SR_BL = 0x10000000, SR_RB = 0x20000000, SR_MD = 0x40000000 };
#define SH3_SR_MASK 0x7FFF1FFFu   // bit 31 and bits 15:13 are reserved and read as 0

// Physical address of a P0-P3 virtual address (MMU off); the 128 KB X/Y memory is mirrored through 0x05000000-0x05FFFFFF.
static inline uint32_t sh3_phys(uint32_t a) {
    if (a >= 0xE0000000u) return a;
    a &= 0x1FFFFFFFu;
    if ((a >> 24) == 0x05) a = 0x05000000u | (a & 0x1FFFFu);
    return a;
}

void sh3_reset(SH3 *c);
void sh3_step(SH3 *c);                     // execute one instruction (incl. its delay slot)
void sh3_run(SH3 *c, int n, uint32_t *ring, unsigned *ring_i);   // up to n instructions, recording each pc in a 64-entry ring; stops on an illegal instruction
void sh3_interrupt(SH3 *c, uint32_t evt);  // take an interrupt: SPC/SSR saved, jump to VBR+0x600
void sh3_exception(SH3 *c, uint32_t expevt, uint32_t off);
void sh3_set_window(SH3 *c, int idx, uint32_t base, uint32_t size, uint8_t *ptr, int writable);
const char *sh3_illegal_name(int reason);
