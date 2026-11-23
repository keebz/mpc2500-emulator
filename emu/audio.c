// Audio: the OS writes the SIOF transmit FIFO (0xA40000E0) with 4-word frames. For the stock OS a sample is a signed 20-bit value
// split as 7 + 13 bits across two words (word0 low 7 bits = top, word1 low 13 bits = bottom), and each value appears in two
// consecutive frames, giving one 44.1 kHz mono sample per two frames. audio_push() decodes that stream to 16-bit stereo for the
// live output and for WAV capture. (The channel/DAC mapping of the hardware is documented in NOTES.md.)
#include "board.h"
#include <stdlib.h>
#include <string.h>

static int32_t *cap_buf; static size_t cap_n, cap_cap; static int cap_on;
static uint32_t ring[1 << 16]; static uint32_t ring_w, ring_r; static int live_on;
static unsigned wcount, hi7, frame_ix; static int32_t left_v;

void audio_reset(void) { wcount = hi7 = frame_ix = 0; left_v = 0; ring_w = ring_r = 0; }

static void emit(uint32_t w) {
    if (live_on) { ring[ring_w & 0xFFFF] = w; __atomic_store_n(&ring_w, ring_w + 1, __ATOMIC_RELEASE); }
    if (!cap_on) return;
    if (cap_n == cap_cap) { cap_cap = cap_cap ? cap_cap * 2 : 1 << 20; cap_buf = realloc(cap_buf, cap_cap * 4); if (!cap_buf) { cap_cap = cap_n = 0; return; } }
    cap_buf[cap_n++] = (int32_t)w;
}

// Output scaling: the OS sample words are 20-bit; they are shifted right by audio_shift (default 3) to 16 bit. The level mapping to the real
// DAC full scale is not known (a normalised kit sample at velocity 100 peaks at ~0.35 of 16-bit full scale at shift 4 and clips at shift 2); use a larger shift if
// clipping is heard. --audio-shift N on the command line.
int audio_shift = 3;
static int clip16(int32_t v) { v >>= audio_shift; return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

// Words come in pairs (hi7, lo13) = one 20-bit sample; two consecutive pairs make one stereo frame: left, then right. (With a centred
// mono voice both are equal, which is all the captured test material has shown; a hard-panned voice would show the difference.)
void audio_push(uint32_t w) {
    unsigned k = wcount++ & 3;
    if (k == 0) hi7 = w & 0x7F;
    else if (k == 1) {
        int32_t v = (int32_t)((hi7 << 13) | (w & 0x1FFF)); if (v & 0x80000) v -= 0x100000;
        if ((frame_ix++ & 1) == 0) left_v = v;
        else emit(((uint32_t)(uint16_t)clip16(left_v) << 16) | (uint16_t)clip16(v));
    }
}

void audio_capture(int on) { cap_on = on; if (on) cap_n = 0; }
void audio_live_enable(int on) { live_on = on; }

uint32_t audio_live_pop(uint32_t *word) {
    uint32_t w = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE);
    if (ring_r == w) return 0;
    if (w - ring_r > 16384) ring_r = w - 4096;             // too far behind: skip ahead to keep the latency bounded
    *word = ring[ring_r & 0xFFFF]; ring_r++;
    return 1;
}

int audio_save_wav(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint32_t bytes = (uint32_t)cap_n * 4, rate = 44100;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    uint32_t riff = 36 + bytes, br = rate * 4;
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    for (size_t i = 0; i < cap_n; i++) {
        uint32_t u = (uint32_t)cap_buf[i]; int16_t l = (int16_t)(u >> 16), r = (int16_t)(u & 0xFFFF);
        fwrite(&l, 2, 1, f); fwrite(&r, 2, 1, f);
    }
    fclose(f);
    return 0;
}
