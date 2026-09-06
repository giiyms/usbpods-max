// SPDX-License-Identifier: GPL-3.0-only
//
// AirPods Max / AACP 0x0006 ear-report → on-head vs off-head + HID edges.
// Opcode and byte values are AAP Definitions / LibrePods only.
// 0x00 In Ear (on-head), 0x01 Out, 0x02 In Case, 0xFF unknown.
//
// Host-testable: polarity, debounce delays, and Pause/Play decisions live
// here so tests can replay the live Max bounce without Pico/BTstack.

#ifndef USBPODS_AACP_EAR_H
#define USBPODS_AACP_EAR_H

#include <stdint.h>
#include <stdbool.h>

// Fast Pause (either cup out). Live take-off is L=1 R=0 then L=1 R=1.
#define AACP_EAR_PAUSE_DEBOUNCE_MS  200
// After HID Pause, Max cups flap L=0 R=0 (false on-head / table bounce).
// Require this long of *continuous* both-cups-on-head before HID Play.
// Live 2026-09-05: Pause then Play ~200 ms later undid YouTube. 2 s hold
// matches "take off for >2 s must stay paused until put back on".
#define AACP_EAR_RESUME_HOLD_MS     2000

// Apple: "AirPods Max also pause audio if you lift one earphone off of
// your head." LibrePods linux default is PauseWhenOneRemoved
// (!primaryInEar || !secondaryInEar). Requiring BOTH cups out misses Max
// take-off, which often reports one cup first (live: L=1 R=0).
// Unknown 0xFF: do not treat as off-head (caller should ignore the packet).
static inline bool aacp_ear_is_off_head(uint8_t el, uint8_t er) {
    if (el == 0xFF || er == 0xFF) return false;
    return (el != 0x00) || (er != 0x00);
}

// Off-head → Pause after 200 ms. On-head → Play only after resume hold
// (and only if we paused). Never invert polarity; never Play/Pause toggle.
static inline uint32_t aacp_ear_delay_ms(bool going_off, bool resume_pending) {
    if (!going_off && resume_pending) return AACP_EAR_RESUME_HOLD_MS;
    return AACP_EAR_PAUSE_DEBOUNCE_MS;
}

typedef struct {
    bool known;
    bool off_head;
    bool pending;
    bool pending_off;
    bool resume_pending;
} aacp_ear_sm_t;

static inline void aacp_ear_sm_reset(aacp_ear_sm_t *s) {
    s->known = false;
    s->off_head = false;
    s->pending = false;
    s->pending_off = false;
    s->resume_pending = false;
}

// Arm / re-arm debounce. 0 = no timer change (unknown, duplicate, or settled).
static inline uint32_t aacp_ear_consider_delay(aacp_ear_sm_t *s, uint8_t el, uint8_t er) {
    if (el == 0xFF || er == 0xFF) return 0;
    bool off = aacp_ear_is_off_head(el, er);
    if (s->pending && s->pending_off == off) return 0;
    if (s->known && !s->pending && s->off_head == off) return 0;
    s->pending = true;
    s->pending_off = off;
    return aacp_ear_delay_ms(off, s->resume_pending);
}

typedef enum {
    AACP_EAR_ACT_NONE = 0,
    AACP_EAR_ACT_PAUSE,
    AACP_EAR_ACT_PLAY,
    AACP_EAR_ACT_SKIP_FIRST,
    AACP_EAR_ACT_SKIP_NOCHANGE,
    AACP_EAR_ACT_SKIP_DISABLED,
    AACP_EAR_ACT_SKIP_NOT_STREAMING,
    AACP_EAR_ACT_SKIP_STAY_PAUSED
} aacp_ear_act_t;

// Apply a settled debounce. Discrete Play (0xB0) / Pause (0xB1) only.
static inline aacp_ear_act_t aacp_ear_fire(aacp_ear_sm_t *s, bool ear_detect_off,
                                           bool streaming) {
    if (!s->pending) return AACP_EAR_ACT_NONE;
    s->pending = false;
    bool now_off = s->pending_off;
    if (!s->known) {
        s->known = true;
        s->off_head = now_off;
        return AACP_EAR_ACT_SKIP_FIRST;
    }
    if (now_off == s->off_head) return AACP_EAR_ACT_SKIP_NOCHANGE;
    s->off_head = now_off;
    if (ear_detect_off) return AACP_EAR_ACT_SKIP_DISABLED;
    if (now_off) {
        if (streaming) {
            s->resume_pending = true;
            return AACP_EAR_ACT_PAUSE;
        }
        return AACP_EAR_ACT_SKIP_NOT_STREAMING;
    }
    if (s->resume_pending) {
        s->resume_pending = false;
        return AACP_EAR_ACT_PLAY;
    }
    return AACP_EAR_ACT_SKIP_STAY_PAUSED;
}

#endif // USBPODS_AACP_EAR_H
