// SPDX-License-Identifier: GPL-3.0-only
//
// Dual-connect A2DP ownership policy (host-side, pure helpers).
// LibrePods: 0x06 OWNS, 0x20 autocon, 0x0E audio source, 0x2E connected
// devices. Anti-ping-pong: librepods-org#724 (i_paused_the_media — auto-resume
// after give-up must not reclaim). 0x10 smart-routing builders live in
// aacp_smart_routing.h (verbatim LibrePods AACPManager.kt).

#ifndef USBPODS_DUAL_CONNECT_POLICY_H
#define USBPODS_DUAL_CONNECT_POLICY_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

// Ownership fight state (logical; firmware may track a subset).
typedef enum {
    DUAL_USB_IDLE = 0,
    DUAL_WE_OWN_STREAMING,
    DUAL_THEY_OWN,
    DUAL_RECLAIMING,
    DUAL_GIVE_UP,
} dual_connect_state_t;

// LibrePods AACPManager.AudioSourceType
#define DUAL_AUDIO_SRC_NONE  0x00u
#define DUAL_AUDIO_SRC_CALL  0x01u
#define DUAL_AUDIO_SRC_MEDIA 0x02u

// Control-command id 0x06 OWNS / id 0x20 Connect Automatically
#define DUAL_OWNS_ID         0x06u
#define DUAL_AUTOCON_ID      0x20u
#define DUAL_OWNS_CLAIM_VAL  0x01u
#define DUAL_OWNS_GIVEUP_VAL 0x00u  // live: OwnsConnection: 00 while peer plays
#define DUAL_AUTOCON_ON      0x01u

#define DUAL_CTRL_FRAME_LEN  11u
#define DUAL_CONN_DEV_REC    8u
#define DUAL_CONN_DEV_MAX    8u

typedef struct {
    uint8_t mac[6]; // display order (packet [6..11] reversed)
    uint8_t type;   // DUAL_AUDIO_SRC_*
} dual_audio_source_t;

typedef struct {
    uint8_t mac[6]; // as in packet (LibrePods 0x2E does not reverse)
    uint8_t info1;
    uint8_t info2;
} dual_connected_device_t;

// --- Frame builders (exact bytes; control command 0x0009) ---
// claim:  04 00 04 00 09 00 06 01 00 00 00
// giveup: 04 00 04 00 09 00 06 00 00 00 00

static inline void dual_connect_build_owns_claim(uint8_t out[DUAL_CTRL_FRAME_LEN]) {
    static const uint8_t k[DUAL_CTRL_FRAME_LEN] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00
    };
    memcpy(out, k, DUAL_CTRL_FRAME_LEN);
}

static inline void dual_connect_build_owns_giveup(uint8_t out[DUAL_CTRL_FRAME_LEN]) {
    static const uint8_t k[DUAL_CTRL_FRAME_LEN] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(out, k, DUAL_CTRL_FRAME_LEN);
}

// Session takeOver companion: 0x20 autocon (LibrePods automaticConnectionEnabled).
static inline void dual_connect_build_autocon(uint8_t out[DUAL_CTRL_FRAME_LEN], uint8_t enable) {
    out[0] = 0x04; out[1] = 0x00; out[2] = 0x04; out[3] = 0x00;
    out[4] = 0x09; out[5] = 0x00;
    out[6] = DUAL_AUTOCON_ID;
    out[7] = enable ? DUAL_AUTOCON_ON : 0x02u;
    out[8] = 0x00; out[9] = 0x00; out[10] = 0x00;
}

// --- Parsers (LibrePods AACPManager verbatim layout) ---

