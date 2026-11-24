// Firmware chooser shown by the packaged app (and by any SDL build started without a usable firmware): lists the firmware files found in the firmware
// folder, accepts files dropped onto the window, and offers to create or build a CompactFlash card image. No terminal needed.
#ifdef USE_SDL
#include "board.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#define WIN_W 920
#define WIN_H 700
typedef struct { int x, y, w, h; const char *label; int id; } Btn;
enum { B_START = 1, B_OPEN, B_NEWCARD, B_BUILD, B_CARDDIR, B_QUIT, B_RESCAN, B_FILES, B_EJECT };

static const uint8_t lfont[96][16] = {
#include "launcherfont.inc"
};
static SDL_Texture *font_tex;
static void make_font(SDL_Renderer *r) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 96 * 8, 16, 32, SDL_PIXELFORMAT_ARGB8888);
    uint32_t *px = (uint32_t *)s->pixels;
    for (int c = 0; c < 96; c++) for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) px[y * 96 * 8 + c * 8 + x] = ((lfont[c][y] >> (7 - x)) & 1) ? 0xFFFFFFFFu : 0;
    font_tex = SDL_CreateTextureFromSurface(r, s); SDL_FreeSurface(s); SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND);
}
static void text(SDL_Renderer *r, int x, int y, int scale, SDL_Color c, const char *s, int maxcols) {
    SDL_SetTextureColorMod(font_tex, c.r, c.g, c.b); SDL_SetTextureAlphaMod(font_tex, c.a);
    for (int i = 0; s[i] && (maxcols <= 0 || i < maxcols); i++) {
        int ch = (unsigned char)s[i]; if (ch < 0x20 || ch >= 0x80) ch = '?';
        if (ch == '_') { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); SDL_Rect u = { x + i * 8 * scale, y + 14 * scale, 8 * scale, scale }; SDL_RenderFillRect(r, &u); continue; }
        SDL_Rect src = { (ch - 0x20) * 8, 0, 8, 16 }, dst = { x + i * 8 * scale, y, 8 * scale, 16 * scale }; SDL_RenderCopy(r, font_tex, &src, &dst);
    }
}
// word-wrapped text; returns the y after the last line
static int wrapped(SDL_Renderer *r, int x, int y, int scale, SDL_Color c, const char *s, int cols) {
    char line[256]; int ll = 0; const char *p = s;
    while (*p) {
        const char *e = p; while (*e && *e != ' ') e++; int wl = (int)(e - p);
        if (ll && ll + 1 + wl > cols) { line[ll] = 0; text(r, x, y, scale, c, line, 0); y += 18 * scale; ll = 0; }
        if (ll) line[ll++] = ' ';
        for (int i = 0; i < wl && ll < 250; i++) line[ll++] = p[i];
        p = e; while (*p == ' ') p++;
    }
    if (ll) { line[ll] = 0; text(r, x, y, scale, c, line, 0); y += 18 * scale; }
    return y;
}
// text drawing for the live window (cheat sheet and toolbar): the same bitmap font, created on first use for that window's renderer
void ui_draw_text(SDL_Renderer *r, int x, int y, int scale, SDL_Color c, const char *s) { if (!font_tex) make_font(r); text(r, x, y, scale, c, s, 0); }
static const char *kind_label(const FwEntry *e) {
    if (!strcmp(e->kind, "build")) return "OS build"; if (!strcmp(e->kind, "test")) return "run-in-place image";
    if (!strcmp(e->kind, "sos")) return "OS-only (.SOS)"; if (!strcmp(e->kind, "dump")) return "whole-flash dump"; return "not recognised";
}
static int has_ext(const char *f, const char *ext) { size_t a = strlen(f), b = strlen(ext); return a >= b && !strcasecmp(f + a - b, ext); }
// shows /home/me/... as ~/... (shorter, and the same for every user)
static const char *tilde(const char *path, char *buf, size_t n) {
    const char *h = getenv("HOME"); size_t hl = h ? strlen(h) : 0;
    if (hl > 1 && !strncmp(path, h, hl) && (path[hl] == '/' || !path[hl])) { snprintf(buf, n, "~%s", path + hl); return buf; }
    return path;
}
static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

