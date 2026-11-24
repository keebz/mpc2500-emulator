// Application support for the packaged (double-click) builds: where the bundle's resources are, the user's folder for firmware and cards,
// the settings file, card image creation (blank, or built from a folder of files) and restarting with another firmware. No SDL here.
//
// A "bundle" is recognised by its resource folder: <exe>/../share/mpc2500-emulator (AppImage / Linux tarball) or <exe>/../Resources (macOS .app).
// Environment overrides for testing: MPC2500_RESOURCES (resource folder), MPC2500_HOME (user folder).
#include "board.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char exe_path[PATH_MAX], res_dir[PATH_MAX], user_dir[PATH_MAX];
static int resolved;

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

const char *app_exe_path(void) {
    if (exe_path[0]) return exe_path;
#ifdef __APPLE__
    uint32_t n = sizeof exe_path; char tmp[PATH_MAX];
    if (_NSGetExecutablePath(tmp, &n) == 0 && realpath(tmp, exe_path)) return exe_path;
#else
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1); if (n > 0) { exe_path[n] = 0; return exe_path; }
#endif
    exe_path[0] = 0; return exe_path;
}

static void resolve(void) {
    if (resolved) return; resolved = 1;
    const char *env = getenv("MPC2500_RESOURCES");
    char cand[PATH_MAX];
    if (env && *env) { snprintf(cand, sizeof cand, "%s/skin/skin.bmp", env); if (exists(cand)) { snprintf(res_dir, sizeof res_dir, "%s", env); return; } }
    const char *exe = app_exe_path(); if (!*exe) return;
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", exe); char *s = strrchr(dir, '/'); if (!s) return; *s = 0;
    static const char *rel[] = { "../share/mpc2500-emulator", "../Resources", "resources" };
    for (int i = 0; i < 3; i++) {
        snprintf(cand, sizeof cand, "%s/%s/skin/skin.bmp", dir, rel[i]);
        if (exists(cand)) { snprintf(cand, sizeof cand, "%s/%s", dir, rel[i]); if (!realpath(cand, res_dir)) snprintf(res_dir, sizeof res_dir, "%s", cand); return; }
    }
}
const char *app_res_dir(void) { resolve(); return res_dir[0] ? res_dir : 0; }
int app_mode(void) { resolve(); return res_dir[0] != 0; }

static void mk(const char *p) { if (!is_dir(p)) mkdir(p, 0755); }
static void write_if_absent(const char *p, const char *text) { if (exists(p)) return; FILE *f = fopen(p, "w"); if (f) { fputs(text, f); fclose(f); } }

// The user's folder: <firmware> for firmware files, <card> for card images, <card-files> for files that "Build card" turns into a card image.
const char *app_user_dir(void) {
    if (user_dir[0]) return user_dir;
    const char *env = getenv("MPC2500_HOME"), *home = getenv("HOME");
    if (env && *env) snprintf(user_dir, sizeof user_dir, "%s", env);
    else if (home && *home) snprintf(user_dir, sizeof user_dir, "%s/MPC2500 Emulator", home);
    else snprintf(user_dir, sizeof user_dir, "./MPC2500 Emulator");
    char p[PATH_MAX + 32];
    mk(user_dir);
    snprintf(p, sizeof p, "%s/firmware", user_dir); mk(p);
    snprintf(p, sizeof p, "%s/card", user_dir); mk(p);
    snprintf(p, sizeof p, "%s/card-files", user_dir); mk(p);
    snprintf(p, sizeof p, "%s/firmware/README.txt", user_dir);
    write_if_absent(p, "Put your MPC2500 firmware files in this folder (or drag a file onto the emulator window).\n"
                       "Accepted: full builds (.bin), run-in-place test images (.osx .os3), OS-only .sos (needs a full build next to it), 8 MiB flash dumps.\n"
                       "No firmware is supplied with the emulator.\n");
    snprintf(p, sizeof p, "%s/card-files/README.txt", user_dir);
    write_if_absent(p, "Files placed here (and in sub-folders such as AUTOLOAD) become the contents of the CompactFlash card when you press B (Build card) in the\n"
                       "launcher. The card image is saved as ../card/card.img. Use 8.3 style names (for example KIT.PGM, BOOMA01.SND).\n");
    return user_dir;
}

