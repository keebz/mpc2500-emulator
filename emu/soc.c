// SH7727 on-chip peripherals as used by the MPC2500 boards: TMU timers, SIOF + DMAC channel 0 (audio clock), ADC (pads), I/O ports
// (key matrix, wheel, card detect, I2C to the SODIMM SPD EEPROM) and the interrupt controller priority registers.
#include "board.h"
#include <stdlib.h>
#include <string.h>

uint8_t soc_lo[SOC_SIZE] __attribute__((aligned(8)));     // 0xA4000000-0xA4000FFF
uint8_t soc_hi[0x200] __attribute__((aligned(8)));        // 0xFFFFFE00-0xFFFFFFFF
uint16_t adc10[4][4];
uint64_t soc_irq_count, siof_frames, dmac_done;
unsigned siof_irq_div = 1;

static uint32_t intevt2;                                   // INTEVT2 @ 0xA4000000: code of the interrupt that entered the 0x600 vector
static uint64_t last_cycles, frame_next = STEPS_PER_FRAME, next_pad_refresh;
static int txi_pending;
static uint64_t tmu_acc[3];

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t *tmu32(int off) { return (uint32_t *)&soc_hi[off - 0xFE00]; }    // soc_hi is 8-byte aligned and all TMU registers are naturally aligned
static uint16_t *tmu16(int off) { return (uint16_t *)&soc_hi[off - 0xFE00]; }

// ================================================================== SODIMM SPD EEPROM (I2C address 0x50)
// Port D lines: SCL = PD3 (PDDR 0xA4000126 bit 3), SDA = PD2, open drain (driven low when PDCR 0xA4000106 bits 5:4 == 01 and PDDR bit 2 == 0).
static struct { int pscl, psda_host, slow, mode, bits, mnack; uint8_t sh; int ptr, rw; uint8_t mem[256]; } i2c = { .pscl = 1, .psda_host = 1 };
int spd_present; uint64_t spd_reads;

void spd_install(int mb) {
    // size = 4 * devbanks(byte 17) * modbanks(byte 5) * 2^(rows+cols), the boot block's own formula
    int rc = 0; while ((32ull << rc) < (uint64_t)mb * 1048576ull) rc++;
    int cols = 9, rows = rc - cols;
    memset(i2c.mem, 0xFF, 256);
    uint8_t *m = i2c.mem;
    m[0] = 128; m[1] = 8; m[2] = 4; m[3] = (uint8_t)rows; m[4] = (uint8_t)cols; m[5] = 2; m[6] = 64; m[7] = 0; m[8] = 1; m[9] = 0x75; m[10] = 0x54;
    m[11] = 0; m[12] = 0x82; m[13] = 0x08; m[14] = 8; m[15] = 1; m[16] = 0x8F; m[17] = 4;
    spd_present = 1;
}
static int sda_host_low(void) { return (((soc_lo[0x106] >> 4) & 3) == 1) && !(soc_lo[0x126] & 4); }
static int sda_line(void) { return !(sda_host_low() || i2c.slow); }
void spd_line_update(void) {
    if (!spd_present) return;
    int scl = (soc_lo[0x126] >> 3) & 1;
    int sdah = !sda_host_low();
    if (scl && i2c.pscl) {
        if (i2c.psda_host && !sdah) { i2c.mode = 1; i2c.bits = 0; i2c.sh = 0; i2c.slow = 0; }          // START: expect address byte
        else if (!i2c.psda_host && sdah) { i2c.mode = 0; i2c.slow = 0; }                               // STOP
    }
    if (scl && !i2c.pscl) {                                                                           // SCL rising
        if (i2c.mode == 1 || i2c.mode == 2) { if (i2c.bits < 8) { i2c.sh = (uint8_t)((i2c.sh << 1) | sdah); i2c.bits++; } else i2c.bits = 9; }
        else if (i2c.mode == 3) { if (i2c.bits < 8) i2c.bits++; else { i2c.mnack = sdah; i2c.bits = 9; } }
    }
    if (!scl && i2c.pscl) {                                                                           // SCL falling
        if (i2c.mode == 1 || i2c.mode == 2) {
            if (i2c.bits == 8) i2c.slow = 1;                                                            // ACK during the 9th clock
            else if (i2c.bits == 9) {
                i2c.slow = 0; i2c.bits = 0;
                if (i2c.mode == 1) {
                    if ((i2c.sh >> 1) == 0x50) { i2c.rw = i2c.sh & 1; if (i2c.rw) { i2c.mode = 3; i2c.slow = !((i2c.mem[i2c.ptr] >> 7) & 1); } else i2c.mode = 2; }
                    else i2c.mode = 0;
                } else { i2c.ptr = i2c.sh; i2c.mode = 4; }
            }
        } else if (i2c.mode == 3) {
            if (i2c.bits < 8) i2c.slow = !((i2c.mem[i2c.ptr] >> (7 - i2c.bits)) & 1);
            else if (i2c.bits == 8) i2c.slow = 0;                                                       // release for the master's ACK
            else if (i2c.bits == 9) {
                if (!i2c.mnack) { i2c.ptr = (i2c.ptr + 1) & 255; spd_reads++; i2c.bits = 0; i2c.slow = !((i2c.mem[i2c.ptr] >> 7) & 1); }
                else { i2c.mode = 0; i2c.slow = 0; }
            }
        }
    }
    i2c.pscl = scl; i2c.psda_host = sdah;
}