// Start-up warning. Shown every time the window version starts; the user must scroll to the end and accept, otherwise (decline, Esc, closing the window)
// the application exits. Returns 1 = accepted, 0 = declined.
static const char *const disc_lines[] = {
#include "disclaimer_text.inc"
};
int ui_disclaimer(void) {
    const char *auto_t = getenv("DISCLAIMER_TEST");              // test hook only: "accept" / "decline" answer without showing the window
    if (auto_t && !strcmp(auto_t, "accept")) return 1;
    if (auto_t && !strcmp(auto_t, "decline")) return 0;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 0; }
    SDL_Window *win = SDL_CreateWindow("MPC2500 Emulator - Important notice", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : 0;
    if (!ren) { fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError()); return 0; }
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H); make_font(ren);
    const int top = 64, bot = WIN_H - 92, view = bot - top;
    int scroll = 0, total = view, result = -1, nag = 0; const char *inject = (auto_t && !strcmp(auto_t, "inject")) ? getenv("DISCLAIMER_INJECT") : 0; int frame = 0;
    Btn bdecl = { 24, WIN_H - 56, 300, 40, "Decline and quit (D / Esc)", 1 }, bacc = { WIN_W - 24 - 300, WIN_H - 56, 300, 40, "I accept (A)", 2 };
    SDL_Color white = { 235, 235, 240, 255 }, dim = { 150, 155, 165, 255 }, red = { 255, 110, 90, 255 }, warn = { 255, 200, 90, 255 };
    while (result < 0) {
        int maxs = total > view ? total - view : 0, at_end = scroll >= maxs - 2, act = 0;      // act: 1 decline, 2 accept
        if (inject && inject[frame / 3] && frame % 3 == 2) {                                     // test hook: p = page down, a = accept key, d = decline key, x = close window
            char c = inject[frame / 3]; SDL_Event ev; SDL_zero(ev);
            if (c == 'x') ev.type = SDL_QUIT; else { ev.type = SDL_KEYDOWN; ev.key.keysym.sym = c == 'p' ? SDLK_PAGEDOWN : c == 'a' ? SDLK_a : SDLK_d; }
            SDL_PushEvent(&ev);
        }
        frame++;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) act = 1;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_d) act = 1;
                else if (k == SDLK_a) act = 2;
                else if (k == SDLK_DOWN) scroll += 36; else if (k == SDLK_UP) scroll -= 36;
                else if (k == SDLK_PAGEDOWN || k == SDLK_SPACE) scroll += view - 36; else if (k == SDLK_PAGEUP) scroll -= view - 36;
                else if (k == SDLK_END) scroll = maxs; else if (k == SDLK_HOME) scroll = 0;
            } else if (e.type == SDL_MOUSEWHEEL) scroll -= e.wheel.y * 54;
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                if (mx >= bdecl.x && mx < bdecl.x + bdecl.w && my >= bdecl.y && my < bdecl.y + bdecl.h) act = 1;
                if (mx >= bacc.x && mx < bacc.x + bacc.w && my >= bacc.y && my < bacc.y + bacc.h) act = 2;
            }
        }
        if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs;
        if (act == 1) result = 0;
        else if (act == 2) { if (scroll >= maxs - 2) result = 1; else nag = 120; }              // cannot accept before the whole notice has been shown
        at_end = scroll >= maxs - 2;
        if (nag > 0) nag--;
        SDL_SetRenderDrawColor(ren, 22, 24, 28, 255); SDL_RenderClear(ren);
        text(ren, 24, 14, 2, red, "WARNING - EXPERIMENTAL SOFTWARE", 0);
        SDL_SetRenderDrawColor(ren, 40, 43, 50, 255); SDL_Rect box = { 24, top - 4, WIN_W - 48, view + 8 }; SDL_RenderFillRect(ren, &box);
        SDL_Rect clip = { 24, top, WIN_W - 48, view }; SDL_RenderSetClipRect(ren, &clip);
        int y = top + 4 - scroll;
        for (size_t i = 0; i < sizeof disc_lines / sizeof *disc_lines; i++) {
            const char *l = disc_lines[i];
            if (!*l) { y += 8; continue; }
            if (*l == '#') y = wrapped(ren, 36, y, 1, warn, l + 1, 104) + 2; else y = wrapped(ren, 36, y, 1, white, l, 104);
        }
        total = y + scroll - top + 8; SDL_RenderSetClipRect(ren, 0);
        if (total > view) {                                                                  // scroll bar
            int bh = view * view / total; if (bh < 24) bh = 24; int by = top + (view - bh) * scroll / (maxs ? maxs : 1);
            SDL_SetRenderDrawColor(ren, 90, 95, 105, 255); SDL_Rect sb = { WIN_W - 22, by, 8, bh }; SDL_RenderFillRect(ren, &sb);
        }
        text(ren, 24, WIN_H - 84, 1, nag ? red : dim, at_end ? "You have reached the end. Choose to accept or decline." : "Scroll to the end of the notice (mouse wheel, arrow keys, Page Down) to enable \"I accept\".", 0);
        SDL_Rect r1 = { bdecl.x, bdecl.y, bdecl.w, bdecl.h }, r2 = { bacc.x, bacc.y, bacc.w, bacc.h };
        SDL_SetRenderDrawColor(ren, 120, 40, 40, 255); SDL_RenderFillRect(ren, &r1);
        if (at_end) SDL_SetRenderDrawColor(ren, 70, 120, 50, 255); else SDL_SetRenderDrawColor(ren, 45, 48, 55, 255);
        SDL_RenderFillRect(ren, &r2);
        text(ren, r1.x + (r1.w - (int)strlen(bdecl.label) * 8) / 2, r1.y + 12, 1, white, bdecl.label, 0);
        SDL_Color ac = at_end ? white : dim; text(ren, r2.x + (r2.w - (int)strlen(bacc.label) * 8) / 2, r2.y + 12, 1, ac, bacc.label, 0);
        const char *shot = getenv("DISCLAIMER_SHOT"); static int sf; if (shot && ++sf == 6) { SDL_Surface *sfc = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_ARGB8888); SDL_RenderReadPixels(ren, 0, SDL_PIXELFORMAT_ARGB8888, sfc->pixels, sfc->pitch); SDL_SaveBMP(sfc, shot); SDL_FreeSurface(sfc); }
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }
    SDL_DestroyTexture(font_tex); font_tex = 0; SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    return result;
}

