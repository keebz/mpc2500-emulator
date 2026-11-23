// Debugging aids for firmware development: memory watchpoints, breakpoints and a first-call trace.
// Watchpoints work in physical addresses (virtual & 0x1FFFFFFF, e.g. 0x881C0000 -> 0x081C0000). While any watchpoint is active
// the CPU's direct memory windows are bypassed so every access reaches the bus and can be seen (slower, debugging only).
#include "board.h"
#include <string.h>

static struct { uint32_t lo, hi; int on; int budget, any_pc; } ww, wr_;
static uint32_t bps[4]; static int nbp, bp_hits;
static int calltrace;

void dbg_watch_write(uint32_t lo, uint32_t len) { ww.lo = lo; ww.hi = lo + (len ? len : 1); ww.on = 1; ww.budget = 20000; dbg_update_windows(); }
void dbg_watch_read(uint32_t lo, uint32_t len) { wr_.lo = lo; wr_.hi = lo + (len ? len : 1); wr_.on = 1; wr_.budget = 6000; dbg_update_windows(); }
void dbg_watch_clear(void) { ww.on = wr_.on = 0; dbg_update_windows(); }
void dbg_watch_any_pc(int on) { ww.any_pc = wr_.any_pc = on; }
void dbg_watch_reset_budget(void) { ww.budget = 20000; wr_.budget = 6000; }
void dbg_call_trace(int on) { calltrace = on; }
void dbg_set_breakpoint(uint32_t pc, int slot) { if (slot >= 0 && slot < 4) { bps[slot] = pc; if (slot >= nbp) nbp = slot + 1; } }
int dbg_active(void) { return ww.on || wr_.on; }
int dbg_stepping(void) { return calltrace || nbp; }
void dbg_update_windows(void) { board_update_windows(); }

void dbg_on_write(uint32_t phys, uint32_t val, int size) {
    if (!ww.on || phys < ww.lo || phys >= ww.hi || ww.budget <= 0) return;
    if (cpu.pc <= 0x20000 && !ww.any_pc) return;
    ww.budget--;
    printf("WRITE %08x <- %08x  pc=%08x pr=%08x step=%llu\n", phys | 0x80000000u, val, cpu.pc - 2, cpu.pr, (unsigned long long)cpu.cycles); (void)size;
}
void dbg_on_read(uint32_t phys, uint32_t val, int size) {
    if (!wr_.on || phys < wr_.lo || phys >= wr_.hi || wr_.budget <= 0) return;
    if (cpu.pc <= 0x20000 && !wr_.any_pc) return;
    wr_.budget--;
    printf("READ  %08x size %d val=%08x pc=%08x pr=%08x step=%llu sr=%08x\n", phys | 0x80000000u, size, val, cpu.pc - 2, cpu.pr, (unsigned long long)cpu.cycles, cpu.sr);
}

void dbg_on_step(uint32_t pc) {
    for (int i = 0; i < nbp; i++) if (bps[i] == pc && bp_hits < 40) {
        bp_hits++;
        printf("BP pc=%08x r0-7: %08x %08x %08x %08x %08x %08x %08x %08x pr=%08x\n", pc, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.pr);
        fflush(stdout);
    }
    if (!calltrace) return;
    uint32_t w = bus_peek32(pc & ~3u); uint16_t op = (uint16_t)((pc & 2) ? w >> 16 : w & 0xFFFF);
    uint32_t tgt = 0; int isc = 0;
    if ((op & 0xF000) == 0xB000) { int32_t dd = (int32_t)((uint32_t)(op & 0xFFF) << 20) >> 20; tgt = pc + 4 + (uint32_t)(dd * 2); isc = 1; }
    else if ((op & 0xF0FF) == 0x400B) { tgt = cpu.r[(op >> 8) & 15]; isc = 1; }
    if (!isc) return;
    static uint32_t seen[4096]; static int n;
    for (int k = 0; k < n; k++) if (seen[k] == tgt) return;
    if (n < 4096) { seen[n++] = tgt; printf("call %08x -> %08x  (step %llu)\n", pc, tgt, (unsigned long long)cpu.cycles); }
}