// ================================================================== TMU
// TMU0 tick = 1/1.662 MHz (the OS time helper at 0x8855F1EC: 1 ms = 1662 ticks, so Pphi = 26.6 MHz). `per` is in units of 1/100 instruction.
static void tmu_tick(uint64_t dcyc) {
    uint8_t tstr = soc_hi[0x92];
    static const int presc[8] = { 4, 16, 64, 256, 1024, 4, 4, 4 };
    for (int n = 0; n < 3; n++) {
        if (!(tstr & (1 << n))) continue;
        int base = 0xFE94 + 12 * n;
        uint16_t tcr = *tmu16(base + 8);
        uint64_t per = (uint64_t)presc[tcr & 7] * 249;
        tmu_acc[n] += dcyc * 100;
        while (tmu_acc[n] >= per) {                  // (a division here showed up as ~7% of total run time)
            tmu_acc[n] -= per;
            uint32_t *cnt = tmu32(base + 4);
            if ((*cnt)-- == 0) { *cnt = *tmu32(base); *tmu16(base + 8) |= 0x100; }
        }
    }
}
static void tmu_irq(void) {
    if (cpu.sr & SR_BL) return;
    uint32_t ipra = *tmu16(0xFEE2); int imask = (cpu.sr >> 4) & 15;
    for (int n = 0; n < 3; n++) {
        uint16_t tcr = *tmu16(0xFE94 + 12 * n + 8);
        int pri = (ipra >> (12 - 4 * n)) & 15;
        if ((tcr & 0x100) && (tcr & 0x20) && pri > imask) { sh3_interrupt(&cpu, 0x400 + 0x20u * n); return; }
    }
}

// ================================================================== SIOF + DMAC channel 0 (audio frame clock)
// One 32-bit frame per frame-sync (44.1 kHz = 1500 instructions). DMAC ch0 moves DMATCR frames from the SIOF receive FIFO to memory,
// sets CHCR.TE and raises DEI0 (INTEVT2 0x800) when IE is set. SIFTXI (INTEVT2 0xB20) is raised every `siof_irq_div` frames while
// SIIER.TDREQE is set.
static int dei0_pending_unused;
static void siof_frame(void) {
    siof_frames++;
    if ((siof_frames % siof_irq_div) == 0) txi_pending = 1;
    if ((siof_frames % siof_irq_div) == 0) {
        uint32_t chcr = rd32(&soc_lo[0x2C]), tcr = rd32(&soc_lo[0x28]) & 0xFFFFFF, dar = rd32(&soc_lo[0x24]);
        if ((chcr & 1) && !(chcr & 2) && tcr) {                              // DE = 1, TE = 0, count > 0
            for (uint32_t i = 0; i < tcr; i++) { bus_write(0, dar, 0, 4); dar += 4; }      // no audio input connected: frames of silence
            wr32(&soc_lo[0x24], dar); wr32(&soc_lo[0x28], 0); wr32(&soc_lo[0x2C], chcr | 2); dmac_done++;
        }
    }
    (void)dei0_pending_unused;
}
static int dsp_loop_active(void) { return cpu.loop_pending || ((cpu.sr >> 16) & 0xFFF); }
static int siof_txi_ready(void) {
    if (!txi_pending) return 0;
    if (!(rd16(&soc_lo[0xD6]) & 0x1000)) { txi_pending = 0; return 0; }
    if (cpu.sr & SR_BL) return 0;
    if (dsp_loop_active()) return 0;
    int pri = rd16(&soc_lo[0x220]) & 15, imask = (cpu.sr >> 4) & 15;
    if (pri > imask) { txi_pending = 0; return 1; }
    return 0;
}
static int siof_dei0_ready(void) {
    if (((rd32(&soc_lo[0x2C]) & 6) != 6) || (cpu.sr & SR_BL)) return 0;      // DEI0 is level triggered: CHCR.TE = 1 and CHCR.IE = 1
    int pri = (rd16(&soc_lo[0x1A]) >> 12) & 15, imask = (cpu.sr >> 4) & 15;
    if (dsp_loop_active()) return 0;                                         // no interrupts inside a DSP repeat
    return pri > imask;
}

