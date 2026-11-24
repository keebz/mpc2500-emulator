// CompactFlash card in True-IDE (memory-mapped ATA) mode at phys 0x1A000000, register stride 4:
//   +0x00 data (16 bit)  +0x04 error/feature  +0x08 sector count  +0x0C LBA 7:0  +0x10 LBA 15:8  +0x14 LBA 23:16
//   +0x18 drive/head + LBA 27:24  +0x1C status/command;  control block at 0x1A800000, alternate status at +0x18.
// The card image is held in memory; writes never touch the file unless card_save() is called.
#include "board.h"
#include <stdlib.h>
#include <string.h>

int card_present, card_dirty;
uint64_t ata_sectors_read, ata_sectors_written;

static uint8_t *card; static size_t card_bytes;
static struct { uint8_t err, count, lba0, lba1, lba2, head, status, cmd; uint8_t buf[512]; int pos, remain; uint32_t lba; int writing; } ata;

enum { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40 };

int card_attach(const uint8_t *image, size_t len) {
    free(card); card = malloc(len ? len : 1);
    if (!card) return -1;
    memcpy(card, image, len); card_bytes = len; card_present = 1; card_dirty = 0;
    ata_reset();
    return 0;
}
int card_attach_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    free(card); card = malloc((size_t)n);
    if (!card || fread(card, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(card); card = 0; return -1; }
    fclose(f); card_bytes = (size_t)n; card_present = 1; card_dirty = 0;
    ata_reset();
    return 0;
}
int card_save(const char *path) {
    if (!card) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t n = fwrite(card, 1, card_bytes, f); fclose(f);
    return n == card_bytes ? 0 : -1;
}

void ata_reset(void) {
    memset(&ata, 0, sizeof ata);
    ata.status = card_present ? (ST_DRDY | ST_DSC) : 0;
}

