// Command line front end.
//
//   mpc2500emu [options] [firmware-file]            live window (default when built with SDL), firmware taken from the firmware/ folder if not given
//   mpc2500emu firmware-file --exp DIR BOOT name=script ...   scripted experiments (see script.c)
//   mpc2500emu firmware-file --sweep DIR [BOOT]               press every key and record the LCD
//   mpc2500emu firmware-file --steps N                        headless run for N instructions
//
// options: --firmware FILE  --firmware-dir DIR  --donor FILE (boot block for OS-only .SOS files)  --card IMAGE  --ram MB (0 = stock 16 MB)
//          --list  --live  --headless  --skin DIR  --keymap FILE  --fault-strict  --save-flash FILE  --flash-state FILE  --help
// environment (kept for scripts): CARD, SODIMM_MB, FAULT_STRICT, IRQ_DIV, HANG_SAMPLES
#include "board.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    printf("MPC2500 emulator\n\n"
           "usage: %s [options] [firmware-file]\n"
           "  With no firmware file, the emulator looks in ./firmware/ (or --firmware-dir) for the file to run.\n\n"
           "  --firmware FILE     firmware to run: an OS build (.bin), a run-in-place image (.osx/.os3), an OS-only .SOS, or an 8 MiB flash dump\n"
           "  --firmware-dir DIR  folder to search (default: firmware)\n"
           "  --donor FILE        full build to take the boot block from when running an OS-only .SOS\n"
           "  --card IMAGE        CompactFlash card image to insert (default: card/card.img if present)\n"
           "  --ram MB            installed memory: 0 = the stock 16 MB (default), otherwise a SODIMM of that size (e.g. 128 = the EXM128 expansion)\n"
           "  --audio-shift N     right shift from the 20-bit OS sample words to 16 bit (default 4 = the full 20-bit DAC range; smaller = louder but clips in WAV capture)\n"
           "  --list              list the firmware files found and exit\n"
           "  --headless          no window: run --steps N instructions, then print a summary\n"
           "  --skin DIR          front-panel artwork folder (default: skin)\n"
           "  --keymap FILE       keyboard map (default: panel_map.txt)\n"
           "  --fault-strict      treat unmapped / misaligned accesses as fatal\n"
           "  --save-flash FILE   write the flash contents when the emulator exits\n"
           "  --save-card FILE    write the CompactFlash image (including anything the firmware saved to it) to FILE when the emulator exits\n"
           "  --flash-state FILE  start from a saved whole-flash image instead of the firmware\n", prog);
}

const char *g_fw_dir = "firmware", *g_card_dir = "card";

static int is_number(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }

