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

#endif // USBPODS_AACP_EAR_H
