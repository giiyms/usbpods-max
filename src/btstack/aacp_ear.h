// SPDX-License-Identifier: GPL-3.0-only
//
// AirPods Max / AACP 0x0006 ear-report → on-head vs off-head.
// Opcode and byte values are AAP Definitions / LibrePods only.
// 0x00 In Ear (on-head), 0x01 Out, 0x02 In Case, 0xFF unknown.

#ifndef USBPODS_AACP_EAR_H
#define USBPODS_AACP_EAR_H

#include <stdint.h>
#include <stdbool.h>

// Apple: "AirPods Max also pause audio if you lift one earphone off of
// your head." LibrePods linux default is PauseWhenOneRemoved
// (!primaryInEar || !secondaryInEar). Requiring BOTH cups out misses Max
// take-off, which often reports one cup first (live: L=1 R=0).
// Unknown 0xFF: do not treat as off-head (caller should ignore the packet).
static inline bool aacp_ear_is_off_head(uint8_t el, uint8_t er) {
    if (el == 0xFF || er == 0xFF) return false;
    return (el != 0x00) || (er != 0x00);
}

// Take-off: 200 ms either-cup-out → HID Pause (keep snappy).
// Resume: Max sensors bounce L=1 R=0 → L=1 R=1 → Pause → L=0 R=1 →
// L=0 R=0 (false on-head). 200 ms then fired HID Play and YouTube
// resumed. Require BOTH cups 0x00 continuously for 1.5 s after we
// paused before HID Play. Never Play/Pause toggle.
#define AACP_EAR_OFF_DEBOUNCE_MS   200
#define AACP_EAR_RESUME_STABLE_MS 1500

static inline uint32_t aacp_ear_commit_delay_ms(bool now_off, bool resume_pending) {
    if (!now_off && resume_pending) return AACP_EAR_RESUME_STABLE_MS;
    return AACP_EAR_OFF_DEBOUNCE_MS;
}

#endif // USBPODS_AACP_EAR_H
