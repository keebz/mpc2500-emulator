// Firmware loading: turns a user-supplied file into a flash image the boot block accepts.
//
// Formats, recognised by content (the extension is only a hint):
//   build  "MPC2500 ..." header at 0x10000, boot block at 0-0xFFFF, <= 0xEFFF0 bytes (Akai's mpc2500.bin, JJOS .bin builds). The boot
//          block only starts an OS whose 16-bit word checksum over [0x10000, 0xEFFF0) is stored at 0xEFFF0; the real updater writes it,
//          so the loader computes and stores it.
//   test   "MPC2500 TEST" header at 0x10000 (JJOS XL / Light / OS3 .osx/.os3): the boot block runs it in place, no checksum, and it may
//          span up to 8 MiB of flash.
//   sos    OS-only payload (MPC2500.SOS = build[0x10000:]): needs a boot block, taken from a full build named as the donor.
//   dump   exactly 8 MiB: a whole-flash image, used unchanged.
#include "board.h"
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum { OS_START = 0x10000, SUM_END = 0xEFFF0, WINDOW_END = 0xEFFF4 };

static void appendf(Firmware *f, const char *fmt, ...) {
    size_t l = strlen(f->report); if (l >= sizeof f->report - 1) return;
    va_list ap; va_start(ap, fmt); vsnprintf(f->report + l, sizeof f->report - l, fmt, ap); va_end(ap);
}
static uint8_t *read_file(const char *path, size_t *len) {
    *len = 0;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    *len = (size_t)n;
    if (n > FLASH_SIZE) { fclose(f); return 0; }
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    return d;
}
static const char *sniff(const uint8_t *d, size_t n) {
    if (n == FLASH_SIZE) return "dump";
    if (n >= OS_START + 16 && !memcmp(d + OS_START, "MPC2500 TEST", 12)) return "test";
    if (n >= OS_START + 16 && !memcmp(d + OS_START, "MPC2500", 7)) return "build";
    if (n >= 16 && !memcmp(d, "MPC2500", 7)) return "sos";
    return "unknown";
}
static int boot_block_blank(const uint8_t *d) { for (int i = 0; i < OS_START; i++) if (d[i] != 0xFF) return 0; return 1; }
static uint32_t os_checksum(const uint8_t *img) {      // ~ sum of little-endian 16-bit words over [0x10000, 0xEFFF0)
    uint32_t s = 0;
    for (uint32_t o = OS_START; o < SUM_END; o += 2) s += (uint32_t)(img[o] | (img[o + 1] << 8));
    return ~s;
}
static void header_text(const uint8_t *d, size_t off, size_t n, char *out, size_t outn) {
    size_t i = 0;
    for (; i < outn - 1 && off + i < n && i < 40; i++) { uint8_t c = d[off + i]; out[i] = (c >= 32 && c < 127) ? (char)c : ' '; }
    out[i] = 0;
    while (i > 0 && out[i - 1] == ' ') out[--i] = 0;
}
static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
    return s ? s + 1 : path;
}

void fw_free(Firmware *f) { free(f->flash_image); memset(f, 0, sizeof *f); }

