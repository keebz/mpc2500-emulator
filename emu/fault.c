// Fault reporting: what real hardware would do badly (crash, wander off, hang) is reported instead of being silently tolerated.
//   fatal (stop emulation + report): execution at an invalid or odd address, stack pointer outside memory, illegal instruction,
//                                    write/erase of the flash boot block
//   non-fatal (counted, listed in the summary): reads/writes of unmapped addresses, misaligned word/long accesses, flash writes
//                                    in the internal-data area, and loops that look endless (warnings).   fault_strict makes the non-fatal bus faults fatal as well.
#include "board.h"
#include <stdlib.h>
#include <string.h>

int fault_fatal, fault_strict;
uint32_t pc_ring[64]; unsigned pc_ring_i;

static const char *fkind[F_NKINDS] = {
    "FLASH write/erase of the BOOT block (0-0xFFFF)", "flash write/erase at/above 0x100000 (internal data area)",
    "read of unmapped address", "write to unmapped address", "misaligned read (SH3 address error)", "misaligned write (SH3 address error)",
    "execution at invalid address", "stack pointer outside RAM", "illegal instruction", "HANG with interrupts masked", "stuck in a loop (no progress)" };
typedef struct { int kind; uint32_t addr, pc, val; int size; uint64_t n, step; } FaultRec;
static FaultRec flt[128]; static int nflt; static uint64_t flt_total[F_NKINDS];

void fault_reset(void) { nflt = 0; memset(flt_total, 0, sizeof flt_total); fault_fatal = 0; pc_ring_i = 0; memset(pc_ring, 0, sizeof pc_ring); }

// F_HANG_MASKED / F_STUCK are warnings: a firmware legitimately spins with interrupts masked while it copies data (e.g. loading a sample bank)
static int is_fatal(int k) { return k == F_BADPC || k == F_BADSP || k == F_ILLEGAL || k == F_FLASH_BOOT || (fault_strict && k >= F_UNMAPPED_R && k <= F_MISALIGNED_W); }

void fault_note(int kind, uint32_t addr, int size, uint32_t val, uint32_t pc) {
    flt_total[kind]++;
    FaultRec *f = 0;
    for (int i = 0; i < nflt; i++) if (flt[i].kind == kind && flt[i].pc == pc && flt[i].addr == addr) { f = &flt[i]; break; }
    if (f) f->n++;
    else if (nflt < 128) {
        f = &flt[nflt++]; f->kind = kind; f->addr = addr; f->pc = pc; f->val = val; f->size = size; f->n = 1; f->step = cpu.cycles;
        if (nflt <= 25) { printf("FAULT: %s: addr=%08x size=%d val=%08x pc=%08x (step %llu)\n", fkind[kind], addr, size, val, pc, (unsigned long long)cpu.cycles); fflush(stdout); }
    }
    if (is_fatal(kind)) fault_fatal = kind + 1;
}

void fault_bus(int rw, uint32_t addr, int size, uint32_t val) {
    uint32_t pc = cpu.pc - 2;
    static const int kinds[4] = { F_UNMAPPED_R, F_UNMAPPED_W, F_MISALIGNED_R, F_MISALIGNED_W };
    fault_note(kinds[rw & 3], addr, size, (rw & 1) ? val : 0, pc);
}

void flash_guard(uint32_t a, int erase) {
    if (a < 0x10000) fault_note(F_FLASH_BOOT, a, erase, 0, cpu.pc - 2);
    else if (a >= 0x100000) fault_note(F_FLASH_DATA, a & ~0xFFFFu, erase, 0, cpu.pc - 2);
}

static int addr_ok(uint32_t a) {
    a = sh3_phys(a);
    return a < FLASH_SIZE || (a >= RAM_BASE && a < RAM_BASE + RAM_WINDOW) || (a >> 24) == 0x05;
}

