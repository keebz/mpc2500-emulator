// T6963C graphic LCD controller, 240x64, 8-pixel-wide cells (FS pin low), 64 KiB display RAM.
// Registers: data port at +0, command/status port at +4 of 0x16000000.
//
// Command set (all implemented unless noted):
//   0x21 cursor pointer  0x22 offset register  0x24 address pointer  0x40/41 text home/area  0x42/43 graphic home/area
//   0x80-0x8F mode set (OR/EXOR/AND/text attribute, internal CG ROM / external CG RAM)   0x90-0x9F display mode
//   0xA0-0xA7 cursor pattern   0xB0 auto write   0xB1 auto read   0xB2 auto reset
//   0xC0-0xC5 data write/read with address increment/decrement/none   0xE0 screen peek   0xE8 screen copy   0xF0-0xFF bit reset/set
// Not rendered: the hardware cursor, text-attribute mode, blinking.
#include "board.h"
#include <string.h>

uint64_t lcd_writes;

static const uint8_t cg_rom[128][8] = {
#include "cgrom.inc"
};

static struct {
    uint8_t vram[65536];
    uint8_t param[2]; int nparam;
    uint16_t ptr, texthome, textarea, grhome, grarea, cursor, offset;
    uint8_t mode, disp, cursor_lines, rdata;
    int autow, autor, error;
} L;

void lcd_reset(void) { memset(&L, 0, sizeof L); L.textarea = 30; L.grarea = 30; lcd_writes = 0; }

void lcd_data_write(uint8_t v) {
    lcd_writes++;
    if (L.autow) { L.vram[L.ptr++] = v; return; }
    L.param[0] = L.param[1]; L.param[1] = v; if (L.nparam < 2) L.nparam++;
}

uint8_t lcd_data_read(void) {
    if (L.autor) return L.vram[L.ptr++];
    return L.rdata;
}

uint8_t lcd_status_read(void) {
    return (uint8_t)(0x03 | (L.autor ? 0x04 : 0) | (L.autow ? 0x08 : 0) | (L.error ? 0x40 : 0));
}

void lcd_cmd_write(uint8_t c) {
    uint16_t w = (uint16_t)(L.param[0] | (L.param[1] << 8));   // first parameter byte = low, second = high
    uint8_t d1 = L.param[1];                                    // with a single parameter the byte is the most recent write
    if (c == 0xB0) L.autow = 1;
    else if (c == 0xB1) L.autor = 1;
    else if (c == 0xB2) L.autow = L.autor = 0;
    else if (c == 0x21) L.cursor = w;
    else if (c == 0x22) L.offset = w & 0x1F;
    else if (c == 0x24) L.ptr = w;
    else if (c == 0x40) L.texthome = w;
    else if (c == 0x41) L.textarea = w;
    else if (c == 0x42) L.grhome = w;
    else if (c == 0x43) L.grarea = w;
    else if ((c & 0xF0) == 0x80) L.mode = c;            // 0x80-0x87 internal CG ROM, 0x88-0x8F external CG RAM
    else if ((c & 0xF0) == 0x90) L.disp = c;
    else if ((c & 0xF8) == 0xA0) L.cursor_lines = (uint8_t)((c & 7) + 1);
    else if (c == 0xC0) { L.vram[L.ptr++] = d1; }
    else if (c == 0xC1) { L.rdata = L.vram[L.ptr++]; }
    else if (c == 0xC2) { L.vram[L.ptr--] = d1; }
    else if (c == 0xC3) { L.rdata = L.vram[L.ptr--]; }
    else if (c == 0xC4) { L.vram[L.ptr] = d1; }
    else if (c == 0xC5) { L.rdata = L.vram[L.ptr]; }
    else if (c == 0xE0) {                                       // screen peek: one byte of the composed display at the address pointer
        uint16_t gr = L.grarea ? L.grarea : 30;
        uint32_t rel = (uint16_t)(L.ptr - L.grhome);
        if ((L.disp & 8) && rel < (uint32_t)gr * LCD_H) L.rdata = L.vram[L.ptr]; else { L.rdata = 0; L.error = 1; }
        if (L.rdata || !L.error) L.error = 0;
    }
    else if (c == 0xE8) {                                       // screen copy: one 8-line character row from the screen to the address pointer
        uint16_t gr = L.grarea ? L.grarea : 30; uint16_t dst = L.ptr, src = L.ptr;
        (void)src;
        for (int line = 0; line < 8; line++) L.vram[(uint16_t)(dst + line * gr)] = L.vram[(uint16_t)(L.grhome + line * gr)];
    }
    else if ((c & 0xF0) == 0xF0) {                              // bit reset / set (bit number 0 = D0)
        uint8_t m = (uint8_t)(1u << (c & 7));
        if (c & 8) L.vram[L.ptr] |= m; else L.vram[L.ptr] &= (uint8_t)~m;
    }
    L.nparam = 0;
}

const uint8_t *lcd_glyph(int c) { return (c >= 0x20 && c < 0x80) ? cg_rom[c - 0x20] : cg_rom[0]; }
static const uint8_t *glyph(uint8_t code) {
    static uint8_t tmp[8];
    int ext = (L.mode & 8) != 0;                                 // external CG RAM for every code
    if (!ext && code < 0x80) return cg_rom[code];
    uint32_t base = ((uint32_t)L.offset << 11) + (uint32_t)code * 8;
    for (int i = 0; i < 8; i++) tmp[i] = L.vram[(uint16_t)(base + i)];
    return tmp;
}

void lcd_render(uint8_t out[LCD_BYTES]) {
    memset(out, 0, LCD_BYTES);
    const int cols = LCD_W / 8;
    if (L.disp & 8) {                                            // graphic layer
        uint16_t gr = L.grarea ? L.grarea : cols;
        for (int y = 0; y < LCD_H; y++) for (int x = 0; x < cols; x++) out[y * cols + x] = L.vram[(uint16_t)(L.grhome + y * gr + x)];
    }
    if (L.disp & 4) {                                            // text layer, combined with the graphic layer by the mode-set logic
        uint16_t ta = L.textarea ? L.textarea : cols;
        int logic = L.mode & 7;
        for (int row = 0; row < LCD_H / 8; row++) for (int col = 0; col < cols; col++) {
            const uint8_t *g = glyph(L.vram[(uint16_t)(L.texthome + row * ta + col)]);
            for (int line = 0; line < 8; line++) {
                uint8_t *o = &out[(row * 8 + line) * cols + col];
                if (!(L.disp & 8)) *o = g[line];
                else if (logic == 1) *o ^= g[line];
                else if (logic == 3) *o &= g[line];
                else *o |= g[line];
            }
        }
    }
}
