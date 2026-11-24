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
uint32_t steps_per_frame = 1500;
int soc_irq_blocked;                   // an interrupt is waiting only for BL / a DSP repeat loop to end: run short batches until it can be taken
static uint64_t last_cycles, frame_next, next_pad_refresh;
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
// Clocks: the CPU board crystal is 13.3 MHz; FRQCR = H'A101 with the on-chip x4 PLL gives I:B:P = 12:4:2, i.e. Iphi 159.6 MHz, Bphi 53.2 MHz and
// Pphi 26.6 MHz (the OS time helper at 0x8855F1EC counts 1 ms = 1662 TMU0 ticks of Pphi/16 = 1.6625 MHz, which confirms 26.6 MHz).
// One emulated instruction is 1/CPU_HZ s (see board.h), so one Pphi cycle = CPU_HZ / 26.6 MHz = 2.4868 instructions; `per` is in 1/10000 instruction.
static void tmu_tick(uint64_t dcyc) {
    uint8_t tstr = soc_hi[0x92];
    static const int presc[8] = { 4, 16, 64, 256, 1024, 4, 4, 4 };
    for (int n = 0; n < 3; n++) {
        if (!(tstr & (1 << n))) continue;
        int base = 0xFE94 + 12 * n;
        uint16_t tcr = *tmu16(base + 8);
        uint64_t per = (uint64_t)presc[tcr & 7] * 24868 * steps_per_frame / 1500;
        tmu_acc[n] += dcyc * 10000;
        while (tmu_acc[n] >= per) {                  // (a division here showed up as ~7% of total run time)
            tmu_acc[n] -= per;
            uint32_t *cnt = tmu32(base + 4);
            if ((*cnt)-- == 0) { *cnt = *tmu32(base); *tmu16(base + 8) |= 0x100; }
        }
    }
}
static void tmu_irq(void) {
    uint32_t ipra = *tmu16(0xFEE2); int imask = (cpu.sr >> 4) & 15;
    for (int n = 0; n < 3; n++) {
        uint16_t tcr = *tmu16(0xFE94 + 12 * n + 8);
        int pri = (ipra >> (12 - 4 * n)) & 15;
        if ((tcr & 0x100) && (tcr & 0x20) && pri > imask) {
            if (cpu.sr & SR_BL) { soc_irq_blocked = 1; return; }
            sh3_interrupt(&cpu, 0x400 + 0x20u * n); return;
        }
    }
}