int ui_launcher(const char *fwdir, char *out_fw, size_t outn, const char *message) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 0; }
    SDL_Window *win = SDL_CreateWindow("MPC2500 Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : 0;
    if (!ren) { fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError()); return 0; }
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H); make_font(ren);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    char carddir[PATH_MAX + 16], cardfiles[PATH_MAX + 16], cardimg[PATH_MAX + 32], msg[400] = "", cardpath[PATH_MAX + 32] = "";
    int appm = app_mode();
    mkdir(fwdir, 0755);
    if (appm) { snprintf(carddir, sizeof carddir, "%s/card", app_user_dir()); snprintf(cardfiles, sizeof cardfiles, "%s/card-files", app_user_dir()); }
    else { snprintf(carddir, sizeof carddir, "card"); snprintf(cardfiles, sizeof cardfiles, "card-files"); }
    mkdir(carddir, 0755); mkdir(cardfiles, 0755);
    snprintf(cardimg, sizeof cardimg, "%s/card.img", carddir);
    if (message && *message) snprintf(msg, sizeof msg, "%s", message);

    FwEntry ent[64]; int n = 0, sel = 0, confirm_build = 0; Uint32 last_click = 0; int last_row = -1;
    #define RESCAN() do { n = fw_scan_folder(fwdir, ent, 64); if (n < 0) n = 0; if (sel >= n) sel = n ? n - 1 : 0; } while (0)
    RESCAN();
    {   // start on the last firmware used
        char last[PATH_MAX + 8]; if (app_setting_get("firmware", last, sizeof last)) for (int i = 0; i < n; i++) if (!strcmp(ent[i].path, last)) sel = i;
    }
    int result = -1;
    while (result < 0) {
        if (app_setting_get("card", cardpath, sizeof cardpath)) { if (!strcmp(cardpath, "none")) cardpath[0] = 0; } else { struct stat st; cardpath[0] = 0; if (stat(cardimg, &st) == 0) snprintf(cardpath, sizeof cardpath, "%s", cardimg); }
        SDL_Event e;
        int do_action = 0;
        { static int fr; if (getenv("LAUNCHER_ENTER") && ++fr == 3) do_action = B_START; }
        { static int fr2; const char *acts = getenv("LAUNCHER_ACTIONS");        // test hook: one action per 4 frames, b = build card, e = eject, s = start
          if (acts && ++fr2 % 4 == 0 && acts[fr2 / 4 - 1]) { char c = acts[fr2 / 4 - 1]; do_action = c == 'b' ? B_BUILD : c == 'e' ? B_EJECT : c == 's' ? B_START : 0; } }       // test hook: press Start after a few frames
        Btn btns[10]; int nb = 0;
        int by = WIN_H - 56, cy = 522;
        btns[nb++] = (Btn){ 36, cy, 210, 34, "1  Card-files folder (F)", B_FILES };
        btns[nb++] = (Btn){ 256, cy, 200, 34, "2  Build card (B)", B_BUILD };
        btns[nb++] = (Btn){ 466, cy, 140, 34, "Blank card (N)", B_NEWCARD };
        btns[nb++] = (Btn){ 616, cy, 110, 34, "Eject (E)", B_EJECT };
        btns[nb++] = (Btn){ 736, cy, 148, 34, "Show card (C)", B_CARDDIR };
        btns[nb++] = (Btn){ 24, by, 200, 36, "Start (Enter)", B_START };
        btns[nb++] = (Btn){ 236, by, 240, 36, "Firmware folder (O)", B_OPEN };
        btns[nb++] = (Btn){ 802, by, 94, 36, "Quit", B_QUIT };
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) result = 0;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) result = 0;
                else if (k == SDLK_UP && sel > 0) sel--;
                else if (k == SDLK_DOWN && sel + 1 < n) sel++;
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) do_action = B_START;
                else if (k == SDLK_o) do_action = B_OPEN; else if (k == SDLK_r) do_action = B_RESCAN;
                else if (k == SDLK_n) do_action = B_NEWCARD; else if (k == SDLK_b) do_action = B_BUILD; else if (k == SDLK_c) do_action = B_CARDDIR; else if (k == SDLK_f) do_action = B_FILES; else if (k == SDLK_e) do_action = B_EJECT;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                for (int i = 0; i < nb; i++) if (mx >= btns[i].x && mx < btns[i].x + btns[i].w && my >= btns[i].y && my < btns[i].y + btns[i].h) do_action = btns[i].id;
                int row = (my - 128) / 52; if (my >= 128 && row >= 0 && row < n && row < 4 && mx < WIN_W - 24) {
                    sel = row; Uint32 t = SDL_GetTicks(); if (row == last_row && t - last_click < 450) do_action = B_START; last_click = t; last_row = row;
                }
            } else if (e.type == SDL_MOUSEWHEEL) { if (e.wheel.y > 0 && sel > 0) sel--; else if (e.wheel.y < 0 && sel + 1 < n) sel++; }
            else if (e.type == SDL_DROPFILE) {
                char *f = e.drop.file; const char *bn = base_of(f); char dst[PATH_MAX + 160];
                if (has_ext(f, ".img")) {
                    snprintf(dst, sizeof dst, "%s/%s", carddir, bn);
                    if (!strcmp(f, dst) || app_copy_file(f, dst) == 0) { app_setting_set("card", dst); snprintf(msg, sizeof msg, "Card image %s will be used.", bn); }
                    else snprintf(msg, sizeof msg, "Could not copy %s into the card folder.", bn);
                } else {
                    snprintf(dst, sizeof dst, "%s/%s", fwdir, bn);
                    if (!strcmp(f, dst) || app_copy_file(f, dst) == 0) {
                        RESCAN(); for (int i = 0; i < n; i++) if (!strcmp(ent[i].name, bn)) sel = i;
                        snprintf(msg, sizeof msg, "%s added to the firmware folder.", bn);
                    } else snprintf(msg, sizeof msg, "Could not copy %s into the firmware folder.", bn);
                }
                SDL_free(f);
            }
        }
        if (do_action) {
            if (do_action != B_BUILD) confirm_build = 0;
            switch (do_action) {
            case B_START:
                if (n > 0 && ent[sel].ok) { snprintf(out_fw, outn, "%s", ent[sel].path); app_setting_set("firmware", ent[sel].path); result = 1; }
                else snprintf(msg, sizeof msg, n ? "That file is not a firmware image the emulator recognises." : "There is no firmware yet: drop a firmware file onto this window.");
                break;
            case B_OPEN: app_open_folder(fwdir); break;
            case B_RESCAN: RESCAN(); snprintf(msg, sizeof msg, "Folder rescanned."); break;
            case B_CARDDIR: app_open_folder(carddir); break;
            case B_FILES: mkdir(cardfiles, 0755); app_open_folder(cardfiles); snprintf(msg, sizeof msg, "Copy your kits and sounds into the folder that just opened, then click \"2  Build card\"."); break;
            case B_EJECT: app_setting_set("card", "none"); snprintf(msg, sizeof msg, "Card ejected: the emulator will start without a memory card."); break;
            case B_NEWCARD: {
                struct stat st;
                if (stat(cardimg, &st) == 0) snprintf(msg, sizeof msg, "%s already exists. Delete or rename it first.", cardimg);
                else if (app_make_blank_card(cardimg, 128) == 0) { app_setting_set("card", cardimg); snprintf(msg, sizeof msg, "Created an EMPTY 128 MB card (no kits on it). To get your files onto a card use steps 1 and 2 instead."); }
                else snprintf(msg, sizeof msg, "Could not create %s.", cardimg);
                break; }
            case B_BUILD: {
                struct stat st;
                if (stat(cardimg, &st) == 0 && !confirm_build) { confirm_build = 1; snprintf(msg, sizeof msg, "card.img exists. Press B again to replace it (a backup is kept as card.img.bak)."); break; }
                if (confirm_build) { char bak[PATH_MAX + 40]; snprintf(bak, sizeof bak, "%s.bak", cardimg); remove(bak); rename(cardimg, bak); }
                confirm_build = 0; char m[300];
                if (app_build_card(cardfiles, cardimg, m, sizeof m) == 0) app_setting_set("card", cardimg);
                snprintf(msg, sizeof msg, "%s", m); break; }
            }
        }
        // ---- draw ----
        SDL_SetRenderDrawColor(ren, 22, 24, 28, 255); SDL_RenderClear(ren);
        SDL_Color white = { 235, 235, 240, 255 }, dim = { 150, 155, 165, 255 }, lime = { 170, 200, 68, 255 }, warn = { 255, 200, 90, 255 };
        text(ren, 24, 14, 2, lime, "MPC2500 Emulator", 0); text(ren, 24 + 17 * 16 + 8, 22, 1, dim, APP_VERSION, 0);
        wrapped(ren, 24, 56, 1, dim, "Choose a firmware to run. Drop a firmware file onto this window to add it. No firmware is supplied with the emulator: use your own files.", 112);
        char line[PATH_MAX + 64]; { char tb[PATH_MAX + 8]; const char *fd = tilde(fwdir, tb, sizeof tb); size_t fl = strlen(fd); const char *tail = fl > 90 ? fd + fl - 88 : fd; snprintf(line, sizeof line, "Firmware folder: %s%s", fl > 90 ? "..." : "", tail); }
        text(ren, 24, 94, 1, dim, line, 112);
        SDL_SetRenderDrawColor(ren, 40, 43, 50, 255); SDL_Rect box = { 24, 118, WIN_W - 48, 222 }; SDL_RenderFillRect(ren, &box);
        if (n == 0) {
            int y = wrapped(ren, 44, 136, 2, white, "No firmware found.", 50) + 6;
            y = wrapped(ren, 44, y, 1, dim, "Drag a firmware file (.bin, .osx, .os3, .sos or an 8 MiB flash dump) onto this window,", 100);
            y = wrapped(ren, 44, y, 1, dim, "or copy it into the folder shown above and press R.", 100);
        }
        for (int i = 0; i < n && i < 4; i++) {
            SDL_Rect row = { 30, 128 + i * 52, WIN_W - 60, 48 };
            if (i == sel) { SDL_SetRenderDrawColor(ren, 70, 82, 40, 255); SDL_RenderFillRect(ren, &row); }
            SDL_Color c = ent[i].ok ? white : dim;
            char nm[64]; snprintf(nm, sizeof nm, "%s", ent[i].name); text(ren, 44, 128 + i * 52 + 2, 2, c, nm, 28);
            char k[96]; snprintf(k, sizeof k, "%s", kind_label(&ent[i])); text(ren, 44 + 29 * 16, 128 + i * 52 + 8, 1, ent[i].ok ? lime : warn, k, 24);
            char nt[256]; snprintf(nt, sizeof nt, "%ld KB   %s", ent[i].size / 1024, ent[i].note); text(ren, 44, 128 + i * 52 + 31, 1, dim, nt, 108);
        }
        if (n > 4) { char more[64]; snprintf(more, sizeof more, "%d of %d shown (Up/Down to choose)", sel + 1, n); text(ren, 600, 26, 1, dim, more, 0); }
        // ---- the memory card panel ----
        { SDL_SetRenderDrawColor(ren, 34, 38, 44, 255); SDL_Rect cb = { 24, 348, WIN_W - 48, 242 }; SDL_RenderFillRect(ren, &cb); }
        text(ren, 36, 356, 1, lime, "YOUR KITS AND SOUNDS LIVE ON THE MEMORY CARD", 0);
        { int y = 378;
          y = wrapped(ren, 36, y, 1, white, "1. Click \"Card-files folder\" and copy your kits and sounds (.PGM .SND .WAV .SEQ .ALL ...) into it. Sub-folders are fine; a folder named AUTOLOAD is loaded by the firmware at start-up, as on the real unit.", 106);
          y = wrapped(ren, 36, y, 1, white, "2. Click \"Build card\". This makes the card image from that folder (it replaces the current card; the old one is kept as card.img.bak). Do this again whenever you add files.", 106);
          y = wrapped(ren, 36, y, 1, white, "3. Click Start. In the firmware press LOAD to see the card. Anything the firmware saves to the card is kept in the card image.", 106);
          (void)y; }
        SDL_Color cc = cardpath[0] ? white : dim;
        if (cardpath[0]) { struct stat st; char tb2[PATH_MAX + 8]; const char *cp2 = tilde(cardpath, tb2, sizeof tb2); size_t cl = strlen(cp2); const char *ct = cl > 80 ? cp2 + cl - 78 : cp2; snprintf(line, sizeof line, "Card: %s%s (%ld MB)", cl > 80 ? "..." : "", ct, stat(cardpath, &st) == 0 ? (long)(st.st_size / 1048576) : 0L); }
        else snprintf(line, sizeof line, "Card: none inserted (build one with steps 1 and 2)");
        text(ren, 36, 496, 1, cc, line, 106);
        if (msg[0]) wrapped(ren, 24, 600, 1, warn, msg, 112);
        if (appm) { char tb3[PATH_MAX + 8], ln[PATH_MAX + 160]; const char *ud = tilde(app_user_dir(), tb3, sizeof tb3);
          snprintf(ln, sizeof ln, "Your files (firmware, card, settings) are in the folder %s - deleting the app does not delete it.", ud); text(ren, 24, WIN_H - 18, 1, dim, ln, 112); }
        for (int i = 0; i < nb; i++) {
            SDL_Rect b = { btns[i].x, btns[i].y, btns[i].w, btns[i].h }; SDL_SetRenderDrawColor(ren, btns[i].id == B_START ? 90 : 55, btns[i].id == B_START ? 120 : 60, btns[i].id == B_START ? 40 : 70, 255); SDL_RenderFillRect(ren, &b);
            text(ren, b.x + (b.w - (int)strlen(btns[i].label) * 8) / 2, b.y + 10, 1, white, btns[i].label, 0);
        }
        static int shot_frames; const char *shot = getenv("LAUNCHER_SHOT");        // test hook: save a screenshot after a few frames and quit
        if (shot && ++shot_frames == 5) { SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_ARGB8888); SDL_RenderReadPixels(ren, 0, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch); SDL_SaveBMP(sf, shot); SDL_FreeSurface(sf); result = 0; }
        SDL_RenderPresent(ren);
        if (result < 0) SDL_Delay(8);
    }
    SDL_DestroyTexture(font_tex); font_tex = 0; SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    return result;
}
#endif