void fault_report(int kind, uint32_t pc) {
    printf("==== FAULT REPORT: %s ====\n", fkind[kind]);
    printf("step %llu, pc=%08x op=%04x  SR=%08x PR=%08x GBR=%08x VBR=%08x SPC=%08x SSR=%08x\n", (unsigned long long)cpu.cycles, pc, cpu.last_op & 0xFFFF, cpu.sr, cpu.pr, cpu.gbr, cpu.vbr, cpu.spc, cpu.ssr);
    for (int i = 0; i < 16; i += 4) printf("R%-2d=%08x R%-2d=%08x R%-2d=%08x R%-2d=%08x\n", i, cpu.r[i], i + 1, cpu.r[i + 1], i + 2, cpu.r[i + 2], i + 3, cpu.r[i + 3]);
    printf("last pcs (oldest first):"); for (int i = 0; i < 24; i++) printf(" %08x", pc_ring[(pc_ring_i + 64 - 24 + i) & 63]); printf("\n");
    if (addr_ok(cpu.r[15])) { printf("stack @R15:"); for (int i = 0; i < 12; i++) printf(" %08x", bus_peek32(cpu.r[15] + 4 * i)); printf("\n"); }
    else printf("stack pointer R15=%08x is not in RAM\n", cpu.r[15]);
    fflush(stdout);
}

int report_illegal(uint32_t pc) {
    printf("%s: %s (op %08x at pc=%08x after %llu steps)\n", cpu.illegal == 2 ? "UNIMPLEMENTED-DSP" : "ILLEGAL", sh3_illegal_name(cpu.illegal_reason), cpu.last_op, cpu.fault_pc ? cpu.fault_pc : pc, (unsigned long long)cpu.cycles);
    printf("PR=%08x SPC=%08x SSR=%08x SR=%08x\n", cpu.pr, cpu.spc, cpu.ssr, cpu.sr);
    fault_note(F_ILLEGAL, cpu.last_op, 0, 0, pc); fault_report(F_ILLEGAL, pc);
    return 1;
}

void fault_summary(void) {
    int any = 0; for (int k = 0; k < F_NKINDS; k++) if (flt_total[k]) any = 1;
    if (!any) { printf("fault summary: none\n"); return; }
    printf("fault summary:\n");
    for (int k = 0; k < F_NKINDS; k++) if (flt_total[k]) printf("  %-40s %llu events\n", fkind[k], (unsigned long long)flt_total[k]);
    printf("  distinct sites (kind addr pc count):\n");
    for (int i = 0; i < nflt && i < 40; i++) printf("   %-36s addr=%08x pc=%08x x%llu\n", fkind[flt[i].kind], flt[i].addr, flt[i].pc, (unsigned long long)flt[i].n);
}

// called once per batch: validity of pc/sp, and a stuck-loop watchdog sampled every 1M instructions
void fault_check(void) {
    if ((cpu.pc & 1) || !addr_ok(cpu.pc)) { fault_note(F_BADPC, cpu.pc, 0, 0, cpu.pc); return; }
    if (!addr_ok(cpu.r[15]) || (cpu.r[15] & 3)) { if (!cpu.sleeping) fault_note(F_BADSP, cpu.r[15], 0, 0, cpu.pc); return; }
    static uint64_t next; static uint32_t lastpc; static int same, limit;
    if (cpu.cycles < next) return;
    next = cpu.cycles + 1000000;
    if (!limit) limit = getenv("HANG_SAMPLES") ? atoi(getenv("HANG_SAMPLES")) : 150;
    uint32_t d = cpu.pc > lastpc ? cpu.pc - lastpc : lastpc - cpu.pc;
    if (d < 64 && !cpu.sleeping) same++; else same = 0;
    lastpc = cpu.pc;
    if (same == 250 && ((cpu.sr >> 4) & 15) == 15) fault_note(F_HANG_MASKED, cpu.pc, 0, 0, cpu.pc);
    if (same == limit) fault_note(F_STUCK, cpu.pc, 0, 0, cpu.pc);
}
