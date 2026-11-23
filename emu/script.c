// Scripted control of the emulator for tests and experiments.
//
// A script is a list of verbs separated by ';'. Examples:  "tap 0 3; wait 20000000; padhit 2 100; dump a"
//   press c r | release c r | tap c r           matrix key (column, row); tap = press, 6M steps, release, 10M steps
//   wait N                                       run N instructions
//   wheel d n                                    turn the DATA wheel n detents (d >= 0 clockwise), wait until the firmware has seen them
//   pad n [vel] | padup n | padhit n [vel]       pad press / release / press-wait-release
//   adc mux ch value | adcclear                  raw ADC injection
//   dump tag                                     LCD frame -> <dir>/<name>_<tag>.bin (1920 bytes) and LED table -> <dir>/<name>.txt
//   audio 0|1 | audiosave tag                    capture decoded audio, write <dir>/<name>_<tag>.wav
//   ramfull path | poke off val | flashsave path | flashstat | faults | tmu | leds
//   watchw lo len | watchr lo len | wreset | watchoff | watchpc any|min   memory watchpoints (physical addresses, hex)
//   bp slot pc | ctron | ctroff                  breakpoint / first-call trace
//   reset                                        power cycle (flash and card contents survive)
#include "board.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static char g_dir[300] = ".", g_name[80] = "x";

static void path_for(char *out, size_t n, const char *tag, const char *ext) { snprintf(out, n, "%s/%s_%s.%s", g_dir, g_name, tag, ext); }
static void dump_lcd(const char *tag) {
    uint8_t fb[LCD_BYTES]; lcd_render(fb);
    char p[400]; path_for(p, sizeof p, tag, "bin");
    FILE *f = fopen(p, "wb"); if (f) { fwrite(fb, 1, LCD_BYTES, f); fclose(f); }
    snprintf(p, sizeof p, "%s/%s.txt", g_dir, g_name);
    f = fopen(p, "a");
    if (f) {
        fprintf(f, "%s led", tag); for (int c = 0; c < 5; c++) fprintf(f, " %02x", ram[0x1c005c + c]);
        fprintf(f, " | ringidx %u ring", bus_peek32(0x881c003c)); for (int k = 0; k < 64; k += 4) { uint32_t w = bus_peek32(0x8820f4f4 + k); for (int j = 0; j < 4; j++) fprintf(f, " %02x", (w >> (8 * j)) & 0xFF); }
        fprintf(f, "\n"); fclose(f);
    }
}
static void wait_wheel(void) { uint64_t limit = cpu.cycles + 400000000ull; while (wheel_pending() && cpu.cycles < limit) if (board_run(100000)) return; board_run(4000000); }