int main(int argc, char **argv) {
    const char *fw_path = 0, *fw_dir = 0, *donor = 0, *card_path = getenv("CARD"), *skin = "skin", *keymap = "panel_map.txt", *save_flash = 0, *flash_state = 0, *save_card = 0;
    int pick = 0, flash_fw = 0, unit_on = 0, direct = 0; const char *unit_path = 0;
    int ram_mb = getenv("SODIMM_MB") ? atoi(getenv("SODIMM_MB")) : 0, ram_given = getenv("SODIMM_MB") != 0, list = 0, headless = 0, want_live = 0;
    enum { M_AUTO, M_EXP, M_SWEEP, M_STEPS } mode = M_AUTO;
    uint64_t steps = 5000000; const char *exp_dir = 0; uint64_t boot_steps = 300000000ull; int exp_n = 0; char **exp_specs = 0;
    if (getenv("FAULT_STRICT")) fault_strict = 1;
    if (getenv("STEPS_PER_FRAME")) steps_per_frame = (uint32_t)atoi(getenv("STEPS_PER_FRAME"));
    if (getenv("IRQ_DIV")) siof_irq_div = (unsigned)atoi(getenv("IRQ_DIV")) ? (unsigned)atoi(getenv("IRQ_DIV")) : 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--firmware") && i + 1 < argc) fw_path = argv[++i];                 // direct load (developer shortcut: bypasses the installer)
        else if (!strcmp(a, "--flash") && i + 1 < argc) { fw_path = argv[++i]; flash_fw = 1; }    // write this OS to the virtual unit like the updater does
        else if (!strcmp(a, "--unit") && i + 1 < argc) { unit_path = argv[++i]; unit_on = 1; }    // keep the flash chip in this file (virtual unit)
        else if (!strcmp(a, "--direct")) direct = 1;
        else if (!strcmp(a, "--firmware-dir") && i + 1 < argc) fw_dir = argv[++i];
        else if (!strcmp(a, "--donor") && i + 1 < argc) donor = argv[++i];
        else if (!strcmp(a, "--card") && i + 1 < argc) card_path = argv[++i];
        else if (!strcmp(a, "--ram") && i + 1 < argc) { ram_mb = atoi(argv[++i]); ram_given = 1; }
        else if (!strcmp(a, "--audio-shift") && i + 1 < argc) audio_shift = atoi(argv[++i]);
        else if (!strcmp(a, "--skin") && i + 1 < argc) skin = argv[++i];
        else if (!strcmp(a, "--keymap") && i + 1 < argc) keymap = argv[++i];
        else if (!strcmp(a, "--save-flash") && i + 1 < argc) save_flash = argv[++i];
        else if (!strcmp(a, "--flash-state") && i + 1 < argc) flash_state = argv[++i];
        else if (!strcmp(a, "--save-card") && i + 1 < argc) save_card = argv[++i];
        else if (!strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "--pick")) pick = 1;
        else if (!strcmp(a, "--headless")) headless = 1;
        else if (!strcmp(a, "--live")) { want_live = 1; if (i + 1 < argc && argv[i + 1][0] != '-' && !is_number(argv[i + 1])) keymap = argv[++i]; }
        else if (!strcmp(a, "--fault-strict")) fault_strict = 1;
        else if (!strcmp(a, "--steps") && i + 1 < argc) { steps = strtoull(argv[++i], 0, 0); mode = M_STEPS; headless = 1; }
        else if (!strcmp(a, "--exp") && i + 2 < argc) { mode = M_EXP; exp_dir = argv[++i]; boot_steps = strtoull(argv[++i], 0, 0); exp_specs = &argv[i + 1]; exp_n = argc - i - 1; break; }
        else if (!strcmp(a, "--sweep")) { mode = M_SWEEP; exp_dir = i + 1 < argc ? argv[++i] : "sweep"; if (i + 1 < argc) boot_steps = strtoull(argv[++i], 0, 0); headless = 1; }
        else if (a[0] != '-' && !fw_path) fw_path = a;
        else if (a[0] != '-' && is_number(a)) { steps = strtoull(a, 0, 0); mode = M_STEPS; headless = 1; }
        else { fprintf(stderr, "unknown or incomplete option: %s (try --help)\n", a); return 2; }
    }
    if (mode == M_EXP || mode == M_SWEEP) headless = 1;
    const int card_given = card_path != 0;            // --card or CARD=: an explicit choice that the chooser does not override
    (void)want_live;

    char err[400] = "", path_buf[600], skin_buf[600], key_buf[600], card_buf[700], fw_dir_buf[700];
    const int appm = app_mode();
#ifdef USE_SDL
    const int window_mode = !headless && mode == M_AUTO && !list;
#else
    const int window_mode = 0;
#endif
#ifdef USE_SDL
    if (window_mode && !ui_disclaimer()) { fprintf(stderr, "Notice declined: exiting.\n"); return 0; }     // every launch, no exceptions
