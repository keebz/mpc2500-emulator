// Differential-test harness for the SH3/SH3-DSP core.
//
// Reads N test cases from a binary file, executes exactly one sh3_step() for each (one instruction, plus its delay slot
// if it is a delayed branch) and writes the resulting state. tests/cpu_ref.py contains an independent reference model
// written from the Hitachi/Renesas SH-3/SH3-DSP software manual; tests/cpu_diff.py compares the two over the whole
// opcode space.
//
// Memory model (identical in the Python reference): code words are served from the test case at [pc, pc+8); every other
// byte reads as hash8(seed, addr); writes are only recorded. This makes loads/stores of any width and address
// deterministic without needing a memory image.
//
//   in  case (uint32 x IN_WORDS):  r[16] bank[8] pc pr gbr vbr mach macl sr ssr spc tra | op0 op1 op2 op3 seed |
//                                  a0 a1 x0 x1 y0 y1 m0 m1 dsr mod rs re a0g a1g
//   out case (uint32 x OUT_WORDS): the same state words | illegal sleeping nwrites expevt | 8 x (addr size value)
#include "../emu/sh3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_WORDS = 33 + 1 + 4 + 1 + 14, OUT_WORDS = 33 + 1 + 14 + 4 + 24 };

static uint32_t g_seed, g_code[4], g_pc;
static struct { uint32_t addr, size, val; } g_wr[8];
static int g_nwr;

static uint8_t hash8(uint32_t seed, uint32_t addr) {
    uint32_t h = addr * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (uint8_t)h;
}
static uint32_t bus_read(void *ctx, uint32_t addr, int size) {
    (void)ctx;
    if (size == 2 && addr >= g_pc && addr < g_pc + 8 && !(addr & 1)) return g_code[(addr - g_pc) >> 1];
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)hash8(g_seed, addr + i) << (8 * i);
    return v;
}
static void bus_write(void *ctx, uint32_t addr, uint32_t val, int size) {
    (void)ctx;
    if (g_nwr < 8) { g_wr[g_nwr].addr = addr; g_wr[g_nwr].size = size; g_wr[g_nwr].val = size == 4 ? val : val & ((1u << (8 * size)) - 1); g_nwr++; }
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s cases.bin results.bin\n", argv[0]); return 2; }
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    if (!fi || !fo) { perror("open"); return 2; }
    uint32_t n; if (fread(&n, 4, 1, fi) != 1) return 2;
    uint32_t in[IN_WORDS], out[OUT_WORDS];
    for (uint32_t k = 0; k < n; k++) {
        if (fread(in, 4, IN_WORDS, fi) != IN_WORDS) return 2;
        SH3 c; sh3_reset(&c); c.bus = (Bus){ bus_read, bus_write, 0 };
        memcpy(c.r, in, 64); memcpy(c.bank, in + 16, 32);
        c.pc = in[24]; c.pr = in[25]; c.gbr = in[26]; c.vbr = in[27]; c.mach = in[28]; c.macl = in[29]; c.sr = in[30]; c.ssr = in[31]; c.spc = in[32];
        c.tra = in[33]; c.expevt = 0;
        for (int i = 0; i < 4; i++) g_code[i] = in[34 + i];
        g_seed = in[38]; g_pc = c.pc; g_nwr = 0;
        c.a0 = in[39]; c.a1 = in[40]; c.x0 = in[41]; c.x1 = in[42]; c.y0 = in[43]; c.y1 = in[44]; c.m0 = in[45]; c.m1 = in[46];
        c.dsr = in[47]; c.mod = in[48]; c.rs = in[49]; c.re = in[50]; c.a0g = (uint8_t)in[51]; c.a1g = (uint8_t)in[52];
        sh3_step(&c);
        memset(out, 0, sizeof out);
        memcpy(out, c.r, 64); memcpy(out + 16, c.bank, 32);
        out[24] = c.pc; out[25] = c.pr; out[26] = c.gbr; out[27] = c.vbr; out[28] = c.mach; out[29] = c.macl; out[30] = c.sr; out[31] = c.ssr; out[32] = c.spc;
        out[33] = c.tra;
        out[34] = c.a0; out[35] = c.a1; out[36] = c.x0; out[37] = c.x1; out[38] = c.y0; out[39] = c.y1; out[40] = c.m0; out[41] = c.m1;
        out[42] = c.dsr; out[43] = c.mod; out[44] = c.rs; out[45] = c.re; out[46] = c.a0g; out[47] = c.a1g;
        out[48] = (uint32_t)c.illegal; out[49] = (uint32_t)c.sleeping; out[50] = (uint32_t)g_nwr; out[51] = c.expevt;
        for (int i = 0; i < g_nwr; i++) { out[52 + 3 * i] = g_wr[i].addr; out[53 + 3 * i] = g_wr[i].size; out[54 + 3 * i] = g_wr[i].val; }
        fwrite(out, 4, OUT_WORDS, fo);
    }
    fclose(fi); fclose(fo);
    return 0;
}