void script_run(const char *text, const char *outdir, const char *name) {
    if (outdir) snprintf(g_dir, sizeof g_dir, "%s", outdir);
    if (name) snprintf(g_name, sizeof g_name, "%s", name);
    char buf[8192]; snprintf(buf, sizeof buf, "%s", text); char *save = 0;
    for (char *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(0, ";", &save)) {
        char cmd[16]; int a = 0, b = 0; unsigned long long n = 0;
        if (sscanf(tok, " %15s %d %d", cmd, &a, &b) < 1) continue;
        if (!strcmp(cmd, "press")) panel_press(a, b, 1);
        else if (!strcmp(cmd, "release")) panel_press(a, b, 0);
        else if (!strcmp(cmd, "tap")) { panel_press(a, b, 1); board_run(6000000); panel_press(a, b, 0); board_run(10000000); }
        else if (!strcmp(cmd, "wait")) { sscanf(tok, " %*s %llu", &n); board_run(n); }
        else if (!strcmp(cmd, "wheel")) { wheel_turn(a >= 0 ? (b ? b : 1) : -(b ? b : 1)); wait_wheel(); }
        else if (!strcmp(cmd, "pad")) pad_down(a, b ? b : 100);
        else if (!strcmp(cmd, "padup")) pad_up(a);
        else if (!strcmp(cmd, "padhit")) { pad_down(a, b ? b : 100); board_run(4000000); pad_up(a); board_run(6000000); }
        else if (!strcmp(cmd, "adc")) { int ch = 0, val = 0; sscanf(tok, " %*s %d %d %d", &a, &ch, &val); adc10[a & 3][ch & 3] = (uint16_t)(val << 2); }
        else if (!strcmp(cmd, "adcclear")) memset(adc10, 0, sizeof adc10);
        else if (!strcmp(cmd, "dump")) { char tag[32] = "x"; sscanf(tok, " %*s %31s", tag); dump_lcd(tag); }
        else if (!strcmp(cmd, "audio")) audio_capture(a);
        else if (!strcmp(cmd, "audiosave")) { char tag[80] = "x", p[400]; sscanf(tok, " %*s %79s", tag); path_for(p, sizeof p, tag, "wav"); audio_save_wav(p); }
        else if (!strcmp(cmd, "ramfull")) { char nm[300]; if (sscanf(tok, " %*s %299s", nm) == 1) { FILE *f = fopen(nm, "wb"); if (f) { fwrite(ram, 1, 0x4000000, f); fclose(f); } } }
        else if (!strcmp(cmd, "poke")) { unsigned long off = 0, val = 0; sscanf(tok, " %*s %lx %lx", &off, &val); if (off + 4 <= RAM_WINDOW) memcpy(ram + off, &val, 4); }
        else if (!strcmp(cmd, "flashsave")) { char nm[300]; if (sscanf(tok, " %*s %299s", nm) == 1 && flash_save(nm) == 0) { printf("FLASH saved %s\n", nm); fflush(stdout); } }
        else if (!strcmp(cmd, "flashstat")) { printf("FLASH programs=%llu sector erases=%llu\n", (unsigned long long)flash_programs, (unsigned long long)flash_erases); fflush(stdout); }
        else if (!strcmp(cmd, "faults")) fault_summary();
        else if (!strcmp(cmd, "leds")) { printf("LED table:"); for (int c = 0; c < 5; c++) printf(" %02x", ram[0x1c005c + c]); printf("\n"); fflush(stdout); }
        else if (!strcmp(cmd, "reset")) { board_reset(); }
        else if (!strcmp(cmd, "watchw") || !strcmp(cmd, "watchr")) { unsigned long lo = 0, len = 1; sscanf(tok, " %*s %lx %lx", &lo, &len); if (cmd[5] == 'w') dbg_watch_write((uint32_t)lo, (uint32_t)len); else dbg_watch_read((uint32_t)lo, (uint32_t)len); }
        else if (!strcmp(cmd, "watchoff")) dbg_watch_clear();
        else if (!strcmp(cmd, "wreset")) dbg_watch_reset_budget();
        else if (!strcmp(cmd, "watchpc")) { char w[8] = ""; sscanf(tok, " %*s %7s", w); dbg_watch_any_pc(!strcmp(w, "any")); }
        else if (!strcmp(cmd, "bp")) { unsigned long pc = 0; sscanf(tok, " %*s %d %lx", &a, &pc); dbg_set_breakpoint((uint32_t)pc, a); }
        else if (!strcmp(cmd, "ctron")) dbg_call_trace(1);
        else if (!strcmp(cmd, "ctroff")) dbg_call_trace(0);
        else fprintf(stderr, "script: unknown verb '%s'\n", cmd);
        fflush(stdout);
    }
}

#ifndef _WIN32
// --exp <dir> <boot_steps> name=script ... : fork per experiment from the booted state. PRELUDE=... runs in the parent first.
int script_experiments(const char *outdir, uint64_t boot_steps, int n, char **specs) {
    mkdir(outdir, 0755); snprintf(g_dir, sizeof g_dir, "%s", outdir);
    board_run(boot_steps);
    for (int i = 0; i < n; i++) {
        if (!strncmp(specs[i], "PRELUDE=", 8)) { script_run(specs[i] + 8, outdir, "prelude"); fflush(stdout); continue; }
        fflush(stdout); pid_t pid = fork();
        if (pid == 0) {
            char *eq = strchr(specs[i], '='); if (!eq) _exit(1);
            *eq = 0; script_run(eq + 1, outdir, specs[i]); fflush(stdout); _exit(0);
        }
        int st; waitpid(pid, &st, 0);
    }
    return 0;
}

// press every matrix cell on a snapshot (fork) of the booted firmware and record the LCD
int script_sweep(const char *outdir, uint64_t boot_steps) {
    mkdir(outdir, 0755);
    board_run(boot_steps);
    if (getenv("PRELUDE")) { script_run(getenv("PRELUDE"), outdir, "prelude"); board_run(6000000); }
    snprintf(g_dir, sizeof g_dir, "%s", outdir);
    for (int c = 0; c < 5; c++) for (int r = 0; r < 11; r++) {
        fflush(stdout); pid_t pid = fork();
        if (pid == 0) {
            char nm[40], tag[16]; snprintf(nm, sizeof nm, "cell_%d_%d", c, r);
            panel_press(c, r, 1); board_run(8000000); snprintf(tag, sizeof tag, "press"); script_run("dump press", outdir, nm);
            panel_press(c, r, 0); board_run(16000000); script_run("dump release", outdir, nm);
            fflush(stdout); _exit(0);
        }
        int st; waitpid(pid, &st, 0);
    }
    return 0;
}
#else
int script_experiments(const char *outdir, uint64_t boot_steps, int n, char **specs) { (void)outdir; (void)boot_steps; (void)n; (void)specs; fprintf(stderr, "--exp needs fork(): not available on Windows\n"); return 1; }
int script_sweep(const char *outdir, uint64_t boot_steps) { (void)outdir; (void)boot_steps; fprintf(stderr, "--sweep needs fork(): not available on Windows\n"); return 1; }
#endif
