// Host-side proof of the TinyUSB speaker L/R sticky-remainder fix.
// gcc -I. -O2 -o /tmp/spk_frame_align_test tests/spk_frame_align_test.c && /tmp/spk_frame_align_test

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "src/tinyusb/spk_frame_align.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while (0)
#define EQ(a, b, msg) do { if ((a) != (b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

static void fill_stereo(int16_t *pcm, unsigned frames, unsigned start_idx) {
    for (unsigned i = 0; i < frames; i++) {
        pcm[2 * i]     = (int16_t)(1000 + (int)(start_idx + i)); // L
        pcm[2 * i + 1] = (int16_t)(-1000 - (int)(start_idx + i)); // R
    }
}

// Old bug: drop n%4 bytes each packet, interpret the rest as aligned frames.
static unsigned discard_ingest(const uint8_t *pkt, unsigned n,
                               int16_t *out, unsigned out_cap_frames) {
    unsigned aligned = n - (n % 4);
    unsigned frames = aligned / 4;
    if (frames > out_cap_frames) frames = out_cap_frames;
    memcpy(out, pkt, frames * 4);
    return frames;
}

int main(void) {
    spk_frame_align_t s;
    memset(&s, 0, sizeof(s));
    spk_frame_align_reset(&s);
    EQ(s.rem_len, 0, "reset rem_len");
    EQ(s.misalign, 0, "reset misalign");

    // Aligned 192-byte packet (48 frames) — no remainder.
    uint8_t buf[256];
    int16_t pcm[96];
    fill_stereo(pcm, 48, 0);
    memcpy(buf, pcm, 192);
    uint16_t aligned = spk_frame_align_ingest(&s, buf, 192, sizeof(buf));
    EQ(aligned, 192, "aligned packet");
    EQ(s.rem_len, 0, "no leftover");
    EQ(s.misalign, 0, "no misalign count");

    // 193-byte packet: 48 frames + 1 leftover byte.
    spk_frame_align_reset(&s);
    s.misalign = 0;
    fill_stereo(pcm, 48, 0);
    memcpy(buf, pcm, 192);
    buf[192] = 0xAB;
    aligned = spk_frame_align_ingest(&s, buf, 193, sizeof(buf));
    EQ(aligned, 192, "193 → 192 frames");
    EQ(s.rem_len, 1, "1-byte remainder");
    EQ(s.rem[0], 0xAB, "stashed leftover");
    EQ(s.misalign, 1, "misalign++");

    // Next 3 bytes complete that leftover into one frame, then 4 more = 2 frames.
    uint8_t chunk[3 + 4] = { 0x01, 0x02, 0x03, 0x11, 0x11, 0x22, 0x22 };
    memcpy(buf, chunk, sizeof(chunk));
    aligned = spk_frame_align_ingest(&s, buf, sizeof(chunk), sizeof(buf));
    EQ(aligned, 8, "1+3 leftover + 4 = 2 frames");
    EQ(s.rem_len, 0, "remainder consumed");
    EQ(buf[0], 0xAB, "prepended leftover is first byte");
    EQ(buf[1], 0x01, "then new bytes");

    // Persistent L/R: split a stereo stream on a 2-byte (one channel) boundary.
    // Generate 100 frames, feed as 194, 192, 192, 192 (194 = 48.5 frames).
    enum { TOTAL = 100 };
    int16_t src[TOTAL * 2];
    fill_stereo(src, TOTAL, 0);
    const uint8_t *raw = (const uint8_t *)src;
    unsigned raw_len = TOTAL * 4;
    unsigned off = 0;

    int16_t got_sticky[TOTAL * 2];
    unsigned got_n = 0;
    spk_frame_align_reset(&s);
    s.misalign = 0;

    int16_t got_discard[TOTAL * 2];
    unsigned disc_n = 0;

    unsigned pkt_sizes[] = { 194, 192, 192, 192, 192, 192 };
    unsigned pi = 0;
    while (off < raw_len) {
        unsigned want = pkt_sizes[pi % 6];
        if (want > raw_len - off) want = raw_len - off;
        uint8_t pkt[256];
        memcpy(pkt, raw + off, want);

        // Sticky (fix)
        uint8_t work[256];
        memcpy(work, pkt, want);
        uint16_t a = spk_frame_align_ingest(&s, work, (uint16_t)want, sizeof(work));
        memcpy((uint8_t *)got_sticky + got_n * 4, work, a);
        got_n += a / 4;

        // Discard (old bug)
        int16_t tmp[128];
        unsigned df = discard_ingest(pkt, want, tmp, 128);
        memcpy(got_discard + disc_n * 2, tmp, df * 4);
        disc_n += df;

        off += want;
        pi++;
    }
    EQ(got_n, TOTAL, "sticky recovered every stereo frame");
    for (unsigned i = 0; i < got_n; i++) {
        if (got_sticky[2 * i] != src[2 * i] || got_sticky[2 * i + 1] != src[2 * i + 1]) {
            fprintf(stderr, "FAIL: sticky L/R mismatch at frame %u got L=%d R=%d want L=%d R=%d\n",
                    i, got_sticky[2 * i], got_sticky[2 * i + 1], src[2 * i], src[2 * i + 1]);
            exit(1);
        }
    }

    // Old discard of the 2-byte leftover from a 194-byte packet shifts every
    // later frame by one channel.
    if (disc_n <= 48) FAIL("discard path produced too few frames");
    int differ = 0;
    for (unsigned i = 48; i < disc_n && i < TOTAL; i++) {
        if (got_discard[2 * i] != src[2 * i] ||
            got_discard[2 * i + 1] != src[2 * i + 1]) {
            differ = 1;
            break;
        }
    }
    if (!differ) FAIL("old discard path unexpectedly stayed aligned after 194-byte packet");

    // Reset remainder + counter on stream restart.
    uint32_t before = s.misalign;
    if (before == 0) FAIL("expected misalign from 194-byte packets");
    spk_frame_align_reset(&s);
    EQ(s.rem_len, 0, "reset leftover");
    // misalign is a CDC lifetime counter — firmware keeps it; test reset
    // only clears rem. Firmware reset-on-alt-set clears rem, not the counter.
    (void)before;

    printf("spk_frame_align_test: PASS (%u sticky frames match source, "
           "discard path diverges after mid-frame packet, misalign events=%u)\n",
           got_n, (unsigned)before);
    return 0;
}
