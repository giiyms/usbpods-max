// SPDX-License-Identifier: GPL-3.0-only
//
// Host USB session wake after dual-connect steal reclaim, and when USB
// speaker ISO OUT drops (1→0) while A2DP + OWNS stay up and the speaker
// alt-set is still open (Windows TinyUSB silent — live Quiet 2026-09-10).
//
// Windows can keep TinyUSB as the default device with the speaker alt-set
// still “open” while WASAPI has stopped ISO OUT (HID Pause / session desync).
// Switching the default device away and back recreates the pin. Firmware
// cannot SET_INTERFACE (host-owned). After A2DP START (reclaim) or on USB
// stream drop we:
//   1) HID Play (discrete 0xB0) if we paused for the steal — delayed so it
//      is not lost during AVDTP setup; one retry if PCM never returns
//   2) Clear UAC speaker Feature Unit mute and interrupt (in-tree
//      tud_audio_int_write pattern)
//   3) Local speaker path reset (same as alt-set) plus a volume interrupt
//      if alt stays open but USB PCM does not resume — not a HID spray,
//      not a fake SET_INTERFACE
//
// Pure helpers for host gcc tests. Firmware owns timers and HID/UAC I/O.

#ifndef USBPODS_HOST_SESSION_WAKE_H
#define USBPODS_HOST_SESSION_WAKE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Play after STREAM start, not at signaling-up (too early for Windows SMTC).
#define HOST_WAKE_PLAY_DELAY_MS  150
#define HOST_WAKE_RETRY_MS       400
#define HOST_WAKE_MAX_PLAYS      2

typedef enum {
    HOST_WAKE_IDLE = 0,
    HOST_WAKE_WAIT_PLAY,
    HOST_WAKE_WAIT_PCM,
} host_wake_phase_t;

typedef struct {
    host_wake_phase_t phase;
    uint8_t plays_sent;
    uint32_t due_ms;
} host_wake_t;

#define HOST_WAKE_ACT_NONE      0u
#define HOST_WAKE_ACT_UNMUTE    (1u << 0)
#define HOST_WAKE_ACT_PLAY      (1u << 1)
#define HOST_WAKE_ACT_ISO_NUDGE (1u << 2)

static inline void host_wake_reset(host_wake_t *w) {
    if (!w) return;
    memset(w, 0, sizeof(*w));
}

static inline void host_wake_cancel(host_wake_t *w) {
    host_wake_reset(w);
}

// Arm only after A2DP is streaming and we HID-paused (or marked pause) for
// the steal. Unmute immediately; Play waits HOST_WAKE_PLAY_DELAY_MS.
static inline bool host_wake_should_arm(bool we_paused_for_steal,
                                        bool a2dp_streaming) {
    return we_paused_for_steal && a2dp_streaming;
}

// USB ISO OUT went silent while A2DP still runs, we own the connection,
// and the speaker alt is still open — same #17 wake as post-reclaim.
// Does not require we_paused_for_steal (Windows session desync, not steal).
static inline bool host_wake_should_arm_on_usb_drop(bool usb_streaming_falling,
                                                    bool a2dp_streaming,
                                                    bool we_own,
                                                    bool usb_spk_open) {
    return usb_streaming_falling && a2dp_streaming && we_own && usb_spk_open;
}

static inline uint8_t host_wake_arm_if(host_wake_t *w, uint32_t now_ms,
                                       bool should) {
    if (!w || !should) return HOST_WAKE_ACT_NONE;
    if (w->phase != HOST_WAKE_IDLE) {
        return HOST_WAKE_ACT_NONE;
    }
    w->phase = HOST_WAKE_WAIT_PLAY;
    w->plays_sent = 0;
    w->due_ms = now_ms + (uint32_t) HOST_WAKE_PLAY_DELAY_MS;
    return HOST_WAKE_ACT_UNMUTE;
}

static inline uint8_t host_wake_arm(host_wake_t *w, uint32_t now_ms,
                                     bool we_paused_for_steal,
                                     bool a2dp_streaming) {
    return host_wake_arm_if(w, now_ms,
                            host_wake_should_arm(we_paused_for_steal,
                                                 a2dp_streaming));
}

static inline uint8_t host_wake_arm_usb_drop(host_wake_t *w, uint32_t now_ms,
                                             bool usb_streaming_falling,
                                             bool a2dp_streaming,
                                             bool we_own,
                                             bool usb_spk_open) {
    return host_wake_arm_if(
        w, now_ms,
        host_wake_should_arm_on_usb_drop(usb_streaming_falling, a2dp_streaming,
                                         we_own, usb_spk_open));
}

static inline bool host_wake_due(uint32_t now_ms, uint32_t due_ms) {
    return (int32_t)(now_ms - due_ms) >= 0;
}

// Next timer delay in ms (1 if overdue). 0 if idle.
static inline uint32_t host_wake_next_delay_ms(const host_wake_t *w,
                                                uint32_t now_ms) {
    if (!w || w->phase == HOST_WAKE_IDLE) return 0;
    if (host_wake_due(now_ms, w->due_ms)) return 1;
    return w->due_ms - now_ms;
}

static inline uint8_t host_wake_poll(host_wake_t *w, uint32_t now_ms,
                                     bool usb_spk_open, bool usb_streaming) {
    if (!w || w->phase == HOST_WAKE_IDLE) return HOST_WAKE_ACT_NONE;

    if (w->phase == HOST_WAKE_WAIT_PCM && usb_streaming) {
        host_wake_reset(w);
        return HOST_WAKE_ACT_NONE;
    }

    if (!host_wake_due(now_ms, w->due_ms)) return HOST_WAKE_ACT_NONE;

    if (w->phase == HOST_WAKE_WAIT_PLAY) {
        w->plays_sent = 1;
        w->phase = HOST_WAKE_WAIT_PCM;
        w->due_ms = now_ms + (uint32_t) HOST_WAKE_RETRY_MS;
        return HOST_WAKE_ACT_PLAY;
    }

    /* WAIT_PCM and still no USB PCM. */
    if (w->plays_sent >= HOST_WAKE_MAX_PLAYS) {
        host_wake_reset(w);
        return HOST_WAKE_ACT_NONE;
    }
    w->plays_sent++;
    w->due_ms = now_ms + (uint32_t) HOST_WAKE_RETRY_MS;
    uint8_t acts = HOST_WAKE_ACT_PLAY;
    /* Alt still open, host not sending: local reset + UAC volume interrupt. */
    if (usb_spk_open && !usb_streaming) {
        acts |= HOST_WAKE_ACT_ISO_NUDGE;
    }
    return acts;
}

#endif // USBPODS_HOST_SESSION_WAKE_H