// ---- settings: key=value lines ----
static void settings_path(char *out, size_t n) { snprintf(out, n, "%s/settings.txt", app_user_dir()); }
int app_setting_get(const char *key, char *out, size_t n) {
    char p[PATH_MAX + 32]; settings_path(p, sizeof p); FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[PATH_MAX + 64]; size_t kl = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == '=') { line[strcspn(line, "\r\n")] = 0; snprintf(out, n, "%s", line + kl + 1); found = 1; }
    fclose(f); return found && out[0];
}
void app_setting_set(const char *key, const char *val) {
    char p[PATH_MAX + 32], tmp[PATH_MAX + 40]; settings_path(p, sizeof p); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *o = fopen(tmp, "w"); if (!o) return; FILE *f = fopen(p, "r"); char line[PATH_MAX + 64]; size_t kl = strlen(key);
    if (f) { while (fgets(line, sizeof line, f)) if (!(strncmp(line, key, kl) == 0 && line[kl] == '=')) fputs(line, o); fclose(f); }
    if (val && *val) fprintf(o, "%s=%s\n", key, val);
    fclose(o); rename(tmp, p);
}

// ---- files ----
int app_copy_file(const char *src, const char *dst) {
    FILE *i = fopen(src, "rb"); if (!i) return -1; FILE *o = fopen(dst, "wb"); if (!o) { fclose(i); return -1; }
    char buf[65536]; size_t n; int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, i)) > 0) if (fwrite(buf, 1, n, o) != n) { rc = -1; break; }
    fclose(i); if (fclose(o) != 0) rc = -1; return rc;
}
void app_open_folder(const char *dir) {
    char cmd[PATH_MAX + 64];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "open '%s' >/dev/null 2>&1 &", dir);
#else
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", dir);
#endif
    if (system(cmd) != 0) { /* no file manager available: nothing to do */ }
}
void app_restart(const char *firmware, const char *card, int pick) {
    app_unit_save();                                  // keep what the virtual unit holds across the restart
    const char *exe = app_exe_path(); if (!*exe) return;
    char *argv[8]; int n = 0; argv[n++] = (char *)exe;
    if (pick) argv[n++] = (char *)"--pick";
    if (firmware && *firmware) { argv[n++] = (char *)"--firmware"; argv[n++] = (char *)firmware; }
    if (card && *card) { argv[n++] = (char *)"--card"; argv[n++] = (char *)card; }
    argv[n] = 0; fflush(stdout); execv(exe, argv);
}

// ---- cards ----
// A blank card is a zero-filled image: like a new CompactFlash card it must be formatted once in the firmware (Load/Save page, F3 UTILIT, F6 DO IT).
int app_make_blank_card(const char *path, int size_mb) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int rc = 0; if (fseek(f, (long)size_mb * 1048576L - 1, SEEK_SET) != 0 || fputc(0, f) == EOF) rc = -1;
    if (fclose(f) != 0) rc = -1; return rc;
}

typedef struct { char name[11]; uint8_t attr; uint16_t cluster; uint32_t size; char path[PATH_MAX + 300]; int is_dir; } CEnt;
static void name83(const char *fn, char out[11], char (*taken)[11], int ntaken) {
    char base[16] = "", ext[8] = ""; const char *dot = strrchr(fn, '.'); size_t bl = dot ? (size_t)(dot - fn) : strlen(fn);
    int bi = 0; for (size_t i = 0; i < bl && bi < 8; i++) { char c = fn[i]; if (c == ' ' || c == '.') continue; if (c >= 'a' && c <= 'z') c -= 32; if (strchr("\"*+,/:;<=>?[\\]|&", c)) c = '_'; base[bi++] = c; }
    int ei = 0; if (dot) for (const char *p = dot + 1; *p && ei < 3; p++) { char c = *p; if (c >= 'a' && c <= 'z') c -= 32; if (strchr("\"*+,/:;<=>?[\\]|&", c)) c = '_'; ext[ei++] = c; }
    if (!bi) base[bi++] = '_';
    for (int tries = 0; tries < 99; tries++) {
        char b2[16]; snprintf(b2, sizeof b2, "%s", base);
        if (tries) { char suf[8]; snprintf(suf, sizeof suf, "~%d", tries); size_t keep = 8 - strlen(suf); if (strlen(b2) > keep) b2[keep] = 0; strcat(b2, suf); }
        memset(out, ' ', 11); memcpy(out, b2, strlen(b2)); memcpy(out + 8, ext, strlen(ext));
        int clash = 0; for (int i = 0; i < ntaken; i++) if (!memcmp(taken[i], out, 11)) { clash = 1; break; }
        if (!clash) return;
    }
}
static int cmp_names(const void *a, const void *b) { return strcmp(((const CEnt *)a)->path, ((const CEnt *)b)->path); }