// 0x0E audio source: opcode at [4], MAC [6..11] reversed, type[12].
static inline bool dual_connect_parse_0e(const uint8_t *pkt, size_t len,
                                         dual_audio_source_t *out) {
    if (!pkt || !out || len < 13u) return false;
    if (pkt[4] != 0x0Eu || pkt[5] != 0x00u) return false;
    // Packet stores MAC little-endian / reversed; restore display order.
    out->mac[0] = pkt[11];
    out->mac[1] = pkt[10];
    out->mac[2] = pkt[9];
    out->mac[3] = pkt[8];
    out->mac[4] = pkt[7];
    out->mac[5] = pkt[6];
    out->type = pkt[12];
    return (out->type == DUAL_AUDIO_SRC_NONE ||
            out->type == DUAL_AUDIO_SRC_CALL ||
            out->type == DUAL_AUDIO_SRC_MEDIA);
}

// 0x2E connected devices: opcode [4], count at [8], then 8-byte records
// (6 MAC + info1 + info2). LibrePods does not reverse 0x2E MACs.

static inline int dual_connect_parse_0x2e(const uint8_t *pkt, size_t len,
                                          dual_connected_device_t *out,
                                          int max_out) {
    if (!pkt || len < 9u) return -1;
    if (pkt[4] != 0x2Eu || pkt[5] != 0x00u) return -1;
    int count = (int)(uint8_t)pkt[8];
    if (count < 0) return -1;
    if (!out || max_out <= 0) return count;
    int n = 0;
    size_t offset = 9u;
    for (int i = 0; i < count && n < max_out; i++) {
        if (offset + DUAL_CONN_DEV_REC > len) break;
        memcpy(out[n].mac, pkt + offset, 6);
        out[n].info1 = pkt[offset + 6];
        out[n].info2 = pkt[offset + 7];
        offset += DUAL_CONN_DEV_REC;
        n++;
    }
    return n;
}

// --- Policy decisions ---

static inline bool dual_connect_usb_wants_sink(bool usb_spk_open, bool is_usb_streaming) {
    return usb_spk_open || is_usb_streaming;
}

// Unexpected AVDTP pause / START reject: fight only when USB wants the sink,
// and not while anti-ping-pong is armed (we paused after give-up).
static inline bool dual_connect_should_reclaim_on_steal(bool usb_spk_open,
                                                       bool is_usb_streaming,
                                                       bool we_paused_after_giveup) {
    if (we_paused_after_giveup) return false;
    return dual_connect_usb_wants_sink(usb_spk_open, is_usb_streaming);
}

// LibrePods #724: after give-up pause, auto-resume must not reclaim until
// explicit USB wants sink again (caller clears we_paused_after_giveup).
static inline bool dual_connect_anti_ping_pong_blocks_reclaim(bool we_paused_after_giveup) {
    return we_paused_after_giveup;
}

// Post-reclaim: allow Play / AVDTP START only if we paused for the steal
// (otherwise stay paused — do not yank media from a quiet Mac).
static inline bool dual_connect_allow_play_after_reclaim(bool we_paused_for_steal) {
    return we_paused_for_steal;
}

// Stop reclaim if OWNS stays 0x00 (earbuds keep peer; LibrePods #724 note).
static inline bool dual_connect_should_stop_reclaim(bool reclaiming, uint8_t owns) {
    return reclaiming && (owns == DUAL_OWNS_GIVEUP_VAL);
}

// 0x0E: peer MEDIA (or CALL) → they own the stream; we should give up.
static inline bool dual_connect_0e_means_they_own(const dual_audio_source_t *src,
                                                 const uint8_t our_mac[6]) {
    if (!src || !our_mac) return false;
    if (src->type != DUAL_AUDIO_SRC_MEDIA && src->type != DUAL_AUDIO_SRC_CALL) {
        return false;
    }
    return memcmp(src->mac, our_mac, 6) != 0;
}

// Idle: no fight.
static inline dual_connect_state_t dual_connect_state_on_unexpected_pause(
        bool usb_spk_open, bool is_usb_streaming, bool we_paused_after_giveup) {
    if (!dual_connect_should_reclaim_on_steal(usb_spk_open, is_usb_streaming,
                                              we_paused_after_giveup)) {
        return we_paused_after_giveup ? DUAL_GIVE_UP : DUAL_USB_IDLE;
    }
    return DUAL_RECLAIMING;
}

#endif // USBPODS_DUAL_CONNECT_POLICY_H