void soc_service(void) {
    uint64_t dcyc = cpu.cycles - last_cycles; last_cycles = cpu.cycles;
    tmu_tick(dcyc); tmu_irq();
    if (cpu.cycles >= next_pad_refresh) { next_pad_refresh = cpu.cycles + 50000; pads_apply(); }
    panel_wheel_service();
    while (cpu.cycles >= frame_next) { frame_next += STEPS_PER_FRAME; siof_frame(); }
    if (siof_dei0_ready()) { intevt2 = 0x800; soc_irq_count++; sh3_interrupt(&cpu, 0x600); }
    else if (siof_txi_ready()) { intevt2 = 0xB20; soc_irq_count++; sh3_interrupt(&cpu, 0x600); }
}

void soc_reset(void) {
    memset(soc_lo, 0, sizeof soc_lo); memset(soc_hi, 0, sizeof soc_hi); memset(adc10, 0, sizeof adc10);
    memset(tmu_acc, 0, sizeof tmu_acc);
    intevt2 = 0; last_cycles = 0; frame_next = STEPS_PER_FRAME; next_pad_refresh = 0; txi_pending = 0;
    siof_frames = dmac_done = soc_irq_count = 0;
    i2c.pscl = 1; i2c.psda_host = 1; i2c.slow = i2c.mode = i2c.bits = i2c.ptr = 0;
}

// ================================================================== register access
uint32_t soc_read(uint32_t a, int size) {
    uint32_t o = a - SOC_BASE, v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)soc_lo[o + i] << (8 * i);
    if (a >= 0x04000080 && a < 0x04000090) {      // ADC data registers ADDRA..D (high byte at +0, low bits at +2 of each 4-byte slot), selected by the mux on port J
        uint8_t pj = soc_lo[0x130]; int m = -1;
        for (int k = 0; k < 4; k++) if (!(pj & (0x40 >> k))) m = k;
        uint8_t b[16] = {0};
        for (int ch = 0; ch < 4; ch++) { uint16_t x = m >= 0 ? adc10[m][ch] : 0; b[ch * 4] = (uint8_t)(x >> 2); b[ch * 4 + 2] = (uint8_t)((x & 3) << 6); }
        v = 0; for (int i = 0; i < size; i++) v |= (uint32_t)b[(a - 0x04000080) + i] << (8 * i);
        return v;
    }
    if (a == 0x04000000) v = intevt2;
    if (a == 0x04000126) v = (v & ~4u) | (spd_present ? (sda_line() ? 4u : 0u) : 4u);   // PD2 = I2C SDA line (idle high without a module)
    if (a == 0x0400012C) v = panel_port_pg();
    if (a == 0x04000138) v = panel_port_pm();
    if (a == 0x0400012A) v = panel_port_pf();
    return v;
}
void soc_write(uint32_t a, uint32_t val, int size) {
    uint32_t o = a - SOC_BASE;
    if (a == 0x040000E0 && size == 4) audio_push(val);                               // SIOF transmit FIFO
    for (int i = 0; i < size; i++) soc_lo[o + i] = (uint8_t)(val >> (8 * i));
    if (spd_present && ((o + size > 0x126 && o <= 0x126) || (o + size > 0x106 && o <= 0x107))) spd_line_update();
}
uint32_t p4_read(uint32_t addr, int size) {
    if (addr == 0xFFFFFFD8u) return cpu.intevt;
    if (addr == 0xFFFFFFD4u) return cpu.expevt;
    uint32_t o = addr - P4_REGS, v = 0;
    for (int i = 0; i < size && o + i < sizeof soc_hi; i++) v |= (uint32_t)soc_hi[o + i] << (8 * i);
    return v;
}
void p4_write(uint32_t addr, uint32_t val, int size) {
    uint32_t o = addr - P4_REGS;
    for (int i = 0; i < size && o + i < sizeof soc_hi; i++) soc_hi[o + i] = (uint8_t)(val >> (8 * i));
}
