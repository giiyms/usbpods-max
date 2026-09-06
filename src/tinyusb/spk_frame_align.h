// SPDX-License-Identifier: MIT
//
// Sticky remainder for USB speaker PCM. 16-bit stereo is 4 bytes/frame.
// If a UAC ISO read is not a multiple of 4, dropping the leftover 1–3 bytes
// shifts the next packet by one channel and L/R stay swapped until the
// stream restarts. Carry the leftover and only push complete frames.

#ifndef USBPODS_SPK_FRAME_ALIGN_H
#define USBPODS_SPK_FRAME_ALIGN_H

#include <stdint.h>
#include <string.h>

#define SPK_STEREO_FRAME_BYTES 4

typedef struct {
    uint8_t  rem[SPK_STEREO_FRAME_BYTES];
    uint8_t  rem_len;     // 0..3
    uint32_t misalign;    // times a leftover was stashed (CDC counter)
} spk_frame_align_t;

static inline void spk_frame_align_reset(spk_frame_align_t *s) {
    if (!s) return;
    s->rem_len = 0;
    memset(s->rem, 0, sizeof(s->rem));
}

// `buf[0..n)` is a newly read USB chunk. Capacity `cap` must have room to
// prepend `rem_len` (at most 3). On return, `buf[0..aligned)` is whole
// stereo frames (4-byte aligned at buf[0]); leftover 1–3 bytes stay in `s`.
static inline uint16_t spk_frame_align_ingest(spk_frame_align_t *s,
                                              uint8_t *buf,
                                              uint16_t n,
                                              uint16_t cap) {
    if (!s || !buf) return 0;

    if (s->rem_len) {
        uint16_t need = s->rem_len;
        if (need >= cap) {
            spk_frame_align_reset(s);
            return 0;
        }
        if ((uint16_t)(n + need) > cap) {
            n = (uint16_t)(cap - need);
        }
        memmove(buf + need, buf, n);
        memcpy(buf, s->rem, need);
        n = (uint16_t)(n + need);
        s->rem_len = 0;
    }

    uint16_t leftover = (uint16_t)(n % SPK_STEREO_FRAME_BYTES);
    uint16_t aligned = (uint16_t)(n - leftover);
    if (leftover) {
        memcpy(s->rem, buf + aligned, leftover);
        s->rem_len = (uint8_t)leftover;
        s->misalign++;
    }
    return aligned;
}

#endif // USBPODS_SPK_FRAME_ALIGN_H
