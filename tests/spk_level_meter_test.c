// Host-side proof of USB speaker L/R RMS → integer dBFS and align-health helpers.
// gcc -I. -O2 -o /tmp/spk_level_meter_test tests/spk_level_meter_test.c && /tmp/spk_level_meter_test

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "src/tinyusb/spk_level_meter.h"
#include "src/tinyusb/spk_frame_align.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while (0)
#define EQ(a, b, msg) do { if ((a) != (b)) { \
    fprintf(stderr, "FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    exit(1); } } while (0)
#define NEAR(a, b, tol, msg) do { \
    int _d = (int)(a) - (int)(b); if (_d < 0) _d = -_d; \
    if (_d > (tol)) { \
        fprintf(stderr, "FAIL: %s (%d vs %d, tol %d)\n", msg, (int)(a), (int)(b), (int)(tol)); \
        exit(1); \
    } \
} while (0)

static void fill_const(int16_t *pcm, unsigned frames, int16_t l, int16_t r) {
    for (unsigned i = 0; i < frames; i++) {
        pcm[2 * i] = l;
        pcm[2 * i + 1] = r;
    }
}

int main(void) {
    EQ(spk_swap_suspect(0), 0, "rem 0 not swap");
    EQ(spk_swap_suspect(1), 0, "rem 1 not swap");
    EQ(spk_swap_suspect(2), 1, "rem 2 is one-channel sticky");
    EQ(spk_swap_suspect(3), 0, "rem 3 not swap");

    EQ(spk_dbfs_from_mag(0), SPK_DBFS_SILENCE, "silence mag");
    EQ(spk_dbfs_from_mag(32767), 0, "full-scale peak");
    NEAR(spk_dbfs_from_mag(16384), -6, 1, "half-scale ≈ -6 dBFS");
    NEAR(spk_dbfs_from_mag(23170), -3, 1, "FS sine RMS ≈ -3 dBFS");

    EQ(spk_dbfs_from_sumsq(0, 64), SPK_DBFS_SILENCE, "empty sum");

    spk_level_meter_t m;
    spk_level_meter_init(&m, 64);
    EQ(m.dbfs_l, SPK_DBFS_SILENCE, "init L silence");
    EQ(m.dbfs_r, SPK_DBFS_SILENCE, "init R silence");

    int16_t pcm[64 * 2];
    fill_const(pcm, 64, 32767, 0);
    spk_level_meter_ingest(&m, pcm, 64);
    NEAR(m.dbfs_l, 0, 1, "full-scale L RMS");
    EQ(m.dbfs_r, SPK_DBFS_SILENCE, "silent R");
    EQ(spk_levels_imbalance_warn(m.dbfs_l, m.dbfs_r), 0, "one-side silence is not |L−R| gap");

    fill_const(pcm, 64, 32767, 16384);
    spk_level_meter_ingest(&m, pcm, 64);
    NEAR(m.dbfs_l, 0, 1, "L still full-scale");
    NEAR(m.dbfs_r, -6, 1, "R half-scale");
    EQ(spk_levels_both_active(m.dbfs_l, m.dbfs_r, SPK_DBFS_ACTIVE), 1, "both active");
    EQ(spk_levels_gap_db(m.dbfs_l, m.dbfs_r) >= SPK_DBFS_GAP_WARN, 1, "gap ≥ 6 dB");
    EQ(spk_levels_imbalance_warn(m.dbfs_l, m.dbfs_r), 1, "imbalance warn");

    fill_const(pcm, 64, 20000, 20000);
    spk_level_meter_ingest(&m, pcm, 64);
    EQ(spk_levels_gap_db(m.dbfs_l, m.dbfs_r) <= 1, 1, "matched L/R");
    EQ(spk_levels_imbalance_warn(m.dbfs_l, m.dbfs_r), 0, "balanced no warn");

    // Quiet both: below active threshold, large numeric gap must not warn.
    EQ(spk_levels_imbalance_warn((int8_t)-90, (int8_t)-96), 0, "near-silence no warn");

    // Frame-align half-frame counter (194-byte packet = leftover 2).
    spk_frame_align_t s;
    memset(&s, 0, sizeof(s));
    spk_frame_align_reset(&s);
    uint8_t buf[256];
    memset(buf, 0x11, 194);
    uint16_t aligned = spk_frame_align_ingest(&s, buf, 194, sizeof(buf));
    EQ(aligned, 192, "194 → 192");
    EQ(s.rem_len, 2, "2-byte remainder");
    EQ(s.misalign, 1, "misalign++");
    EQ(s.half, 1, "half++ on leftover 2");
    EQ(spk_swap_suspect(s.rem_len), 1, "live rem_len 2 is swap-suspect");

    memset(buf, 0x22, 193);
    aligned = spk_frame_align_ingest(&s, buf, 193, sizeof(buf));
    EQ(s.half, 1, "leftover 1 does not bump half");
    EQ(s.misalign, 2, "misalign still counts leftover 1");
    (void)aligned;

    printf("spk_level_meter_test: PASS (integer dBFS, imbalance helpers, rem_len=2 half)\n");
    return 0;
}