// ================================================================== SIOF + DMAC channel 0 (audio frame clock)
// One 32-bit frame per frame-sync (44.1 kHz = 1500 instructions). DMAC ch0 moves DMATCR frames from the SIOF receive FIFO to memory,
// sets CHCR.TE and raises DEI0 (INTEVT2 0x800) when IE is set. SIFTXI (INTEVT2 0xB20) is raised every `siof_irq_div` frames while
// SIIER.TDREQE is set.
// ---- ADC control/status (manual section 28): ADST starts a conversion that ends after 536 (CKS=0) or 266 (CKS=1) Pphi states; the ADF flag
// is set when all selected channels are done, ADST clears itself except in scan mode, and ADF is cleared only by writing 0 to it.
// Pphi is taken as 33.075 MHz, i.e. two emulated instructions per state.
static uint64_t adc_done_at; static int adc_running;
static int adc_channels(uint8_t csr) {
    int ch = csr & 7, multi = (csr >> 4) & 1;
    if (ch < 2) return 0;                                   // reserved selections
    if (ch == 2) return 1; if (ch == 3) return multi ? 2 : 1;
    return multi ? ch - 3 : 1;                              // AN4: 1, AN4-5: 2, AN4-6: 3, AN4-7: 4 (multi); single mode: one channel
}
static void adc_start(void) {
    uint8_t csr = soc_lo[0x90]; int n = adc_channels(csr);
    if (!n) { soc_lo[0x90] &= (uint8_t)~0x20; adc_running = 0; return; }
    adc_running = 1; adc_done_at = cpu.cycles + (uint64_t)n * ((csr & 8) ? 266 : 536) * 2;
}
static void adc_service(void) {
    if (!adc_running || cpu.cycles < adc_done_at) return;
    uint8_t csr = soc_lo[0x90]; int scan = ((csr >> 4) & 1) && (soc_lo[0x92] & 0x20);
    soc_lo[0x90] |= 0x80;
    if (scan && (csr & 0x20)) adc_start(); else { soc_lo[0x90] &= (uint8_t)~0x20; adc_running = 0; }
}
static void raise_module_irq(int pri, uint32_t code2) { intevt2 = code2; soc_irq_count++; sh3_interrupt(&cpu, 0x200u + 0x20u * (uint32_t)(15 - pri)); }
static int adc_irq_ready(void) {                         // ADI: ADF = 1 and ADIE = 1, level triggered
    if ((soc_lo[0x90] & 0xC0) != 0xC0 || (rd16(&soc_lo[0x1A]) & 15) <= ((cpu.sr >> 4) & 15)) return 0;
    if ((cpu.sr & SR_BL) || ((cpu.sr >> 16) & 0xFFF) || cpu.loop_pending) { soc_irq_blocked = 1; return 0; }
    return 1;
}
// DMAC channel 0 serves the SIOF receive requests: one request per frame. If the channel is not armed when a request arrives (the interrupt handler is
// late re-arming it), the request waits (the SIOF receive FIFO holds a few frames) and is served as soon as the channel is armed again; only after a
// backlog of 8 are requests dropped.
static int dma_req;
static void try_dma(void) {
    while (dma_req > 0) {
        uint32_t chcr = rd32(&soc_lo[0x2C]), tcr = rd32(&soc_lo[0x28]) & 0xFFFFFF, dar = rd32(&soc_lo[0x24]);
        if (!((chcr & 1) && !(chcr & 2) && tcr)) return;                      // DE = 1, TE = 0, count > 0
        for (uint32_t i = 0; i < tcr; i++) { bus_write(0, dar, 0, 4); dar += 4; }      // no audio input connected: frames of silence
        wr32(&soc_lo[0x24], dar); wr32(&soc_lo[0x28], 0); wr32(&soc_lo[0x2C], chcr | 2); dmac_done++;
        dma_req--;
    }
}
static void siof_frame(void) {
    siof_frames++;
    if ((siof_frames % siof_irq_div) == 0) {
        if (txi_pending < 8) txi_pending++;      // a late interrupt is queued (the SIOF FIFO absorbs a few frames of latency), not lost
        if (dma_req < 8) dma_req++;
    }
    try_dma();
}
static int dsp_loop_active(void) { return cpu.loop_pending || ((cpu.sr >> 16) & 0xFFF); }
static int siof_txi_ready(void) {
    if (!txi_pending) return 0;
    if (!(rd16(&soc_lo[0xD6]) & 0x1000)) { txi_pending = 0; return 0; }
    int pri = rd16(&soc_lo[0x220]) & 15, imask = (cpu.sr >> 4) & 15;
    if (pri <= imask) return 0;
    if ((cpu.sr & SR_BL) || dsp_loop_active()) { soc_irq_blocked = 1; return 0; }
    txi_pending--; return 1;
}
static int siof_dei0_ready(void) {
    if ((rd32(&soc_lo[0x2C]) & 6) != 6) return 0;                            // DEI0 is level triggered: CHCR.TE = 1 and CHCR.IE = 1
    int pri = (rd16(&soc_lo[0x1A]) >> 12) & 15, imask = (cpu.sr >> 4) & 15;
    if (pri <= imask) return 0;
    if ((cpu.sr & SR_BL) || dsp_loop_active()) { soc_irq_blocked = 1; return 0; }   // no interrupts inside a DSP repeat or with BL set
    return 1;
}