// Collects the regular files (and, one level down, the files of sub-folders) of `dir`.
static int scan_dir(const char *dir, CEnt *out, int max) {
    DIR *d = opendir(dir); if (!d) return 0; struct dirent *e; int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "README.txt")) continue;
        char p[PATH_MAX + 300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); struct stat st; if (stat(p, &st) != 0) continue;
        CEnt *c = &out[n]; memset(c, 0, sizeof *c); snprintf(c->path, sizeof c->path, "%s", p);
        if (S_ISREG(st.st_mode)) { c->size = (uint32_t)st.st_size; c->is_dir = 0; n++; }
        else if (S_ISDIR(st.st_mode)) { c->is_dir = 1; n++; }
    }
    closedir(d); qsort(out, (size_t)n, sizeof *out, cmp_names); return n;
}

// Builds a FAT16 card image (MBR + one partition, 8.3 names, files in the root and in sub-folders one level deep) from the contents of `srcdir`.
int app_build_card(const char *srcdir, const char *outimg, char *msg, size_t msgn) {
    static CEnt root[512]; static CEnt sub[8][256]; static int nsub[8];
    int nroot = scan_dir(srcdir, root, 512), ndirs = 0; uint64_t bytes = 0;
    for (int i = 0; i < nroot; i++) {
        if (root[i].is_dir) {
            if (ndirs >= 8) continue;
            nsub[ndirs] = scan_dir(root[i].path, sub[ndirs], 256);
            for (int k = 0; k < nsub[ndirs]; k++) if (!sub[ndirs][k].is_dir) bytes += sub[ndirs][k].size;
            ndirs++;
        } else bytes += root[i].size;
    }
    if (nroot == 0) { snprintf(msg, msgn, "The card-files folder is empty: put files there first."); return -1; }
    // size: 128 MB, or larger to fit the files (FAT16 caps a card at about 2 GB)
    uint64_t want = bytes + bytes / 8 + (4u << 20); int size_mb = 128;
    while ((uint64_t)size_mb * 1048576ull < want && size_mb < 2000) size_mb += 32;
    if ((uint64_t)size_mb * 1048576ull < want) { snprintf(msg, msgn, "The files are too large for a FAT16 card (about 2 GB)."); return -1; }
    uint32_t total = (uint32_t)size_mb * 2048u, start = 63, part = total - start, reserved = 1, nfats = 2, rootent = 512, rootsec = rootent * 32 / 512;
    uint32_t spc = 8; while ((part - reserved - rootsec) / spc >= 65000) spc *= 2;
    uint32_t fatsz = 1, clusters;
    for (;;) { uint32_t data = part - reserved - nfats * fatsz - rootsec; clusters = data / spc; uint32_t need = ((clusters + 2) * 2 + 511) / 512; if (need <= fatsz) break; fatsz = need; }
    if (clusters < 4085 || clusters >= 65525) { snprintf(msg, msgn, "Cannot lay out the card (%u clusters).", clusters); return -1; }
    uint8_t *fat = calloc(fatsz, 512), *rootdir = calloc(rootent, 32); if (!fat || !rootdir) { free(fat); free(rootdir); return -1; }
    fat[0] = 0xF8; fat[1] = fat[2] = fat[3] = 0xFF;
    uint32_t nextc = 2, csize = spc * 512;
    time_t tt = time(0); struct tm *tmv = localtime(&tt);
    uint16_t dt = (uint16_t)(((tmv->tm_year - 80) << 9) | ((tmv->tm_mon + 1) << 5) | tmv->tm_mday), tm = (uint16_t)((tmv->tm_hour << 11) | (tmv->tm_min << 5) | (tmv->tm_sec / 2));
    FILE *o = fopen(outimg, "wb"); if (!o) { snprintf(msg, msgn, "Cannot write %s", outimg); free(fat); free(rootdir); return -1; }
    if (fseek(o, (long)((uint64_t)total * 512 - 1), SEEK_SET) != 0 || fputc(0, o) == EOF) { fclose(o); free(fat); free(rootdir); return -1; }
    uint64_t dstart = ((uint64_t)start + reserved + nfats * fatsz + rootsec) * 512;
    int overflow = 0;
    #define ALLOC(nbytes, first_out) do { uint32_t n_ = (uint32_t)(((nbytes) + csize - 1) / csize); if (n_ < 1) n_ = 1; first_out = nextc; \
        if (nextc + n_ > clusters + 2) { overflow = 1; } else { for (uint32_t i_ = 0; i_ < n_; i_++) { uint32_t c_ = nextc + i_; uint16_t v_ = (i_ < n_ - 1) ? (uint16_t)(c_ + 1) : 0xFFFF; fat[c_ * 2] = v_ & 0xFF; fat[c_ * 2 + 1] = v_ >> 8; } nextc += n_; } } while (0)
    #define PUTENT(buf, idx, nm, at, cl, sz) do { uint8_t *e_ = (buf) + (idx) * 32; memset(e_, 0, 32); memcpy(e_, nm, 11); e_[11] = at; e_[14] = tm & 0xFF; e_[15] = tm >> 8; e_[16] = dt & 0xFF; e_[17] = dt >> 8; \
        e_[18] = dt & 0xFF; e_[19] = dt >> 8; e_[22] = tm & 0xFF; e_[23] = tm >> 8; e_[24] = dt & 0xFF; e_[25] = dt >> 8; e_[26] = (cl) & 0xFF; e_[27] = (cl) >> 8; \
        e_[28] = (sz) & 0xFF; e_[29] = ((sz) >> 8) & 0xFF; e_[30] = ((sz) >> 16) & 0xFF; e_[31] = ((sz) >> 24) & 0xFF; } while (0)
    int rent = 0, di = 0, nfiles = 0; static char names[512][11]; int nn = 0;
    for (int i = 0; i < nroot && !overflow && rent < (int)rootent; i++) {
        CEnt *c = &root[i]; const char *bn = strrchr(c->path, '/') + 1; char nm[11]; name83(bn, nm, names, nn);
        if (c->is_dir) {
            if (di >= 8) continue;
            int cnt = nsub[di]; uint32_t dsize = (uint32_t)(2 + cnt) * 32, first; ALLOC(dsize, first);
            if (overflow) break;
            uint8_t *dirbuf = calloc((dsize + csize - 1) / csize, csize); char sn[256][11]; int sk = 0;
            PUTENT(dirbuf, 0, ".          ", 0x10, first, 0); PUTENT(dirbuf, 1, "..         ", 0x10, 0, 0);
            int ent = 2;
            for (int k = 0; k < cnt; k++) {
                CEnt *f = &sub[di][k]; if (f->is_dir) continue;
                const char *fn = strrchr(f->path, '/') + 1; char fnm[11]; name83(fn, fnm, sn, sk); memcpy(sn[sk], fnm, 11); sk++;
                uint32_t ff; ALLOC(f->size, ff); if (overflow) break;
                PUTENT(dirbuf, ent, fnm, 0x20, f->size ? ff : 0, f->size); ent++; nfiles++;
                FILE *in = fopen(f->path, "rb"); if (in) { uint8_t *buf = malloc(csize); fseek(o, (long)(dstart + (uint64_t)(ff - 2) * csize), SEEK_SET); size_t r; while ((r = fread(buf, 1, csize, in)) > 0) fwrite(buf, 1, r, o); free(buf); fclose(in); }
            }
            fseek(o, (long)(dstart + (uint64_t)(first - 2) * csize), SEEK_SET); fwrite(dirbuf, 1, csize, o); free(dirbuf);
            PUTENT(rootdir, rent, nm, 0x10, first, 0); rent++; memcpy(names[nn++], nm, 11); di++;
        } else {
            uint32_t first; ALLOC(c->size, first); if (overflow) break;
            PUTENT(rootdir, rent, nm, 0x20, c->size ? first : 0, c->size); rent++; nfiles++; memcpy(names[nn++], nm, 11);
            FILE *in = fopen(c->path, "rb"); if (in) { uint8_t *buf = malloc(csize); fseek(o, (long)(dstart + (uint64_t)(first - 2) * csize), SEEK_SET); size_t r; while ((r = fread(buf, 1, csize, in)) > 0) fwrite(buf, 1, r, o); free(buf); fclose(in); }
        }
    }
    if (overflow) { fclose(o); remove(outimg); free(fat); free(rootdir); snprintf(msg, msgn, "The files do not fit on the card."); return -1; }
    uint8_t bs[512] = {0}, mbr[512] = {0};
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90; memcpy(bs + 3, "MSDOS5.0", 8);
    bs[11] = 0x00; bs[12] = 0x02; bs[13] = (uint8_t)spc; bs[14] = reserved; bs[16] = (uint8_t)nfats; bs[17] = rootent & 0xFF; bs[18] = rootent >> 8;
    if (part < 65536) { bs[19] = part & 0xFF; bs[20] = (part >> 8) & 0xFF; }
    bs[21] = 0xF8; bs[22] = fatsz & 0xFF; bs[23] = fatsz >> 8; bs[24] = 63; bs[26] = 16; bs[28] = start & 0xFF; bs[29] = (start >> 8) & 0xFF;
    if (part >= 65536) { bs[32] = part & 0xFF; bs[33] = (part >> 8) & 0xFF; bs[34] = (part >> 16) & 0xFF; bs[35] = (part >> 24) & 0xFF; }
    bs[36] = 0x80; bs[38] = 0x29; bs[39] = 0x32; bs[40] = 0x43; bs[41] = 0x50; bs[42] = 0x4D; memcpy(bs + 43, "MPC2500    ", 11); memcpy(bs + 54, "FAT16   ", 8); bs[510] = 0x55; bs[511] = 0xAA;
    #define CHS(lba, o3) do { uint32_t c_ = (lba) / (16 * 63), h_ = ((lba) / 63) % 16, s_ = (lba) % 63 + 1; (o3)[0] = (uint8_t)h_; (o3)[1] = (uint8_t)(((c_ >> 2) & 0xC0) | s_); (o3)[2] = (uint8_t)(c_ & 0xFF); } while (0)
    mbr[446] = 0x80; CHS(start, mbr + 447); mbr[450] = 0x06; CHS(total - 1, mbr + 451);
    mbr[454] = start & 0xFF; mbr[455] = (start >> 8) & 0xFF; mbr[456] = (start >> 16) & 0xFF; mbr[457] = (start >> 24) & 0xFF;
    mbr[458] = part & 0xFF; mbr[459] = (part >> 8) & 0xFF; mbr[460] = (part >> 16) & 0xFF; mbr[461] = (part >> 24) & 0xFF; mbr[510] = 0x55; mbr[511] = 0xAA;
    fseek(o, 0, SEEK_SET); fwrite(mbr, 1, 512, o);
    uint64_t base = (uint64_t)start * 512; fseek(o, (long)base, SEEK_SET); fwrite(bs, 1, 512, o);
    for (uint32_t k = 0; k < nfats; k++) { fseek(o, (long)(base + (uint64_t)(reserved + k * fatsz) * 512), SEEK_SET); fwrite(fat, 512, fatsz, o); }
    fseek(o, (long)(base + (uint64_t)(reserved + nfats * fatsz) * 512), SEEK_SET); fwrite(rootdir, 32, rootent, o);
    int rc = fclose(o); free(fat); free(rootdir);
    if (rc != 0) { snprintf(msg, msgn, "Writing the card image failed."); return -1; }
    snprintf(msg, msgn, "Built a %d MB card with %d files.", size_mb, nfiles); return 0;
}