// Translated CHS geometry reported by IDENTIFY: 16 heads x 63 sectors, cylinders capped at 16383 (as large CF cards do)
static uint32_t geo_cyl(void) { uint32_t sec = (uint32_t)(card_bytes / 512), c = sec / (16 * 63); return c > 16383 ? 16383 : c; }
// Drive/head bit 6 selects LBA (28-bit) or CHS addressing. Returns 0 and sets ata.lba, or -1 if the address is outside the card.
static int ata_address(void) {
    uint32_t sec = (uint32_t)(card_bytes / 512);
    if (ata.head & 0x40) ata.lba = ata.lba0 | (ata.lba1 << 8) | (ata.lba2 << 16) | ((uint32_t)(ata.head & 0x0F) << 24);
    else {
        uint32_t cyl = ata.lba1 | (ata.lba2 << 8), hd = ata.head & 0x0F, sc = ata.lba0;
        if (sc == 0 || sc > 63 || hd >= 16 || cyl >= geo_cyl()) return -1;
        ata.lba = (cyl * 16 + hd) * 63 + (sc - 1);
    }
    uint32_t n = ata.count ? ata.count : 256;
    return ((uint64_t)ata.lba + n <= sec) ? 0 : -1;
}
static void abort_idnf(void) { ata.status = ST_DRDY | ST_DSC | ST_ERR; ata.err = 0x10; ata.remain = 0; ata.writing = 0; }
static void load_sector(void) {
    if ((uint64_t)ata.lba * 512 + 512 <= card_bytes) memcpy(ata.buf, card + (size_t)ata.lba * 512, 512); else memset(ata.buf, 0, 512);
    ata.pos = 0; ata.status = ST_DRDY | ST_DSC | ST_DRQ; ata_sectors_read++;
}
static void identify(void) {
    uint16_t w[256] = {0}; uint32_t sec = (uint32_t)(card_bytes / 512); uint32_t cyl = geo_cyl();
    w[0] = 0x848A;                                    // CompactFlash signature
    w[22] = 4; w[47] = 0x0001; w[49] = 0x0200;
    w[1] = (uint16_t)cyl; w[3] = 16; w[6] = 63;
    w[49] = 0x0200;                                   // LBA supported
    w[53] = 1; w[54] = (uint16_t)cyl; w[55] = 16; w[56] = 63; w[57] = (uint16_t)((cyl * 16 * 63) & 0xFFFF); w[58] = (uint16_t)((cyl * 16 * 63) >> 16);
    w[60] = sec & 0xFFFF; w[61] = sec >> 16;
    const char *ser = "EMU0000000000000001", *mod = "MPC2500 EMULATED CF CARD";
    for (int i = 0; i < 10; i++) w[10 + i] = (uint16_t)((ser[2 * i] << 8) | ser[2 * i + 1]);
    for (int i = 0; i < 20; i++) { char a = 2 * i < (int)strlen(mod) ? mod[2 * i] : ' ', b = 2 * i + 1 < (int)strlen(mod) ? mod[2 * i + 1] : ' '; w[27 + i] = (uint16_t)((a << 8) | b); }
    for (int i = 0; i < 256; i++) { ata.buf[2 * i] = w[i] & 0xFF; ata.buf[2 * i + 1] = (uint8_t)(w[i] >> 8); }
    ata.pos = 0; ata.remain = 1; ata.status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void command(uint8_t c) {
    ata.cmd = c; ata.err = 0; ata.writing = 0;
    static int alog = -1; if (alog < 0) alog = getenv("ATA_LOG") != 0;
    if (alog) fprintf(stderr, "ata: cmd %02x lba %02x%02x%02x head %02x count %02x\n", c, ata.lba2, ata.lba1, ata.lba0, ata.head, ata.count);
    if (!card_present) { ata.status = 0; return; }
    switch (c) {
    case 0x20: case 0x21: case 0xC4:                                    // READ SECTORS / READ MULTIPLE
        if (ata_address()) { abort_idnf(); break; } ata.remain = ata.count ? ata.count : 256; load_sector(); break;
    case 0x30: case 0x31: case 0xC5:                                    // WRITE SECTORS / WRITE MULTIPLE
        if (ata_address()) { abort_idnf(); break; } ata.remain = ata.count ? ata.count : 256; ata.writing = 1; ata.pos = 0; ata.status = ST_DRDY | ST_DSC | ST_DRQ; break;
    case 0x90: ata.status = ST_DRDY | ST_DSC; ata.err = 1; ata.count = 1; ata.lba0 = 1; ata.lba1 = ata.lba2 = 0; ata.head &= 0xF0; break;   // EXECUTE DEVICE DIAGNOSTIC: no error, device 0 signature
    case 0xEC: identify(); break;                                       // IDENTIFY DEVICE
    case 0xEF: case 0x91: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5: case 0xE6: case 0xE7: case 0xC6: case 0x40: case 0x41: case 0x10: case 0x00:
        ata.status = ST_DRDY | ST_DSC;                                  // SET FEATURES, INIT PARAMS, standby/idle/sleep, SET MULTIPLE, VERIFY, RECALIBRATE, NOP
        if (c == 0xE5) ata.count = 0xFF;                                // CHECK POWER MODE: active
        break;
    default: ata.status = ST_DRDY | ST_DSC | ST_ERR; ata.err = 0x04; break;   // aborted command
    }
}

uint32_t ata_read(uint32_t off, int size) {
    (void)size;
    if (!card_present) return 0;
    switch (off) {
    case 0x00: {
        if (!(ata.status & ST_DRQ)) return 0;
        uint32_t v = ata.buf[ata.pos] | (ata.buf[ata.pos + 1] << 8); ata.pos += 2;
        if (ata.pos >= 512) {
            if (--ata.remain > 0 && ata.cmd != 0xEC) { ata.lba++; load_sector(); } else ata.status = ST_DRDY | ST_DSC;
        }
        return v;
    }
    case 0x04: return ata.err;
    case 0x08: return ata.count;
    case 0x0C: return ata.lba0;
    case 0x10: return ata.lba1;
    case 0x14: return ata.lba2;
    case 0x18: return ata.head;
    case 0x1C: case 0x38: return ata.status;
    }
    return 0;
}

void ata_write(uint32_t off, uint32_t v, int size) {
    (void)size;
    if (!card_present) return;
    switch (off) {
    case 0x00:
        if (ata.writing && (ata.status & ST_DRQ)) {
            ata.buf[ata.pos] = (uint8_t)v; ata.buf[ata.pos + 1] = (uint8_t)(v >> 8); ata.pos += 2;
            if (ata.pos >= 512) {
                if ((uint64_t)ata.lba * 512 + 512 <= card_bytes) { memcpy(card + (size_t)ata.lba * 512, ata.buf, 512); card_dirty = 1; }
                ata_sectors_written++; ata.pos = 0; ata.lba++;
                if (--ata.remain <= 0) { ata.status = ST_DRDY | ST_DSC; ata.writing = 0; }
            }
        }
        break;
    case 0x04: break;
    case 0x08: ata.count = (uint8_t)v; break;
    case 0x0C: ata.lba0 = (uint8_t)v; break;
    case 0x10: ata.lba1 = (uint8_t)v; break;
    case 0x14: ata.lba2 = (uint8_t)v; break;
    case 0x18: ata.head = (uint8_t)v; break;
    case 0x1C: command((uint8_t)v); break;
    }
}

uint8_t ata_alt_status(void) { return card_present ? ata.status : 0; }
void ata_control_write(uint32_t val) {              // device control: bit 2 = soft reset
    if (card_present && (val & 4)) { ata.status = ST_DRDY | ST_DSC; ata.err = 1; ata.remain = 0; ata.writing = 0; }
}