void soc_service(void) {
    soc_irq_blocked = 0;
    uint64_t dcyc = cpu.cycles - last_cycles; last_cycles = cpu.cycles;
    tmu_tick(dcyc); tmu_irq();
    if (cpu.cycles >= next_pad_refresh) { next_pad_refresh = cpu.cycles + 50000; pads_apply(); }
    panel_wheel_service();
    adc_service();
    try_dma();
    while (cpu.cycles >= frame_next) { frame_next += STEPS_PER_FRAME; siof_frame(); }
    // Module interrupts (INTEVT = level code H'200 + 20*(15-level), INTEVT2 = source code). The highest priority level wins; ties go to the
    // order DMAC, ADC, SIOF. TMU interrupts are handled above (their INTEVT is the fixed source code).
    if (siof_dei0_ready()) raise_module_irq((rd16(&soc_lo[0x1A]) >> 12) & 15, 0x800);
    else if (adc_irq_ready()) raise_module_irq(rd16(&soc_lo[0x1A]) & 15, 0x980);
    else if (siof_txi_ready()) raise_module_irq(rd16(&soc_lo[0x220]) & 15, 0xB20);
}

// Prints the interrupt priority registers and the enable bits of the peripherals the emulator can raise (for firmware debugging)
void soc_dump_intc(void) {
    uint16_t a = rd16(&soc_hi[0x1E2]), b = rd16(&soc_hi[0x1E4]);
    printf("INTC: IPRA=%04x IPRB=%04x IPRC=%04x IPRD=%04x IPRE=%04x IPRF=%04x IPRG=%04x  (priority nibbles; 0 = source masked)\n", a, b,
           rd16(&soc_lo[0x16]), rd16(&soc_lo[0x18]), rd16(&soc_lo[0x1A]), rd16(&soc_lo[0x220]), rd16(&soc_lo[0x222]));
    printf("      SIOF: SIMDR=%04x SISCR=%04x SITDAR=%04x SIRDAR=%04x SICDAR=%04x SICTR=%04x SIFCTR=%04x SITCR=%08x\n", rd16(&soc_lo[0xC0]), rd16(&soc_lo[0xC2]), rd16(&soc_lo[0xC4]), rd16(&soc_lo[0xC6]), rd16(&soc_lo[0xC8]), rd16(&soc_lo[0xCC]), rd16(&soc_lo[0xD0]), rd32(&soc_lo[0xE8]));
    printf("      FRQCR=%04x STBCR=%02x  ", rd16(&soc_hi[0x180]), soc_hi[0x102]);
    printf("TSTR=%02x TCR0=%04x TCR1=%04x TCR2=%04x  ADCSR=%02x  DMAC0 CHCR=%08x  SIIER=%04x  interrupts taken: %llu\n", soc_hi[0x92], rd16(&soc_hi[0x9C]),
           rd16(&soc_hi[0xA8]), rd16(&soc_hi[0xB4]), soc_lo[0x90], rd32(&soc_lo[0x2C]), rd16(&soc_lo[0xD6]), (unsigned long long)soc_irq_count);
}

void soc_reset(void) {
    memset(soc_lo, 0, sizeof soc_lo); memset(soc_hi, 0, sizeof soc_hi); memset(adc10, 0, sizeof adc10); adc_running = 0; soc_lo[0x92] = 0x07;
    memset(tmu_acc, 0, sizeof tmu_acc);
    intevt2 = 0; last_cycles = 0; frame_next = STEPS_PER_FRAME; next_pad_refresh = 0; txi_pending = 0; dma_req = 0;
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
    if (a == 0x04000158) v = 0x0060;          // SCSSR2 (SCIF / MIDI UART): transmitter always empty (TDFE | TEND), no receive data, no errors
    if (a == 0x0400015E) v = 0;               // SCFDR2: both FIFOs empty
    return v;
}
void soc_write(uint32_t a, uint32_t val, int size) {
    uint32_t o = a - SOC_BASE;
    if (a == 0x04000124 && size == 1) panel_led_strobe(val, soc_lo[0x136]);          // PCDR: column strobe; the LED row data was put on SCPDR first
    if (a == 0x04000090 && size == 1) {                                              // ADCSR: ADF can only be cleared; other bits are plain storage
        uint8_t old = soc_lo[0x90], nv = (uint8_t)((val & 0x7F) | (old & val & 0x80));
        soc_lo[0x90] = nv;
        if (nv & 0x20) { if (!adc_running || !(old & 0x20)) adc_start(); } else adc_running = 0;
        return;
    }
    if ((a == 0x04000092) && size == 1) { soc_lo[0x92] = (uint8_t)((val & 0xF8) | 0x07); return; }       // ADCR: bits 2:0 read as 1 (initial H'07)
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
