// MPC2500 board model: shared declarations for the emulator modules.
//
//   sh3.c / dsp.c   CPU core + DSP unit            flash.c   NOR flash (AM29LV641DL)     ata.c    CompactFlash card
//   board.c         memory map, bus, run loop      lcd.c     T6963C LCD controller       audio.c  SIOF audio capture/decode
//   soc.c           SH7727 on-chip peripherals     panel.c   keys, pads, wheel, LEDs     fault.c  diagnostics
//   dbg.c           watchpoints / breakpoints      fwload.c  firmware loading            script.c, cli.c, ui_sdl.c  front ends
#pragma once
#include "sh3.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ---- timing --------------------------------------------------------------------------------------------------------
// One emulated instruction = 1/CPU_HZ second. Calibrated against the stock OS: its TMU clock gives 1 ms = 1662 TMU0 ticks,
// and one audio frame (44.1 kHz) is steps_per_frame instructions (default 1500 = 66.15 M instr/s; the OS needs far less on average, and late audio requests are queued).
extern uint32_t steps_per_frame;       // emulated instructions per 44.1 kHz audio frame (default 1500); the instruction rate is 44100 x this
#define CPU_HZ (44100ull * steps_per_frame)
#define STEPS_PER_FRAME steps_per_frame
#define BATCH 64                       // instructions between peripheral/interrupt service points

// ---- physical memory map -------------------------------------------------------------------------------------------
#define FLASH_SIZE   0x00800000u       // 8 MiB (one AM29LV641DL)
#define SOC_BASE     0x04000000u       // on-chip peripherals seen at 0xA4000000
#define SOC_SIZE     0x00001000u
#define XY_BASE      0x05000000u       // X/Y/IL memory, 128 KB mirrored through 0x05000000-0x05FFFFFF
#define XY_SIZE      0x00020000u
#define RAM_BASE     0x08000000u       // SDRAM (onboard + SODIMM modelled as one window)
#define RAM_WINDOW   0x08000000u
#define FPGA_CFG     0x10000000u
#define LCD_BASE     0x16000000u
#define FPGA_REGS    0x16800000u
#define FPGA_STATUS  0x17000000u
#define ATA_BASE     0x1A000000u
#define ATA_CTRL     0x1A800000u
#define P4_REGS      0xFFFFFE00u       // on-chip peripherals in the P4 area

// ---- board.c -------------------------------------------------------------------------------------------------------
extern SH3 cpu;
extern uint8_t *ram;                   // RAM_WINDOW bytes, lazily committed
extern uint8_t xy_mem[XY_SIZE];
extern uint32_t ram_installed;         // bytes the OS may use (16 MB stock, or the SODIMM size)
void board_init(void);                 // allocate memory, create devices (flash/card must be attached first)
void board_reset(void);                // power cycle: CPU, RAM and every device reset; flash and card contents survive
int  board_run(uint64_t steps);        // run `steps` instructions; returns 1 if the CPU stopped on a fatal fault
uint32_t bus_read(void *ctx, uint32_t addr, int size);
void bus_write(void *ctx, uint32_t addr, uint32_t val, int size);
uint32_t bus_peek32(uint32_t addr);    // side-effect free read for diagnostics (RAM/flash only, else 0)
void board_update_windows(void);       // (re)install the CPU's direct memory windows (disabled while observing accesses)
int  board_halted(void);
uint64_t board_fpga_cfg_bytes(void);

// ---- flash.c -------------------------------------------------------------------------------------------------------
int  flash_attach(const uint8_t *image, size_t len);   // copies the image into the 8 MiB device (rest erased)
void flash_reset_state(void);
int  flash_in_read_mode(void);
uint32_t flash_read(uint32_t off, int size);
void flash_write(uint32_t off, uint32_t val, int size);
extern uint8_t *flash_data;            // FLASH_SIZE bytes
extern uint64_t flash_programs, flash_erases;
int  flash_save(const char *path);     // write the device contents (for persistence)
int  flash_load_state(const char *path);

// ---- ata.c ---------------------------------------------------------------------------------------------------------
int  card_attach(const uint8_t *image, size_t len);
int  card_attach_file(const char *path);
extern int card_present;
extern uint64_t ata_sectors_read, ata_sectors_written;
void ata_reset(void);
uint32_t ata_read(uint32_t off, int size);
void ata_write(uint32_t off, uint32_t val, int size);
uint8_t ata_alt_status(void);
void ata_control_write(uint32_t val);
int  card_save(const char *path);