int fw_load_file(const char *path, const char *donor_path, Firmware *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    size_t n = 0; uint8_t *d = read_file(path, &n);
    const char *base = base_name(path);
    if (!d) {
        if (n > FLASH_SIZE) snprintf(err, errlen, "%s is %zu bytes; the flash device holds 8 MiB (0x800000)", base, n);
        else snprintf(err, errlen, "cannot read %s", path);
        return -1;
    }
    snprintf(out->name, sizeof out->name, "%s", base);
    const char *kind = sniff(d, n);
    char hdr[48];

    if (!strcmp(kind, "dump")) {
        out->flash_image = d; out->flash_len = n; out->big = 1;
        snprintf(out->kind, sizeof out->kind, "whole-flash dump");
        appendf(out, "  8 MiB whole-flash image, used unchanged\n");
        return 0;
    }
    if (!strcmp(kind, "test")) {
        header_text(d, OS_START, n, hdr, sizeof hdr);
        size_t len = (n + 0xFFFF) & ~(size_t)0xFFFF;
        uint8_t *img = malloc(len);
        if (!img) { free(d); snprintf(err, errlen, "out of memory"); return -1; }
        memset(img, 0xFF, len); memcpy(img, d, n); free(d);
        out->flash_image = img; out->flash_len = len; out->run_in_place = 1; out->big = n > SUM_END;
        snprintf(out->kind, sizeof out->kind, "run-in-place image");
        appendf(out, "  header at 0x10000: \"%s\"\n  run in place by the boot block (no checksum); %zu bytes, flash sectors 0-%zu\n", hdr, n, (n - 1) >> 16);
        if (n > SUM_END) {
            size_t last = (n - 1) >> 16;
            appendf(out, "  NOTE: this image extends past the stock OS window (0xEFFF0) and overwrites flash sector 15 (settings)");
            if (last >= 16) appendf(out, ", internal-data sectors 16-%zu (including the SEQS&SONGS header)", last < 41 ? last : 41);
            if (last >= 42) appendf(out, " and programs/samples sectors 42-%zu", last);
            appendf(out, ".\n  The stock updater cannot install it; reverting to stock needs the clear_internal OS first.\n");
        }
        return 0;
    }
    if (!strcmp(kind, "build") || !strcmp(kind, "sos")) {
        uint8_t *img = malloc(0x100000);
        if (!img) { free(d); snprintf(err, errlen, "out of memory"); return -1; }
        memset(img, 0xFF, 0x100000);
        size_t os_len;
        if (!strcmp(kind, "build")) {
            for (size_t i = WINDOW_END; i < n; i++) if (d[i] != 0xFF) {
                free(d); free(img);
                snprintf(err, errlen, "%s has data past 0xEFFF0, the end of the OS window the boot block validates; an image that large needs a 'MPC2500 TEST' header to be run in place", base);
                return -1;
            }
            if (boot_block_blank(d)) {
                free(d); free(img);
                snprintf(err, errlen, "%s has no boot block (bytes 0-0xFFFF are blank); it looks like an OS-only file - put a full build next to it as the boot-block donor", base);
                return -1;
            }
            memcpy(img, d, n > SUM_END ? SUM_END : n); os_len = n;
            snprintf(out->kind, sizeof out->kind, "OS build");
            header_text(d, OS_START, n, hdr, sizeof hdr);
        } else {
            size_t dn = 0; uint8_t *donor = donor_path ? read_file(donor_path, &dn) : 0;
            if (!donor || dn < OS_START || boot_block_blank(donor)) {
                free(donor); free(d); free(img);
                snprintf(err, errlen, "%s is an OS-only payload (MPC2500.SOS) and needs a boot block: put a full build (for example Akai's mpc2500.bin) in the same folder", base);
                return -1;
            }
            if (n > SUM_END - OS_START) { free(donor); free(d); free(img); snprintf(err, errlen, "%s is %zu bytes; the OS window holds %d", base, n, SUM_END - OS_START); return -1; }
            memcpy(img, donor, OS_START); memcpy(img + OS_START, d, n); free(donor);
            os_len = n + OS_START;
            snprintf(out->kind, sizeof out->kind, "OS-only (.SOS) + boot block of %s", base_name(donor_path));
            header_text(d, 0, n, hdr, sizeof hdr);
        }
        uint32_t sum = os_checksum(img);
        memcpy(img + SUM_END, &sum, 4);
        out->flash_image = img; out->flash_len = 0x100000; free(d);
        appendf(out, "  header: \"%s\"\n  %zu bytes (%zu%% of the %d-byte flash OS area); boot checksum %08x written at 0xEFFF0\n", hdr, os_len, os_len * 100 / SUM_END, SUM_END, sum);
        return 0;
    }
    free(d);
    snprintf(err, errlen, "%s is not MPC2500 firmware: no \"MPC2500\" header at byte 0 (OS-only .SOS) or at 0x10000 (full build / run-in-place image), and it is not an 8 MiB flash dump", base);
    return -1;
}

// ---- folder scanning ----
static int has_ext(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext); if (a < b) return 0;
    for (size_t i = 0; i < b; i++) if (tolower((unsigned char)name[a - b + i]) != ext[i]) return 0;
    return 1;
}
int fw_scan_folder(const char *dir, FwEntry *out, int max) {
    DIR *dp = opendir(dir); if (!dp) return -1;
    static const char *exts[] = { ".bin", ".sos", ".osx", ".os3", ".os", ".rom", ".img" };
    int n = 0; struct dirent *e;
    while ((e = readdir(dp)) && n < max) {
        if (e->d_name[0] == '.') continue;
        int hit = 0; for (unsigned i = 0; i < sizeof exts / sizeof *exts; i++) if (has_ext(e->d_name, exts[i])) hit = 1;
        if (!hit) continue;
        FwEntry *f = &out[n]; memset(f, 0, sizeof *f);
        snprintf(f->path, sizeof f->path, "%s/%s", dir, e->d_name); snprintf(f->name, sizeof f->name, "%.*s", (int)sizeof f->name - 1, e->d_name);
        size_t len = 0; uint8_t *d = read_file(f->path, &len);
        f->size = (long)len;
        if (!d) { snprintf(f->kind, sizeof f->kind, len > FLASH_SIZE ? "too large" : "unreadable"); n++; continue; }
        snprintf(f->kind, sizeof f->kind, "%s", sniff(d, len)); f->ok = strcmp(f->kind, "unknown") != 0;
        if (!strcmp(f->kind, "build") || !strcmp(f->kind, "test")) header_text(d, OS_START, len, f->note, sizeof f->note);
        else if (!strcmp(f->kind, "sos")) header_text(d, 0, len, f->note, sizeof f->note);
        free(d); n++;
    }
    closedir(dp);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(out[j].name, out[i].name) < 0) { FwEntry t = out[i]; out[i] = out[j]; out[j] = t; }
    return n;
}
int fw_find_donor(const char *dir, const char *exclude, char *path_out, size_t n) {
    FwEntry list[64]; int cnt = fw_scan_folder(dir, list, 64);
    for (int i = 0; i < cnt; i++) {
        if (exclude && !strcmp(list[i].path, exclude)) continue;
        if (!strcmp(list[i].kind, "build")) { snprintf(path_out, n, "%s", list[i].path); return 1; }
    }
    return 0;
}
