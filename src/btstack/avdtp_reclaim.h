// SPDX-License-Identifier: GPL-3.0-only
//
// A2DP reclaim timing after signaling drop / Apple dual-connect steal.
// Live 2026-09-05: iPhone (AACP 0x2E peer) stole A2DP, AVDTP START rejected,
// then "signaling connection failed: status 129" (0x81). 400 ms / 2 s
// retries stacked while the phone still held the sink.

#ifndef USBPODS_AVDTP_RECLAIM_H
#define USBPODS_AVDTP_RECLAIM_H

#include <stdint.h>
#include <stdbool.h>

#define AVDTP_RECLAIM_DELAY_MS        400
#define AVDTP_RECLAIM_STEAL_DELAY_MS 2000
#define AVDTP_RECLAIM_RETRY_MS       2000
#define AVDTP_RECLAIM_STATUS129_MS   5000
#define AVDTP_RECLAIM_MAX_TRIES        40
#define AVDTP_RECLAIM_PENDING_STUCK_MS 6000
// Live steal: AVDTP source signaling connection failed: status 129.
// 0x81 = insufficient resources / peer still owns the A2DP sink.
#define AVDTP_RECLAIM_STATUS_RESOURCE 129u

static inline uint32_t avdtp_reclaim_arm_delay_ms(bool steal) {
    return steal ? AVDTP_RECLAIM_STEAL_DELAY_MS : AVDTP_RECLAIM_DELAY_MS;
}

static inline uint32_t avdtp_reclaim_backoff_ms(uint8_t last_status, uint8_t tries) {
    if (last_status == (uint8_t) AVDTP_RECLAIM_STATUS_RESOURCE) {
        uint32_t extra = (uint32_t) tries * 500u;
        uint32_t d = AVDTP_RECLAIM_STATUS129_MS + extra;
        if (d > 10000u) d = 10000u;
        return d;
    }
    (void) tries;
    return AVDTP_RECLAIM_RETRY_MS;
}

#endif // USBPODS_AVDTP_RECLAIM_H