// ---- lcd.c ---------------------------------------------------------------------------------------------------------
enum { LCD_W = 240, LCD_H = 64, LCD_BYTES = LCD_W / 8 * LCD_H };
void lcd_reset(void);
void lcd_data_write(uint8_t v);
void lcd_cmd_write(uint8_t c);
uint8_t lcd_data_read(void);
uint8_t lcd_status_read(void);
void lcd_render(uint8_t out[LCD_BYTES]);   // 1 bit per pixel, 30 bytes per row, bit 7 = leftmost, 1 = dark
extern uint64_t lcd_writes;

// ---- soc.c ---------------------------------------------------------------------------------------------------------
extern uint8_t soc_lo[SOC_SIZE];
extern uint8_t soc_hi[0x200];
extern uint16_t qlink_adc[4];          // Q-Link inputs (AN2 on each analog mux line), 10 bit
extern uint16_t adc10[4][4];           // ADC value per [mux line][channel], 10 bit (left aligned x4)
void soc_reset(void);
void soc_dump_intc(void);
uint32_t soc_read(uint32_t phys, int size);          // 0x04000000-0x04000FFF
void soc_write(uint32_t phys, uint32_t val, int size);
uint32_t p4_read(uint32_t addr, int size);           // 0xFFFFFE00-0xFFFFFFFF
void p4_write(uint32_t addr, uint32_t val, int size);
extern int soc_irq_blocked;            // set by soc_service while an interrupt waits for BL / a DSP loop: board_run then uses short batches
void soc_service(void);                // timers, SIOF frames and interrupt delivery; called every BATCH instructions
extern uint64_t soc_irq_count, siof_frames, dmac_done;
extern unsigned siof_irq_div;
// SODIMM SPD EEPROM on the I2C lines of port D
void spd_install(int megabytes);
extern int spd_present;
extern uint64_t spd_reads;
void spd_line_update(void);

// ---- audio.c -------------------------------------------------------------------------------------------------------
void audio_push(uint32_t siof_word);   // one 32-bit word written to the SIOF transmit FIFO
uint32_t audio_live_total(void);       // frames produced since reset
uint32_t audio_live_fill(void);        // frames waiting in the live ring
extern int audio_shift;                // 20-bit -> 16-bit right shift (default 4)
void audio_capture(int on);
int  audio_save_wav(const char *path);
void audio_live_enable(int on);
uint32_t audio_live_pop(uint32_t *word);   // returns 1 and a stereo frame (left in the high half) if one is queued
void audio_reset(void);

// ---- panel.c -------------------------------------------------------------------------------------------------------
enum { PANEL_COLS = 5, PANEL_ROWS = 11 };
extern uint16_t panel_keys[PANEL_COLS];   // pressed rows per matrix column
extern uint8_t pad_velocity[17];
void panel_reset(void);
void panel_press(int col, int row, int down);
void pad_down(int n, int vel);
void pad_up(int n);
void pads_apply(void);                 // refresh the pad ADC values (called periodically)
void wheel_turn(int detents);          // + = clockwise; each detent is a full quadrature cycle
int  wheel_angle(void);                // 0..359 for the front-end knob graphic
uint8_t panel_port_pm(void);           // PMDR (key rows 6:0, active low)
uint8_t panel_port_pg(void);           // PGDR (key rows 10:7, card detect)
uint8_t panel_port_pf(void);           // PFDR (wheel quadrature)
int  panel_led(int col, int bit);      // 1 if the LED is lit
void panel_led_strobe(uint32_t pcdr, uint8_t scpdr);   // latch LED row data (SCPDR, active low) for the strobed column
void panel_wheel_service(void);
int  wheel_pending(void);              // a turn is still being delivered to the firmware