#endif
    if (appm) {                          // packaged build: resources inside the bundle, the user's files in their MPC2500 Emulator folder
        const char *rd = app_res_dir(), *ud = app_user_dir();
        if (!strcmp(skin, "skin")) { snprintf(skin_buf, sizeof skin_buf, "%s/skin", rd); skin = skin_buf; }
        if (!strcmp(keymap, "panel_map.txt")) { snprintf(key_buf, sizeof key_buf, "%s/panel_map.txt", rd); keymap = key_buf; }
        if (!fw_dir) { snprintf(fw_dir_buf, sizeof fw_dir_buf, "%s/firmware", ud); fw_dir = fw_dir_buf; }
        g_card_dir = strdup((snprintf(card_buf, sizeof card_buf, "%s/card", ud), card_buf));
        if (!card_path) {
            char saved[600]; FILE *t;
            if (app_setting_get("card", saved, sizeof saved) && !strcmp(saved, "none")) card_path = 0;                      // ejected in the launcher
            else if (app_setting_get("card", saved, sizeof saved) && (t = fopen(saved, "rb"))) { fclose(t); card_path = strdup(saved); }
            else if (!(app_setting_get("card", saved, sizeof saved) && !strcmp(saved, "none"))) { snprintf(card_buf, sizeof card_buf, "%s/card/card.img", ud); if ((t = fopen(card_buf, "rb"))) { fclose(t); card_path = strdup(card_buf); } }
        }
    }
    if (appm && !direct && !flash_state && !unit_on) { static char up[800]; snprintf(up, sizeof up, "%s/unit.flash", app_user_dir()); unit_path = up; unit_on = 1; }
    if (direct || flash_state) unit_on = 0;
    if (unit_on && unit_path) app_unit_set(unit_path);
    if (!fw_dir) fw_dir = "firmware";
    g_fw_dir = fw_dir;
    if (0 && !fw_path && !flash_state && window_mode && !pick && appm) {   // (disabled) the chooser is now always shown, so the card options stay reachable
                              // reuse the firmware chosen last time
        char saved[600]; FILE *t; if (app_setting_get("firmware", saved, sizeof saved) && (t = fopen(saved, "rb"))) { fclose(t); snprintf(path_buf, sizeof path_buf, "%s", saved); fw_path = path_buf; }
    }
    int need_pick = pick && window_mode;
    const char *pick_msg = 0;
#ifndef USE_SDL
    need_pick = 0;
