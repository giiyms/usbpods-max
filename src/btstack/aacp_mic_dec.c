// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// The stream parameters (AudioSpecificConfig, frame layout) come from
// librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AAC-ELD decoder for the AirPods hi-res mic stream — see header.
//
// Runs in the BTstack run loop (called per AU from the AACP L2CAP handler).
// One mono 480-sample ELD frame decodes in ~1.4ms at 250MHz (measured), well
// inside the 7.5ms frame budget; the decode-time stats below keep watch.
//
// LOGGING RULE: nothing is printed per AU. One "[DEC]" line per second.
//

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include <aacdecoder_lib.h>
#include "pico/time.h"

#include "aacp_mic_dec.h"

// Fixed AudioSpecificConfig for the mic stream (from librepods):
// AAC-ELD (AOT 39), mono, 480 samples/frame, nominal 64 kHz.
static UCHAR aacp_mic_asc[] = { 0xF8, 0xE6, 0x30, 0x00 };

// librepods ELD_INBUF_MAX — AUs above this are malformed, don't feed them.
#define MIC_AU_MAX 512

// Decoded PCM scratch. Expected mono 480; sized x2 in case GetStreamInfo
// reports stereo (then frameSize*channels must still fit).
#define MIC_PCM_MAX_SAMPLES 960
static INT_PCM mic_pcm[MIC_PCM_MAX_SAMPLES];

static HANDLE_AACDECODER mic_dec = NULL;
static bool mic_dec_streaminfo_logged = false;

// --- Decoded-PCM ring (mono 16-bit @ 64 kHz) ---
// 8192 samples = 16KB = 128ms of audio; absorbs the SDU batching jitter
// (AUs arrive in ~30ms bursts of 4). Lock-free SPSC: producer is the decoder
// (BTstack context), consumer is the USB IN callback (USB timer IRQ).
#define MIC_RING_SAMPLES 8192u              // power of two
#define MIC_RING_MASK    (MIC_RING_SAMPLES - 1u)
static int16_t           mic_ring[MIC_RING_SAMPLES];
static volatile uint32_t mic_ring_w = 0;    // producer index (monotonic)
static volatile uint32_t mic_ring_r = 0;    // consumer index (monotonic)
static uint32_t          mic_ring_overrun = 0;  // samples dropped, ring full

static void mic_pcm_write(const int16_t *src, uint32_t n) {
    uint32_t w = mic_ring_w, r = mic_ring_r;
    uint32_t space = MIC_RING_SAMPLES - (w - r);
    if (n > space) {                        // full: drop the oldest new data
        mic_ring_overrun += n - space;
        n = space;
    }
    for (uint32_t i = 0; i < n; i++) {
        mic_ring[(w + i) & MIC_RING_MASK] = src[i];
    }
    __asm volatile ("" ::: "memory");       // data before index
    mic_ring_w = w + n;
}

uint32_t aacp_mic_pcm_read(int16_t *dst, uint32_t n) {
    uint32_t w = mic_ring_w, r = mic_ring_r;
    uint32_t avail = w - r;
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = mic_ring[(r + i) & MIC_RING_MASK];
    }
    __asm volatile ("" ::: "memory");
    mic_ring_r = r + n;
    return n;
}

void aacp_mic_pcm_reset(void) {
    mic_ring_r = mic_ring_w;
    mic_ring_overrun = 0;
}

static struct {
    // interval (reset each report)
    uint32_t ok_interval;
    uint32_t err_interval;
    uint32_t dec_us_sum;
    uint32_t dec_us_max;
    uint64_t sq_sum;          // sum of sample^2 across the interval
    uint32_t sq_samples;      // samples contributing to sq_sum
    int32_t  peak;            // max |sample| this interval
    // cumulative (reset on mic START)
    uint32_t ok_total;
    uint32_t err_total;
} dstat;

// NOTE: must be called from plain thread context (main(), at boot) — NOT from
// a BTstack/async-context callback. aacDecoder_Open mallocs ~258KB in many
// chunks; doing that from the async worker context hung the system on
// hardware (same family as upstream's "FDK-AAC hangs on Core 1" note).
// Heap occupancy is logged at open so an out-of-memory failure is visible:
// arena = heap claimed from the system, uordblks = in use, fordblks = free;
// region = the total heap area from the linker (end .. __StackLimit).
extern char __StackLimit[];
extern char end[];   // linker: end of .bss = start of heap

static void dec_report_heap(const char *tag) {
    struct mallinfo mi = mallinfo();
    printf("[DEC] heap %s: region=%u arena=%u inuse=%u free=%u\n",
           tag, (unsigned)(__StackLimit - end),
           (unsigned) mi.arena, (unsigned) mi.uordblks, (unsigned) mi.fordblks);
}