// ---- fault.c / dbg.c -----------------------------------------------------------------------------------------------
enum { F_FLASH_BOOT, F_FLASH_DATA, F_UNMAPPED_R, F_UNMAPPED_W, F_MISALIGNED_R, F_MISALIGNED_W, F_BADPC, F_BADSP, F_ILLEGAL, F_HANG_MASKED, F_STUCK, F_NKINDS };
void fault_note(int kind, uint32_t addr, int size, uint32_t val, uint32_t pc);
void fault_bus(int kind_rw, uint32_t addr, int size, uint32_t val);   // 0 read unmapped, 1 write unmapped, 2 misaligned read, 3 misaligned write
void fault_check(void);                // pc/sp validity and the stuck-loop watchdog
void fault_summary(void);
void fault_report(int kind, uint32_t pc);
void fault_reset(void);
extern int fault_fatal;                // 0 or kind + 1
extern int fault_strict;               // misaligned/unmapped accesses are fatal
extern uint32_t pc_ring[64]; extern unsigned pc_ring_i;
void flash_guard(uint32_t a, int erase);
int  report_illegal(uint32_t pc);

typedef struct { uint32_t lo, hi; int on_read, on_write; } Watch;
void dbg_set_breakpoint(uint32_t pc, int slot);
void dbg_watch_write(uint32_t phys_lo, uint32_t len);
void dbg_watch_read(uint32_t phys_lo, uint32_t len);
void dbg_watch_clear(void);
void dbg_call_trace(int on);
int  dbg_active(void);                 // nonzero: the fast memory windows are bypassed so accesses can be observed
int  dbg_stepping(void);               // nonzero: per-instruction hooks (breakpoints, call trace) are active
void dbg_watch_any_pc(int on);
void dbg_watch_reset_budget(void);
void dbg_on_write(uint32_t phys, uint32_t val, int size);
void dbg_on_read(uint32_t phys, uint32_t val, int size);
void dbg_on_step(uint32_t pc);
void dbg_update_windows(void);

// ---- fwload.c ------------------------------------------------------------------------------------------------------
typedef struct {
    uint8_t *flash_image;              // device image ready to attach (boot block, OS, checksum)
    size_t flash_len;
    char kind[64];                     // human readable classification
    char name[128];
    char report[1024];                 // multi-line description of what was found / done
    int run_in_place;                  // "MPC2500 TEST" image: the boot block jumps straight to it, no checksum
    int big;                           // image extends past the stock OS window (JJOS XL style)
} Firmware;
int  fw_load_file(const char *path, const char *boot_donor, Firmware *out, char *err, size_t errlen);
void fw_free(Firmware *f);
// ---- application support (app.c, launcher.c): packaged builds ----
const char *app_exe_path(void);
const char *app_res_dir(void);          // bundle resource folder (skin/, panel_map.txt) or NULL when not running from a bundle
int  app_mode(void);                    // 1 = running from a bundle (user files live in the user's MPC2500 Emulator folder)
const char *app_user_dir(void);
int  app_setting_get(const char *key, char *out, size_t n);
void app_setting_set(const char *key, const char *val);
int  app_copy_file(const char *src, const char *dst);
void app_open_folder(const char *dir);
void app_restart(const char *firmware, const char *card, int pick);
int  app_make_blank_card(const char *path, int size_mb);
int  app_build_card(const char *srcdir, const char *outimg, char *msg, size_t msgn);
const uint8_t *lcd_glyph(int c);        // 8x8 glyph of an ASCII character (the LCD's stand-in character generator)
extern int card_dirty;                  // the firmware wrote to the card since it was attached
extern const char *g_fw_dir, *g_card_dir;   // firmware / card folders in use (set by the command line)
int  ui_disclaimer(void);   // start-up warning that must be accepted every launch (SDL builds); 1 = accepted, 0 = declined: the caller must quit
int  ui_launcher(const char *fwdir, char *out_fw, size_t n, const char *message);   // window to choose a firmware (SDL builds); 1 = chosen, 0 = quit
typedef struct { char path[512]; char name[128]; char kind[24]; int ok; char note[200]; long size; } FwEntry;
int  fw_scan_folder(const char *dir, FwEntry *out, int max);      // classify every candidate firmware file in a folder
int  fw_find_donor(const char *dir, const char *exclude, char *path_out, size_t n);   // a full build to borrow a boot block from

// ---- script.c / ui_sdl.c / cli.c ----
void script_run(const char *text, const char *outdir, const char *name);
int  script_experiments(const char *outdir, uint64_t boot_steps, int n, char **specs);
int  script_sweep(const char *outdir, uint64_t boot_steps);
int  ui_run(const char *skin_dir, const char *keymap_file);        // live window; returns when the user quits
