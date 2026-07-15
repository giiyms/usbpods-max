// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
//
// AAC-ELD decoder for the AirPods hi-res mic stream.
//
// Wraps fdk-aac's aacDecoder_* API: TT_MP4_RAW transport, mono, configured
// with the fixed 4-byte AudioSpecificConfig from librepods. Decoded PCM
// lands in a lock-free ring consumed by the USB mic endpoint (uac.c).
//

#ifndef AACP_MIC_DEC_H
#define AACP_MIC_DEC_H

#include <stdint.h>
#include <stdbool.h>

// Open + configure the decoder. MUST be called from plain thread context at
// boot (main()) — never from a BTstack/async callback; the bulk mallocs hang
// there (see aacp_mic_dec.c). Idempotent; the instance persists forever.
bool aacp_mic_dec_init(void);

// True if the decoder was successfully opened at boot.
bool aacp_mic_dec_ready(void);

// Decode one AAC-ELD access unit. Updates internal statistics.
// Returns true on a successfully decoded frame.
bool aacp_mic_dec_decode(const uint8_t *au, uint16_t len);

// Emit the once-per-second "[DEC]" statistics line and reset interval
// counters. Called from the mic stats timer in btstack_aacp.c.
void aacp_mic_dec_report(void);

// Reset cumulative statistics (called on mic START).
void aacp_mic_dec_reset_stats(void);

// --- Decoded-PCM ring buffer (mono 16-bit @ 64 kHz) ---
// Single producer (decoder, BTstack context) / single consumer (USB IN
// callback in the USB timer IRQ) — lock-free.

// Pull up to `n` samples into dst; returns the number actually read.
// The USB callback zero-fills any shortfall.
uint32_t aacp_mic_pcm_read(int16_t *dst, uint32_t n);

// Drop any buffered samples (called on mic START for a clean session).
void aacp_mic_pcm_reset(void);

#endif // AACP_MIC_DEC_H
