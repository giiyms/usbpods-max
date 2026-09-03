// SPDX-License-Identifier: GPL-3.0-only
//
// Digital mic gain applied to decoded PCM *after* AAC-ELD decode, before the
// USB mic endpoint. This is software gain on the Pico — not HFP, and not an
// Apple AACP opcode (librepods has no documented mic-gain opcode; see
// AACP-FEATURES.md).
//

#ifndef MIC_GAIN_H
#define MIC_GAIN_H

#include <stdint.h>
#include <stdbool.h>

#define MIC_GAIN_DB_MIN  0
#define MIC_GAIN_DB_MAX  24

// Load persisted gain from flash (default 0 dB if unset). Call once at boot.
void mic_gain_init(void);

// RAM-only; IRQ-safe. Clamps to 0..24 dB. Marks flash persist for the main loop.
void mic_gain_set_db(uint8_t db);
uint8_t mic_gain_get_db(void);

// UAC mute (Windows recording slider). IRQ-safe.
void mic_gain_set_mute(bool mute);
bool mic_gain_get_mute(void);

// Apply boost + soft-clip/limiter in place. No-op at 0 dB with unmute.
// Call on the USB mic PCM *after* aacp_mic_pcm_read(), never inside the decoder.
void mic_gain_apply(int16_t *samples, uint32_t n);

// If a set_db requested persist, write flash once the change has settled.
// allow_flash=false keeps the pending flag (do not erase+program mid-AACP).
// Call from the main loop only (flash_safe_execute).
void mic_gain_persist_task(bool allow_flash);
bool mic_gain_persist_pending(void);
void mic_gain_persist_ack(void);

#endif // MIC_GAIN_H