bool aacp_mic_dec_init(void) {
    if (mic_dec != NULL) return true;

    dec_report_heap("before");
    printf("[DEC] aacDecoder_Open ...\n");
    mic_dec = aacDecoder_Open(TT_MP4_RAW, /* nrOfLayers */ 1);
    if (mic_dec == NULL) {
        printf("[DEC] aacDecoder_Open FAILED (out of heap?)\n");
        return false;
    }

    printf("[DEC] aacDecoder_ConfigRaw ...\n");
    UCHAR *conf[] = { aacp_mic_asc };
    UINT   clen   = sizeof(aacp_mic_asc);
    AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(mic_dec, conf, &clen);
    if (err != AAC_DEC_OK) {
        printf("[DEC] aacDecoder_ConfigRaw FAILED: 0x%x\n", err);
        aacDecoder_Close(mic_dec);
        mic_dec = NULL;
        return false;
    }

    dec_report_heap("after");
    printf("[DEC] decoder ready (TT_MP4_RAW, ASC F8 E6 30 00)\n");
    return true;
}

bool aacp_mic_dec_ready(void) {
    return mic_dec != NULL;
}

void aacp_mic_dec_reset_stats(void) {
    memset(&dstat, 0, sizeof(dstat));
    mic_dec_streaminfo_logged = false;
}

bool aacp_mic_dec_decode(const uint8_t *au, uint16_t len) {
    if (mic_dec == NULL || len == 0 || len > MIC_AU_MAX) {
        dstat.err_interval++;
        dstat.err_total++;
        return false;
    }

    UCHAR *inb[]  = { (UCHAR *) au };
    UINT   insz   = len;
    UINT   valid  = len;
    aacDecoder_Fill(mic_dec, inb, &insz, &valid);

    uint32_t t0 = time_us_32();
    AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(mic_dec, mic_pcm, MIC_PCM_MAX_SAMPLES, 0);
    uint32_t dt = time_us_32() - t0;

    if (err != AAC_DEC_OK) {
        // Keep PCM continuity for the USB stream: substitute one frame of
        // silence for the failed AU.
        static const int16_t silence[480] = { 0 };
        mic_pcm_write(silence, 480);
        dstat.err_interval++;
        dstat.err_total++;
        return false;
    }

    dstat.ok_interval++;
    dstat.ok_total++;
    dstat.dec_us_sum += dt;
    if (dt > dstat.dec_us_max) dstat.dec_us_max = dt;

    CStreamInfo *si = aacDecoder_GetStreamInfo(mic_dec);

    // Log the decoder's reported stream parameters once per session.
    if (!mic_dec_streaminfo_logged && si != NULL) {
        mic_dec_streaminfo_logged = true;
        printf("[DEC] *** stream info: sampleRate=%d numChannels=%d frameSize=%d aot=%d ***\n",
               (int) si->sampleRate, (int) si->numChannels, (int) si->frameSize, (int) si->aot);
    }

    // Feed the USB mic path (mono: frameSize samples as-is at 64 kHz).
    int samples = (si != NULL) ? (int)(si->frameSize * si->numChannels) : 480;
    if (samples > MIC_PCM_MAX_SAMPLES) samples = MIC_PCM_MAX_SAMPLES;
    mic_pcm_write(mic_pcm, (uint32_t) samples);

    // PCM level stats over the decoded frame.
    for (int i = 0; i < samples; i++) {
        int32_t s = mic_pcm[i];
        dstat.sq_sum += (uint64_t)((int64_t) s * s);
        int32_t a = s < 0 ? -s : s;
        if (a > dstat.peak) dstat.peak = a;
    }
    dstat.sq_samples += (uint32_t) samples;

    return true;
}

void aacp_mic_dec_report(void) {
    uint32_t ok = dstat.ok_interval;
    uint32_t avg_us = ok ? dstat.dec_us_sum / ok : 0;
    // RMS from the mean square; M33 has an FPU, sqrtf is cheap here (1 Hz).
    uint32_t rms = 0;
    if (dstat.sq_samples > 0) {
        rms = (uint32_t) sqrtf((float)(dstat.sq_sum / dstat.sq_samples));
    }
    printf("[DEC] ok/s=%lu err=%lu(total %lu) dec_us avg/max=%lu/%lu rms=%lu peak=%ld ring=%lu%s\n",
           (unsigned long) ok,
           (unsigned long) dstat.err_interval,
           (unsigned long) dstat.err_total,
           (unsigned long) avg_us,
           (unsigned long) dstat.dec_us_max,
           (unsigned long) rms,
           (long) dstat.peak,
           (unsigned long) (mic_ring_w - mic_ring_r),
           mic_ring_overrun ? " OVERRUN" : "");

    dstat.ok_interval  = 0;
    dstat.err_interval = 0;
    dstat.dec_us_sum   = 0;
    dstat.dec_us_max   = 0;
    dstat.sq_sum       = 0;
    dstat.sq_samples   = 0;
    dstat.peak         = 0;
}
