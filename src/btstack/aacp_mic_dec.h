//
// Phase 3: AAC-ELD decoder for the AirPods hi-res mic stream.
//
// Wraps fdk-aac's aacDecoder_* API: TT_MP4_RAW transport, mono, configured
// with the fixed 4-byte AudioSpecificConfig from librepods (HANDOFF §3.5).
// Phase 3 decodes every AU and gathers statistics (error rate, PCM RMS/peak,
// decode time) — the PCM itself is consumed by the USB mic path in Phase 4.
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

#endif // AACP_MIC_DEC_H
