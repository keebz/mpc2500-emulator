// Board: memory map, bus dispatch, reset and the main run loop.
#include "board.h"
#include <stdlib.h>
#include <string.h>

SH3 cpu;
uint8_t *ram;
uint8_t xy_mem[XY_SIZE] __attribute__((aligned(8)));
uint32_t ram_installed = 0x01000000;       // 16 MB stock; a SODIMM replaces this value

static uint8_t fpga_regs[0x1000];          // FPGA register window (plain read/write storage)
static uint64_t fpga_cfg_bytes;            // bytes written to the FPGA configuration port
static uint8_t fpga_status_toggle;
static int halted;

uint64_t board_fpga_cfg_bytes(void) { return fpga_cfg_bytes; }

// ---- direct memory windows for the CPU core: RAM, flash (read mode only) and X/Y memory ----
void board_update_windows(void) {
    int observe = dbg_active();
    sh3_set_window(&cpu, 0, RAM_BASE, RAM_WINDOW, observe ? 0 : ram, 1);
    sh3_set_window(&cpu, 1, 0, FLASH_SIZE, (observe || !flash_in_read_mode() || !flash_data) ? 0 : flash_data, 0);
    sh3_set_window(&cpu, 2, XY_BASE, XY_SIZE, observe ? 0 : xy_mem, 1);
}

void board_init(void) {
    if (!ram) ram = calloc(1, RAM_WINDOW);
    cpu.bus = (Bus){ bus_read, bus_write, 0 };
    board_reset();
}

void board_reset(void) {
    memset(ram, 0, RAM_WINDOW); memset(xy_mem, 0, sizeof xy_mem); memset(fpga_regs, 0, sizeof fpga_regs);
    fpga_cfg_bytes = 0; fpga_status_toggle = 0; halted = 0;
    Bus bus = cpu.bus; sh3_reset(&cpu); cpu.bus = bus;
    soc_reset(); lcd_reset(); ata_reset(); audio_reset(); panel_reset(); fault_reset(); flash_reset_state();
    if (spd_present) spd_line_update();
    board_update_windows();
}

// ---- bus ----
static uint32_t load(const uint8_t *p, int size) { uint32_t v = 0; memcpy(&v, p, (size_t)size); return v; }

uint32_t bus_read(void *ctx, uint32_t addr, int size) {
    (void)ctx;
    if (size > 1 && (addr & (uint32_t)(size - 1))) fault_bus(2, addr, size, 0);
    uint32_t a = sh3_phys(addr), v = 0;
    if (a < FLASH_SIZE) v = flash_read(a, size);
    else if (a - RAM_BASE < RAM_WINDOW) v = load(ram + (a - RAM_BASE), size);
    else if (a - SOC_BASE < SOC_SIZE) v = soc_read(a, size);
    else if (a - XY_BASE < XY_SIZE) v = load(xy_mem + (a - XY_BASE), size);
    else if (addr >= P4_REGS) v = p4_read(addr, size);
    else if (a - ATA_BASE < 0x40) v = ata_read(a - ATA_BASE, size);
    else if (a - ATA_CTRL < 0x40) v = (a & 0x3C) == 0x18 ? ata_alt_status() : 0;
    else if (a == LCD_BASE + 4) v = lcd_status_read();
    else if (a == LCD_BASE) v = lcd_data_read();
    else if (a == FPGA_STATUS) { fpga_status_toggle ^= 0x10; v = fpga_status_toggle; }       // handshake/sync bit 4 toggles on every read
    else if (a - FPGA_REGS < sizeof fpga_regs) v = load(fpga_regs + (a - FPGA_REGS), size);
    else { fault_bus(0, addr, size, 0); return 0; }
    if (dbg_active()) dbg_on_read(a, v, size);
    return v;
}

void bus_write(void *ctx, uint32_t addr, uint32_t val, int size) {
    (void)ctx;
    if (size > 1 && (addr & (uint32_t)(size - 1))) fault_bus(3, addr, size, val);
    uint32_t a = sh3_phys(addr);
    if (dbg_active()) dbg_on_write(a, val, size);
    if (a < FLASH_SIZE) { flash_write(a, val, size); return; }
    if (a - RAM_BASE < RAM_WINDOW) { memcpy(ram + (a - RAM_BASE), &val, (size_t)size); return; }
    if (a - SOC_BASE < SOC_SIZE) { soc_write(a, val, size); return; }
    if (a - XY_BASE < XY_SIZE) { memcpy(xy_mem + (a - XY_BASE), &val, (size_t)size); return; }
    if (addr >= P4_REGS) { p4_write(addr, val, size); return; }
    if (a == FPGA_CFG) { fpga_cfg_bytes++; return; }
    if (a - FPGA_REGS < sizeof fpga_regs) { memcpy(fpga_regs + (a - FPGA_REGS), &val, (size_t)size); return; }
    if (a - ATA_BASE < 0x40) { ata_write(a - ATA_BASE, val, size); return; }
    if (a - ATA_CTRL < 0x40) { if ((a & 0x3C) == 0x18) ata_control_write(val); return; }
    if (a == LCD_BASE) { lcd_data_write((uint8_t)val); return; }
    if (a == LCD_BASE + 4) { lcd_cmd_write((uint8_t)val); return; }
    fault_bus(1, addr, size, val);
}

uint32_t bus_peek32(uint32_t addr) {       // side-effect free: RAM and flash only
    uint32_t a = sh3_phys(addr & ~3u);
    if (a < FLASH_SIZE && flash_data) return load(flash_data + a, 4);
    if (a - RAM_BASE < RAM_WINDOW) return load(ram + (a - RAM_BASE), 4);
    if (a - XY_BASE < XY_SIZE) return load(xy_mem + (a - XY_BASE), 4);
    return 0;
}

// ---- run loop ----
// Peripherals and interrupts are serviced every BATCH instructions. Returns 1 if emulation stopped on a fatal fault; once
// halted the machine stays halted until board_reset().
int board_run(uint64_t n) {
    if (halted) return 1;
    uint64_t end = cpu.cycles + n;
    int hooks = dbg_stepping();
    while (cpu.cycles < end) {
        if (hooks) {
            for (int k = 0; k < BATCH; k++) {
                uint32_t pc = cpu.pc;
                pc_ring[pc_ring_i++ & 63] = pc;
                dbg_on_step(pc);
                sh3_step(&cpu);
                if (cpu.illegal) break;
            }
        } else sh3_run(&cpu, BATCH, pc_ring, &pc_ring_i);
        if (cpu.illegal) { report_illegal(cpu.fault_pc ? cpu.fault_pc : cpu.pc); halted = 1; return 1; }
        fault_check();
        if (fault_fatal) { fault_report(fault_fatal - 1, cpu.pc); halted = 1; return 1; }
        soc_service();
    }
    return 0;
}
int board_halted(void) { return halted; }
