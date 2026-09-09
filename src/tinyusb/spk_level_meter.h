// SPDX-License-Identifier: MIT
//
// Rolling L/R level meter for USB speaker 16-bit interleaved stereo PCM.
// Runs after spk_frame_align_ingest, before A2DP encode.
//
// @STATUS keys (integer dBFS, 0 = full-scale, -96 = silence):
//   spk_l=  spk_r=     last completed window RMS
// Cross-correlation lag is NOT implemented (deferred — Pico CPU).

#ifndef USBPODS_SPK_LEVEL_METER_H
#define USBPODS_SPK_LEVEL_METER_H

#include <stdint.h>

#define SPK_DBFS_SILENCE       (-96)
#define SPK_DBFS_ACTIVE        (-50)  // both cups "active" for |L−R| gap warn
#define SPK_DBFS_GAP_WARN      6      // |L−R| dB while both active
#define SPK_LEVEL_WINDOW_N     4800   // 100 ms at 48 kHz stereo frames

// One-channel leftover (2 bytes) is the classic L/R sticky-swap remainder.
static inline int spk_swap_suspect(uint8_t rem_len) {
    return rem_len == 2;
}

static inline int spk_levels_gap_db(int8_t l, int8_t r) {
    int d = (int)l - (int)r;
    return d < 0 ? -d : d;
}

static inline int spk_levels_both_active(int8_t l, int8_t r, int8_t thresh) {
    return l >= thresh && r >= thresh;
}

// |L−R| large while both channels are carrying audio (not one-side silence).
static inline int spk_levels_imbalance_warn(int8_t l, int8_t r) {
    return spk_levels_both_active(l, r, SPK_DBFS_ACTIVE) &&
           spk_levels_gap_db(l, r) >= SPK_DBFS_GAP_WARN;
}

// Integer sqrt for RMS. Host-testable; no libm.
static inline uint32_t spk_isqrt64(uint64_t x) {
    if (x == 0) return 0;
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = (uint64_t)1 << 62;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (uint32_t)res;
}

// 20*log10(mag/32768) as integer dBFS. Linear-plus-quadratic log2; ±1 dB.
static inline int8_t spk_dbfs_from_mag(uint32_t mag) {
    if (mag == 0) return (int8_t)SPK_DBFS_SILENCE;
    if (mag >= 32767u) return 0;

    int e = 31 - __builtin_clz(mag);
    uint32_t m_q16;
    if (e >= 16) m_q16 = mag >> (e - 16);
    else         m_q16 = mag << (16 - e);

    uint32_t f = m_q16 - 0x10000u;
    uint32_t omf = 0x10000u - f;
    uint32_t corr = (uint32_t)(((uint64_t)f * omf * 343u) / (1000u * 65536u));
    uint32_t log2_frac = f + corr;
    int32_t log2_q16 = ((int32_t)e << 16) + (int32_t)log2_frac;
    int32_t rel_q16 = log2_q16 - (15 << 16);
    int32_t dbfs_q16 = (rel_q16 * 6165) >> 10;
    int dbfs = (dbfs_q16 + (1 << 15)) >> 16;
    if (dbfs > 0) dbfs = 0;
    if (dbfs < SPK_DBFS_SILENCE) dbfs = SPK_DBFS_SILENCE;
    return (int8_t)dbfs;
}

static inline int8_t spk_dbfs_from_sumsq(uint64_t sum_sq, uint32_t n) {
    if (n == 0 || sum_sq == 0) return (int8_t)SPK_DBFS_SILENCE;
    uint64_t mean = sum_sq / n;
    return spk_dbfs_from_mag(spk_isqrt64(mean));
}

typedef struct {
    uint64_t sq_l;
    uint64_t sq_r;
    uint32_t n;
    uint32_t window_n;
    int8_t   dbfs_l;
    int8_t   dbfs_r;
} spk_level_meter_t;

static inline void spk_level_meter_reset(spk_level_meter_t *m) {
    if (!m) return;
    m->sq_l = 0;
    m->sq_r = 0;
    m->n = 0;
    m->dbfs_l = (int8_t)SPK_DBFS_SILENCE;
    m->dbfs_r = (int8_t)SPK_DBFS_SILENCE;
}

static inline void spk_level_meter_init(spk_level_meter_t *m, uint32_t window_n) {
    if (!m) return;
    m->window_n = window_n ? window_n : SPK_LEVEL_WINDOW_N;
    spk_level_meter_reset(m);
}

static inline void spk_level_meter_ingest(spk_level_meter_t *m,
                                          const int16_t *stereo,
                                          uint16_t frames) {
    if (!m || !stereo || !frames) return;
    uint32_t win = m->window_n ? m->window_n : SPK_LEVEL_WINDOW_N;
    for (uint16_t i = 0; i < frames; i++) {
        int32_t l = stereo[2 * i];
        int32_t r = stereo[2 * i + 1];
        m->sq_l += (uint64_t)((int64_t)l * (int64_t)l);
        m->sq_r += (uint64_t)((int64_t)r * (int64_t)r);
        m->n++;
        if (m->n >= win) {
            m->dbfs_l = spk_dbfs_from_sumsq(m->sq_l, m->n);
            m->dbfs_r = spk_dbfs_from_sumsq(m->sq_r, m->n);
            m->sq_l = 0;
            m->sq_r = 0;
            m->n = 0;
        }
    }
}

#endif // USBPODS_SPK_LEVEL_METER_H