#endif
    int action = LAUNCH_DIRECT;                      // what to do with the flash: power on the virtual unit / write an OS to it / load an image directly
    if (unit_on) action = flash_fw ? LAUNCH_FLASH : fw_path ? LAUNCH_DIRECT : LAUNCH_POWER;
    if (unit_on && action == LAUNCH_POWER && window_mode) need_pick = 1;
    for (;;) {                                                                    // choose and load the firmware; in a window build a failure returns to the chooser
    if (!fw_path && !flash_state && !need_pick && !(unit_on && action == LAUNCH_POWER)) {
        FwEntry list_e[64]; int n = fw_scan_folder(fw_dir, list_e, 64);
        if (n < 0 && !window_mode) { fprintf(stderr, "firmware folder '%s' not found.\nCreate it and put your firmware file in it, or pass --firmware FILE.\n", fw_dir); return 2; }
        int ok = 0, pick_i = -1;
        for (int i = 0; i < n; i++) if (list_e[i].ok && strcmp(list_e[i].kind, "sos") != 0) { ok++; pick_i = i; }
        if (list) {
            printf("firmware folder: %s\n", fw_dir);
            for (int i = 0; i < n; i++) printf("  %-34s %9ld bytes  %-12s %s\n", list_e[i].name, list_e[i].size, list_e[i].kind, list_e[i].note);
            if (!n) printf("  (no firmware files found)\n");
            return 0;
        }
        if (ok == 0) { for (int i = 0; i < n; i++) if (list_e[i].ok) pick_i = i; if (pick_i >= 0) ok = 1; }
        if (n <= 0 || ok != 1 || (window_mode && appm)) {      // the packaged app always shows the chooser (even with one firmware), so the card options stay reachable
            if (window_mode) need_pick = 1;
            else if (n == 0) { fprintf(stderr, "no firmware found in '%s'. Put an MPC2500 firmware file (.bin / .osx / .os3 / .sos) there.\n", fw_dir); return 2; }
            else {
                fprintf(stderr, "%d firmware files in '%s'; choose one with --firmware:\n", n, fw_dir);
                for (int i = 0; i < n; i++) fprintf(stderr, "  %-34s %-12s %s\n", list_e[i].name, list_e[i].kind, list_e[i].note);
                return 2;
            }
        } else { snprintf(path_buf, sizeof path_buf, "%s", list_e[pick_i].path); fw_path = path_buf; }
    }
#ifdef USE_SDL
    if (need_pick) {
        g_launch_action = LAUNCH_DIRECT;
        if (!ui_launcher(fw_dir, path_buf, sizeof path_buf, pick_msg)) return 0;
        action = unit_on ? g_launch_action : LAUNCH_DIRECT;
        fw_path = action == LAUNCH_POWER ? 0 : path_buf; need_pick = 0; pick_msg = 0;
    }
#endif
    if (list) { FwEntry le[64]; int n = fw_scan_folder(fw_dir, le, 64); printf("firmware folder: %s\n", fw_dir); for (int i = 0; i < n; i++) printf("  %-34s %9ld bytes  %-12s %s\n", le[i].name, le[i].size, le[i].kind, le[i].note); return 0; }

    Firmware fw; char donor_buf[600];
    if (unit_on && action == LAUNCH_POWER) {
        if (flash_load_state(unit_path) != 0) {
            snprintf(err, sizeof err, "There is no virtual unit yet. Select an Akai OS build in the list and use \"Flash OS\" to create it.");
            fprintf(stderr, "%s\n", err);
            if (window_mode) { need_pick = 1; fw_path = 0; pick_msg = err; continue; }
            return 2;
        }
        app_unit_active(1);
        printf("Virtual unit: %s\n", unit_path);
        break;
    } else if (flash_state) {
        if (flash_load_state(flash_state) != 0) { fprintf(stderr, "cannot load flash state %s\n", flash_state); return 2; }
        printf("Flash state: %s\n", flash_state);
        break;
    } else {
        if (!donor) { const char *slash = strrchr(fw_path, '/'); char dir[600]; if (slash) { snprintf(dir, sizeof dir, "%.*s", (int)(slash - fw_path), fw_path); } else snprintf(dir, sizeof dir, "."); if (fw_find_donor(dir, fw_path, donor_buf, sizeof donor_buf)) donor = donor_buf; }
        if (fw_load_file(fw_path, donor, &fw, err, sizeof err) != 0) {
            fprintf(stderr, "Cannot load firmware: %s\n", err);
            if (window_mode) { need_pick = 1; fw_path = 0; pick_msg = err; donor = 0; continue; }
            return 2;
        }
        printf("Firmware: %s - %s\n%s", fw.name, fw.kind, fw.report);
        if (unit_on && action == LAUNCH_FLASH) {            // like the real updater: only the OS window (sectors 1-14) is rewritten; boot block, settings and internal memory stay
            if (fw.run_in_place) {
                snprintf(err, sizeof err, "%s is an Extra Large (XL) OS image: it does not fit the OS area the boot block and the Akai OS updater can write, so they refuse it, exactly as on the real unit. It must be installed by an OS that has its own installer, and it overwrites part of the internal memory.", fw.name);
                fprintf(stderr, "%s\n", err); fw_free(&fw);
                if (window_mode) { need_pick = 1; fw_path = 0; pick_msg = err; donor = 0; continue; }
                return 2;
            }
            struct stat ust;
            if (fw.big || stat(unit_path, &ust) != 0 || flash_load_state(unit_path) != 0) { if (flash_attach(fw.flash_image, fw.flash_len) != 0) { fprintf(stderr, "cannot place the firmware in the flash device\n"); return 2; } }
            else { memcpy(flash_data + 0x10000, fw.flash_image + 0x10000, 0xE0000); flash_reset_state(); }
            app_unit_active(1); if (app_unit_save() != 0) fprintf(stderr, "could not write the virtual unit %s\n", unit_path);
            printf("Virtual unit: OS written to %s\n", unit_path);
        } else if (flash_attach(fw.flash_image, fw.flash_len) != 0) { fprintf(stderr, "cannot place the firmware in the flash device\n"); return 2; }
        fw_free(&fw);
        break;
    }
    }                                                                              // end of the choose-and-load loop
    if (appm && !card_given) {           // the chooser may have built / chosen / ejected a card after the settings were first read: use the current choice
        char saved[600], dflt[700]; FILE *t; card_path = 0;
        if (app_setting_get("card", saved, sizeof saved) && !strcmp(saved, "none")) card_path = 0;
        else if (app_setting_get("card", saved, sizeof saved) && (t = fopen(saved, "rb"))) { fclose(t); card_path = strdup(saved); }
        else { snprintf(dflt, sizeof dflt, "%s/card/card.img", app_user_dir()); if ((t = fopen(dflt, "rb"))) { fclose(t); card_path = strdup(dflt); } }
    }
    if (!card_path && !appm) { FILE *t = fopen("card/card.img", "rb"); if (t) { fclose(t); card_path = "card/card.img"; } }
    if (card_path) {
        if (card_attach_file(card_path) != 0) fprintf(stderr, "warning: cannot read card image %s; running without a card\n", card_path);
        else printf("Card: %s\n", card_path);
    }
    if (appm && !ram_given) { char rs[16]; if (app_setting_get("ram", rs, sizeof rs) && atoi(rs) > 16) ram_mb = atoi(rs); }      // the chooser's memory choice (default: the stock 16 MB)
    if (ram_mb == 16) ram_mb = 0;                          // 16 MB is the stock memory, not a module
    if (ram_mb > 0) { ram_installed = (uint32_t)ram_mb << 20; spd_install(ram_mb); } else ram_installed = 0x01000000;
    printf("Memory: %u MB%s\n", ram_installed >> 20, ram_mb > 0 ? " (SODIMM)" : " (stock)");
    board_init();

    int rc = 0;
    if (mode == M_EXP) rc = script_experiments(exp_dir, boot_steps, exp_n, exp_specs);
    else if (mode == M_SWEEP) rc = script_sweep(exp_dir, boot_steps);
    else if (headless || mode == M_STEPS) {
        board_run(steps);
        printf("end: pc=%08x sr=%08x steps=%llu\nR0-R7: ", cpu.pc, cpu.sr, (unsigned long long)cpu.cycles);
        for (int i = 0; i < 8; i++) printf("%08x ", cpu.r[i]);
        printf("\nR8-R15: "); for (int i = 8; i < 16; i++) printf("%08x ", cpu.r[i]);
        printf("\n"); soc_dump_intc(); fault_summary();
    } else {
#ifdef USE_SDL
        rc = ui_run(skin, keymap);
#else
        (void)skin; (void)keymap;
        fprintf(stderr, "this build has no window support (built without SDL2); use --headless/--steps/--exp, or rebuild with SDL2\n"); rc = 2;
#endif
    }
    app_unit_save();                                                              // the virtual unit keeps the flash (installed OS, settings, internal memory)
    if (appm && card_dirty && card_path && !save_card) { if (card_save(card_path) != 0) fprintf(stderr, "could not write the card image %s\n", card_path); }   // a real card keeps what was saved to it
    if (save_card) { if (card_save(save_card) == 0) printf("Card saved to %s\n", save_card); else fprintf(stderr, "could not write %s\n", save_card); }
    if (save_flash) { if (flash_save(save_flash) == 0) printf("Flash saved to %s\n", save_flash); else fprintf(stderr, "could not write %s\n", save_flash); }
    return rc;
}