// ---- virtual unit: the flash chip's contents in a file ----
int g_launch_action = LAUNCH_POWER;
static char unit_file[PATH_MAX + 32]; static int unit_on;
void app_unit_set(const char *path) { snprintf(unit_file, sizeof unit_file, "%s", path); }
const char *app_unit_path(void) { return unit_file; }
void app_unit_active(int on) { unit_on = on; }
int app_unit_save(void) {                         // written to a temporary file first so a crash never leaves a half-written unit
    if (!unit_on || !unit_file[0] || !flash_data) return 0;
    char tmp[PATH_MAX + 40]; snprintf(tmp, sizeof tmp, "%s.new", unit_file);
    if (flash_save(tmp) != 0) { remove(tmp); return -1; }
    return rename(tmp, unit_file) == 0 ? 0 : -1;
}
int app_unit_info(char *out, size_t n) {
    out[0] = 0; if (!unit_file[0]) return 0;
    FILE *f = fopen(unit_file, "rb"); if (!f) return 0;
    uint8_t b[48] = { 0 }; size_t got = 0;
    if (fseek(f, 0x10000, SEEK_SET) == 0) got = fread(b, 1, 40, f);
    fclose(f);
    size_t k = 0; int blank = 1;
    for (size_t i = 0; i < got && k < n - 1; i++) { if (b[i] != 0xFF) blank = 0; out[k++] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : ' '; }
    out[k] = 0; while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
    if (blank || strncmp(out, "MPC2500", 7)) snprintf(out, n, "no OS installed");
    else { char *r = out; char *w = out; int sp = 0; while (*r) { if (*r == ' ') { if (!sp) *w++ = ' '; sp = 1; } else { *w++ = *r; sp = 0; } r++; } *w = 0; }   // squeeze the padding
    return 1;
}
void app_unit_reset(void) { if (unit_file[0]) remove(unit_file); }
