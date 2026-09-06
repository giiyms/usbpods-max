// SPDX-License-Identifier: GPL-3.0-only
//
// A2DP reclaim after iPhone (or any host) steals the sink.
// Host-testable delays / busy-status handling. No AACP opcodes here.

#ifndef USBPODS_AVDTP_RECLAIM_H
#define USBPODS_AVDTP_RECLAIM_H

#include <stdint.h>
#include <stdbool.h>

// Clean signaling drop (we still want the headset).
#define AVDTP_RECLAIM_DELAY_MS        400
// After remote SUSPEND / START reject / stream release: give the other
// host a moment, then take the sink back. 400 ms was too soon — live
// Mac 2026-09-05: reclaim try → status 129 then a stuck reconnect.
#define AVDTP_RECLAIM_STEAL_DELAY_MS  1500
#define AVDTP_RECLAIM_RETRY_MS        2000
#define AVDTP_RECLAIM_BUSY_RETRY_MS   4000
#define AVDTP_RECLAIM_MAX_TRIES       40

// Live log: "AVDTP source signaling connection failed: status 129".
// 0x81 = controller/BTstack "not now". HCI COMMAND_DISALLOWED is 0x0C.
#define AVDTP_RECLAIM_STATUS_BUSY     129
#define AVDTP_RECLAIM_STATUS_DISALLOWED 0x0C

static inline bool avdtp_reclaim_status_busy(uint8_t status) {
    return status == AVDTP_RECLAIM_STATUS_BUSY ||
           status == AVDTP_RECLAIM_STATUS_DISALLOWED;
}

// tries_done: 0 = first arm (no connect yet). After each connect/open, pass
// the attempt count and last status so busy (129) backs off longer.
static inline uint32_t avdtp_reclaim_next_delay_ms(uint8_t tries_done, bool steal,
                                                   uint8_t last_status) {
    if (tries_done == 0) {
        return steal ? AVDTP_RECLAIM_STEAL_DELAY_MS : AVDTP_RECLAIM_DELAY_MS;
    }
    if (avdtp_reclaim_status_busy(last_status)) return AVDTP_RECLAIM_BUSY_RETRY_MS;
    return steal ? AVDTP_RECLAIM_STEAL_DELAY_MS : AVDTP_RECLAIM_RETRY_MS;
}

// Every other busy failure: drop a stale AVDTP object so the next connect
// is not COMMAND_DISALLOWED forever (needs BOOTSEL otherwise).
static inline bool avdtp_reclaim_should_drop_stale(uint8_t last_status,
                                                   uint8_t tries_done) {
    return avdtp_reclaim_status_busy(last_status) &&
           tries_done >= 2 && ((tries_done % 2u) == 0);
}

// Remote SUSPEND is a steal/handoff — do not auto-START (that was rejected
// live, then streaming released). Local send-fail recovery still restarts.
static inline bool avdtp_should_autostart_after_suspend(bool local_recovery) {
    return local_recovery;
}

#endif // USBPODS_AVDTP_RECLAIM_H
