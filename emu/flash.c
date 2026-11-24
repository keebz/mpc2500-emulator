// NOR flash: one AMD AM29LV641DL (8 MiB, 128 uniform 64 KiB sectors) on a 16-bit bus.
//
// AMD/Fujitsu command set. In x16 mode the unlock cycles are word addresses 0x555 and 0x2AA, i.e. byte addresses 0xAAA and
// 0x554 (compared on the low 12 address bits). Program and erase complete instantly, so data polling and toggle-bit
// polling read back the final state straight away. Supported: reset (F0), autoselect (90), program (A0), unlock bypass
// (20, A0, 90/00), sector erase (80 AA 55 30), chip erase (80 AA 55 10), erase suspend/resume (accepted, no effect since erase
// is instantaneous), CFI query (98).
#include "board.h"
#include <stdlib.h>
#include <string.h>

uint8_t *flash_data;
uint64_t flash_programs, flash_erases;

enum { M_READ, M_AUTOSELECT, M_CFI };
static int cycle;          // position in the command sequence
static int mode;           // what a read returns
static int bypass;         // unlock bypass active
static int erase_pending;  // 0x80 setup received

int flash_attach(const uint8_t *image, size_t len) {
    if (len > FLASH_SIZE) return -1;
    if (!flash_data) flash_data = malloc(FLASH_SIZE);
    if (!flash_data) return -1;
    memset(flash_data, 0xFF, FLASH_SIZE);
    memcpy(flash_data, image, len);
    flash_programs = flash_erases = 0;
    flash_reset_state();
    return 0;
}

void flash_reset_state(void) { cycle = 0; mode = M_READ; bypass = 0; erase_pending = 0; dbg_update_windows(); }
int flash_in_read_mode(void) { return mode == M_READ; }

// autoselect words (word index = byte offset / 2): manufacturer, device, extended device IDs, sector protection
static uint32_t autoselect_word(uint32_t off) {
    switch ((off >> 1) & 0xFF) {
    case 0x00: return 0x0001;       // AMD
    case 0x01: return 0x227E;       // device ID (multi-word ID follows at 0x0E/0x0F)
    case 0x0E: return 0x2210;
    case 0x0F: return 0x2200;
    case 0x02: return 0x0000;       // sector protection: unprotected
    }
    return 0;
}
static uint32_t cfi_word(uint32_t off) {
    static const uint16_t q[0x30] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x51, 0x52, 0x59, 0x02, 0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0x27, 0x36, 0, 0,
                                      0x04, 0x00, 0x0A, 0x00, 0x05, 0x00, 0x04, 0x00, 0x17, 0x01, 0x00, 0x05, 0x00, 0x01, 0x7F, 0x00 };
    uint32_t w = (off >> 1) & 0xFF;
    return w < 0x30 ? q[w] : 0;
}

uint32_t flash_read(uint32_t off, int size) {
    uint32_t v = 0;
    if (mode == M_READ || off >= FLASH_SIZE) {
        for (int i = 0; i < size; i++) v |= (uint32_t)(off + i < FLASH_SIZE ? flash_data[off + i] : 0xFF) << (8 * i);
        return v;
    }
    uint32_t w = mode == M_AUTOSELECT ? autoselect_word(off) : cfi_word(off);
    return size == 1 ? ((off & 1) ? w >> 8 : w & 0xFF) : size == 2 ? w : w | (w << 16);
}

static void program(uint32_t a, uint32_t val, int size) {
    static int plog = -1; if (plog < 0) plog = getenv("FLASH_PLOG") != 0;
    if (plog) fprintf(stderr, "prog %06x %0*x @%llu pc=%08x pr=%08x\n", a, size * 2, val, (unsigned long long)cpu.cycles, cpu.pc, cpu.pr);
    flash_guard(a, 0);
    for (int i = 0; i < size && a + i < FLASH_SIZE; i++) flash_data[a + i] &= (uint8_t)(val >> (8 * i));   // programming only clears bits
    flash_programs++;
}
static void erase_sector(uint32_t a) {
    flash_guard(a, 1);
    uint32_t base = a & ~0xFFFFu;
    if (base < FLASH_SIZE) { memset(flash_data + base, 0xFF, 0x10000); flash_erases++; }
}

void flash_write(uint32_t a, uint32_t val, int size) {
    uint32_t cmd = val & 0xFF, lo = a & 0xFFF;
    if (cycle != 3 && getenv("FLASH_LOG") && cmd != 0xAA && cmd != 0x55) fprintf(stderr, "flash: cmd %02x @%06x pc=%08x\n", cmd, a, cpu.pc); if (cycle != 3 && cmd == 0xF0) { cycle = 0; mode = M_READ; erase_pending = 0; dbg_update_windows(); return; }   // reset (also leaves autoselect/CFI)
    switch (cycle) {
    case 0:
        if (lo == 0xAAA && cmd == 0xAA) cycle = 1;
        else if (lo == 0x0AA && cmd == 0x98) { mode = M_CFI; dbg_update_windows(); }
        else if (bypass && cmd == 0xA0) cycle = 3;
        else if (bypass && cmd == 0x90) cycle = 7;
        else if (cmd == 0xB0 || cmd == 0x30) { /* erase suspend / resume: erase is instantaneous */ }
        break;
    case 1: cycle = (lo == 0x554 && cmd == 0x55) ? 2 : 0; break;
    case 2:
        if (lo == 0xAAA && cmd == 0xA0) cycle = 3;
        else if (lo == 0xAAA && cmd == 0x80) cycle = 4;
        else if (lo == 0xAAA && cmd == 0x90) { if (getenv("FLASH_LOG")) fprintf(stderr, "flash: autoselect pc=%08x\n", cpu.pc); mode = M_AUTOSELECT; cycle = 0; dbg_update_windows(); }
        else if (lo == 0xAAA && cmd == 0x20) { bypass = 1; cycle = 0; }
        else cycle = 0;
        break;
    case 3: program(a, val, size); cycle = 0; break;
    case 4: cycle = (lo == 0xAAA && cmd == 0xAA) ? 5 : 0; break;
    case 5: cycle = (lo == 0x554 && cmd == 0x55) ? 6 : 0; break;
    case 6:
        if (cmd == 0x30) erase_sector(a);
        else if (lo == 0xAAA && cmd == 0x10) { for (uint32_t s = 0; s < FLASH_SIZE; s += 0x10000) erase_sector(s); }
        cycle = 0; break;
    case 7: if (cmd == 0x00) bypass = 0; cycle = 0; break;     // unlock-bypass reset: 90 then 00
    default: cycle = 0;
    }
}

int flash_save(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t n = fwrite(flash_data, 1, FLASH_SIZE, f); fclose(f);
    return n == FLASH_SIZE ? 0 : -1;
}
int flash_load_state(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    if (!flash_data) flash_data = malloc(FLASH_SIZE);
    size_t n = fread(flash_data, 1, FLASH_SIZE, f); fclose(f);
    flash_reset_state();
    return n == FLASH_SIZE ? 0 : -1;
}
