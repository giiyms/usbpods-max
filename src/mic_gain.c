// SPDX-License-Identifier: GPL-3.0-only
//
// Digital mic gain — see mic_gain.h.
//

#include "mic_gain.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "pico_w_led.h"

// Q8 linear multipliers: round(256 * 10^(dB/20)) for dB = 0..24.
static const uint16_t gain_q8[MIC_GAIN_DB_MAX + 1] = {
    256,  287,  323,  362,  406,  456,  512,  575,  645,  724,
    813,  912, 1024, 1149, 1290, 1448, 1626, 1825, 2048, 2299,
    2580, 2896, 3251, 3649, 4064
};

static volatile uint8_t gain_db = 0;
static volatile bool    muted   = false;
static volatile bool    persist_pending = false;
static volatile uint32_t persist_at_ms = 0;

#define PERSIST_DELAY_MS 750

void mic_gain_init(void) {
    uint8_t db = read_mic_gain_flash();
    if (db > MIC_GAIN_DB_MAX) db = 0;
    gain_db = db;
    muted = false;
    persist_pending = false;
    printf("[GAIN] init %u dB (persisted)\n", (unsigned) db);
}

void mic_gain_set_db(uint8_t db) {
    if (db > MIC_GAIN_DB_MAX) db = MIC_GAIN_DB_MAX;
    if (db == gain_db) return;
    gain_db = db;
    persist_pending = true;
    persist_at_ms = to_ms_since_boot(get_absolute_time()) + PERSIST_DELAY_MS;
}

uint8_t mic_gain_get_db(void) {
    return gain_db;
}

void mic_gain_set_mute(bool mute) {
    muted = mute;
}

bool mic_gain_get_mute(void) {
    return muted;
}

// Soft-clip: linear to a knee, then 1/4 slope, then hard clamp. Never wraps.
static inline int16_t soft_clip(int32_t y) {
    const int32_t MAX  = 32767;
    const int32_t KNEE = 24576;   // ~0.75 FS
    if (y > KNEE) {
        y = KNEE + ((y - KNEE) >> 2);
        if (y > MAX) y = MAX;
    } else if (y < -KNEE) {
        y = -KNEE + ((y + KNEE) >> 2);
        if (y < -MAX) y = -MAX;
    }
    return (int16_t) y;
}

void mic_gain_apply(int16_t *samples, uint32_t n) {
    if (muted) {
        memset(samples, 0, n * sizeof(int16_t));
        return;
    }
    uint8_t db = gain_db;
    if (db == 0) return;   // unity: decoder PCM is already 16-bit, no wrap risk

    uint16_t mul = gain_q8[db];
    for (uint32_t i = 0; i < n; i++) {
        int32_t y = ((int32_t) samples[i] * (int32_t) mul) >> 8;
        samples[i] = soft_clip(y);
    }
}

bool mic_gain_persist_pending(void) {
    return persist_pending;
}

void mic_gain_persist_ack(void) {
    persist_pending = false;
}

void mic_gain_persist_task(bool allow_flash) {
    if (!persist_pending) return;
    if (to_ms_since_boot(get_absolute_time()) < persist_at_ms) return;
    if (!allow_flash) return;   // keep pending until AACP/mic is idle
    persist_pending = false;
    uint8_t db = gain_db;
    if (write_mic_gain_flash(db)) {
        printf("[GAIN] persisted %u dB\n", (unsigned) db);
    } else {
        printf("[GAIN] persist FAILED (%u dB)\n", (unsigned) db);
    }
}
