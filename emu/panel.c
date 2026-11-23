// Front panel: key matrix (5 columns x 11 rows), 16 velocity pads (via the ADC), the DATA wheel (quadrature on PFDR 1:0) and LEDs.
//
// Key matrix: column strobe = PCDR (0xA4000124) bits 7:3 one-hot (column c = 0x08 << c); rows read active low on PMDR (0xA4000138)
// bits 6:0 = rows 0-6 and PGDR (0xA400012C) bits 5:2 = rows 7-10.
//
// DATA wheel: the OS polls PFDR and decodes the 2-bit pin history; one detent is a full cycle returning to the rest state 3:
// clockwise 2,0,1,3 (history 0x87) and counter-clockwise 1,0,2,3 (history 0x4B). A transition is only delivered after the
// firmware has read the pins since the previous one, so no transition is lost while the OS is busy redrawing.
#include "board.h"
#include <string.h>

uint16_t panel_keys[PANEL_COLS];
uint8_t pad_velocity[17];
static uint64_t pad_hold_until[17];

// ---- keys ----
void panel_press(int col, int row, int down) {
    if (col < 0 || col >= PANEL_COLS || row < 0 || row >= PANEL_ROWS) return;
    if (down) panel_keys[col] |= (uint16_t)(1u << row); else panel_keys[col] &= (uint16_t)~(1u << row);
}
static int active_col(void) { uint8_t pc = soc_lo[0x124] & 0xF8; for (int c = 0; c < PANEL_COLS; c++) if (pc == (8u << c)) return c; return -1; }
uint8_t panel_port_pm(void) { int c = active_col(); uint16_t rows = c >= 0 ? panel_keys[c] : 0; return 0x80 | (uint8_t)(~rows & 0x7F); }
uint8_t panel_port_pg(void) { int c = active_col(); uint16_t rows = c >= 0 ? panel_keys[c] : 0; return (uint8_t)((card_present ? 0xC0 : 0xC3) | (((~(rows >> 7)) & 0xF) << 2)); }

// ---- pads: pad n (1..16, PAD 1 bottom-left) -> raw index r = 4*(3-(n-1)/4) + (n-1)%4 -> mux m = r/4, ADC channel = (r+3)%4 ----
static uint16_t vel_to_adc(int v) { int a = 12 + v * 243 / 127; return (uint16_t)(a > 255 ? 255 : a); }   // the OS registers a hit at ADC >= 10
void pads_apply(void) {
    for (int n = 1; n <= 16; n++) {
        int r = 4 * (3 - (n - 1) / 4) + (n - 1) % 4, m = r / 4, ch = (r + 3) % 4;
        int on = pad_velocity[n] || cpu.cycles < pad_hold_until[n];
        adc10[m][ch] = on ? (uint16_t)(vel_to_adc(pad_velocity[n] ? pad_velocity[n] : 100) << 2) : 0;
    }
}
void pad_down(int n, int vel) { if (n < 1 || n > 16) return; pad_velocity[n] = (uint8_t)vel; pad_hold_until[n] = cpu.cycles + 3000000; pads_apply(); }   // held >= 3M steps so the OS gets its 8 samples
void pad_up(int n) { if (n < 1 || n > 16) return; pad_velocity[n] = 0; pads_apply(); }

// ---- DATA wheel ----
static const uint8_t seq_cw[4] = { 2, 0, 1, 3 }, seq_ccw[4] = { 1, 0, 2, 3 };
static int pending;            // detents still to deliver (+ = clockwise)
static int idx, dir;           // position inside the current detent's 4-transition sequence
static uint8_t pins = 3;       // PFDR[1:0]
static int seen = 1;           // the firmware has read the pins since the last transition
static uint64_t next_at, last_move;
static int angle_units;        // quarter-detents, for the front-end graphic

void wheel_turn(int detents) {
    pending += detents;
    if (pending > 64) pending = 64; if (pending < -64) pending = -64;
}
int wheel_angle(void) { int a = (angle_units * 15 / 4) % 360; return a < 0 ? a + 360 : a; }
// Only reads made in normal context (SR.BL = 0) count as the firmware having looked: the OS also reads the pins from its timer
// interrupt (BL = 1), which is not the routine that decodes the wheel and does not poll while the screen is being redrawn.
uint8_t panel_port_pf(void) { if (!(cpu.sr & SR_BL)) seen = 1; return (uint8_t)(0xFC | pins); }
int wheel_pending(void) { return pending != 0 || idx != 0; }

void panel_wheel_service(void) {
    if (cpu.cycles < next_at) return;
    if (!seen && cpu.cycles < last_move + 1000000) return;       // fallback: firmware that only polls from an interrupt handler still gets the turn
    if (idx == 0) {
        if (!pending) return;
        dir = pending > 0 ? 1 : -1;
    }
    pins = dir > 0 ? seq_cw[idx] : seq_ccw[idx];
    idx++; angle_units += dir; seen = 0; next_at = cpu.cycles + 2000; last_move = cpu.cycles;
    if (idx == 4) { idx = 0; pending -= dir; }
}

int panel_led(int col, int bit) {
    // The stock OS keeps the LED states in a 5-byte table (one byte per matrix column, 4 bits each) at 0x881C005C.
    if (col < 0 || col >= PANEL_COLS || bit < 0 || bit > 7) return 0;
    return (ram[0x1c005c + col] >> bit) & 1;
}

void panel_reset(void) {
    memset(pad_velocity, 0, sizeof pad_velocity); memset(pad_hold_until, 0, sizeof pad_hold_until);
    pending = idx = 0; pins = 3; seen = 1; next_at = last_move = 0; dir = 1;
    /* held keys stay held across a power cycle (the user's finger does not move) */
}
