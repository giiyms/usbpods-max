// SPDX-License-Identifier: GPL-3.0-only
//
// Dual-connect A2DP ownership policy (host-side, pure helpers).
// LibrePods: 0x06 OWNS, 0x20 autocon, 0x0E audio source, 0x2E connected
// devices, 0x11 SMART_ROUTING_RESP (SetOwnershipToFalse). Anti-ping-pong:
// librepods-org#724 (i_paused_the_media — auto-resume after give-up must not
// reclaim). 0x10 smart-routing builders live in aacp_smart_routing.h
// (verbatim LibrePods AACPManager.kt). Soft exclusive (default on):
// dual_softexcl_* drops extra ACL while USB wants the sink; reclaim / 0x10
// fight path is the fallback when softexcl is off. Do not invent opcodes.

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

// LibrePods AACPManager.Opcodes.SMART_ROUTING_RESP — Host RX of 0x10 relay.
#define DUAL_SR_RESP_OPCODE          0x11u
// Substring search matches AACPManager.kt packet.decodeToString().contains(...)
#define DUAL_0X11_OWN_FALSE_KEY      "SetOwnershipToFalse"
#define DUAL_0X11_REVERSE_BANNER_KEY "ReverseBannerTapped"

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

// --- 0x11 SMART_ROUTING_RESP (LibrePods AACPManager.kt) ---
// sender MAC = packet[6..11] reversed (same as 0x10 TX / 0x0E).
// SetOwnershipToFalse / ReverseBannerTapped: raw substring on the whole packet.

static inline bool dual_connect_pkt_contains(const uint8_t *pkt, size_t len,
                                             const char *needle) {
    if (!pkt || !needle) return false;
    size_t nlen = strlen(needle);
    if (nlen == 0u || len < nlen) return false;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(pkt + i, needle, nlen) == 0) return true;
    }
    return false;
}

static inline bool dual_connect_parse_0x11_sender(const uint8_t *pkt, size_t len,
                                                  uint8_t mac[6]) {
    if (!pkt || !mac || len < 12u) return false;
    if (pkt[4] != DUAL_SR_RESP_OPCODE || pkt[5] != 0x00u) return false;
    mac[0] = pkt[11];
    mac[1] = pkt[10];
    mac[2] = pkt[9];
    mac[3] = pkt[8];
    mac[4] = pkt[7];
    mac[5] = pkt[6];
    return true;
}

static inline bool dual_connect_0x11_is_ownership_false(const uint8_t *pkt,
                                                        size_t len) {
    if (!pkt || len < 12u) return false;
    if (pkt[4] != DUAL_SR_RESP_OPCODE || pkt[5] != 0x00u) return false;
    return dual_connect_pkt_contains(pkt, len, DUAL_0X11_OWN_FALSE_KEY);
}

static inline bool dual_connect_0x11_is_reverse_banner(const uint8_t *pkt,
                                                       size_t len) {
    if (!dual_connect_0x11_is_ownership_false(pkt, len)) return false;
    return dual_connect_pkt_contains(pkt, len, DUAL_0X11_REVERSE_BANNER_KEY);
}

// AirPodsService.onOwnershipToFalseRequest: pause media, OWNS=00, disconnect
// audio; do not hijack back. linux-rust i_paused_the_media / librepods#724.
typedef struct {
    bool recognized;          // opcode 0x11 + SetOwnershipToFalse
    bool reverse_banner;      // ReverseBannerTapped (no USBPods reverse UI)
    bool pause_media;         // HID Pause when USB wants the sink
    bool set_we_paused;       // anti-ping-pong (we_paused_after_giveup)
    bool send_owns_giveup;    // control 0x06 = 00 (LibrePods)
    bool send_hijack;         // always false — no immediate 0x10 / OWNS claim
    dual_connect_state_t state;
} dual_connect_0x11_decision_t;

static inline dual_connect_0x11_decision_t dual_connect_decide_0x11(
        const uint8_t *pkt, size_t len,
        bool usb_spk_open, bool is_usb_streaming) {
    dual_connect_0x11_decision_t d;
    memset(&d, 0, sizeof(d));
    d.state = DUAL_USB_IDLE;
    d.send_hijack = false;
    if (!dual_connect_0x11_is_ownership_false(pkt, len)) {
        return d;
    }
    d.recognized = true;
    d.reverse_banner = dual_connect_0x11_is_reverse_banner(pkt, len);
    d.pause_media = usb_spk_open || is_usb_streaming;
    d.set_we_paused = true;
    d.send_owns_giveup = true;
    d.send_hijack = false;
    d.state = DUAL_THEY_OWN;
    return d;
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
// Timing/retries live in host_session_wake.h (HID Play after STREAM start).
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

// After 0x11 / give-up: keep we_paused_after_giveup until USB wants the sink
// again — USB PCM streaming 0→1 (host Play after HID Pause) or speaker alt
// 0→1. A still-open idle alt-set does not clear (librepods#724 auto-resume).
static inline bool dual_connect_should_clear_anti_ping_pong(
        bool we_paused_after_giveup,
        bool usb_streaming_rising,
        bool usb_spk_open_rising) {
    if (!we_paused_after_giveup) return false;
    return usb_streaming_rising || usb_spk_open_rising;
}

// --- Soft exclusive (keep iPhone paired; drop extra ACL while USB wants sink) ---
// Default ON. Fight path (AVDTP reclaim / LibrePods 0x10 hijack) is the
// fallback when softexcl is off. Never Forget / wipe keys / HCI-drop the Max.
//
// Phase: IDLE → HOLD on USB wants sink; HOLD → GRACE on USB idle; GRACE →
// IDLE after DUAL_SOFTEXCL_GRACE_MS if still idle. USB wants during GRACE
// returns to HOLD immediately. Drop/refuse the non-Max peer only in HOLD.

#define DUAL_SOFTEXCL_GRACE_MS  2000u

typedef enum {
    DUAL_SX_IDLE = 0,
    DUAL_SX_HOLD,
    DUAL_SX_GRACE,
} dual_softexcl_phase_t;

typedef struct {
    bool enabled;
    dual_softexcl_phase_t phase;
    uint32_t grace_until_ms;
} dual_softexcl_t;

// Flash byte: 1=on, 2=off; 0x00 / 0xFF / anything else → on (factory default).
static inline bool dual_softexcl_flash_means_on(uint8_t b) {
    return b != 2u;
}

static inline void dual_softexcl_init(dual_softexcl_t *s, bool enabled) {
    if (!s) return;
    s->enabled = enabled;
    s->phase = DUAL_SX_IDLE;
    s->grace_until_ms = 0;
}

static inline void dual_softexcl_set_enabled(dual_softexcl_t *s, bool enabled) {
    if (!s) return;
    s->enabled = enabled;
    if (!enabled) {
        s->phase = DUAL_SX_IDLE;
        s->grace_until_ms = 0;
    }
}

static inline const char *dual_softexcl_phase_name(dual_softexcl_phase_t p) {
    switch (p) {
        case DUAL_SX_HOLD:  return "hold";
        case DUAL_SX_GRACE: return "grace";
        default:            return "idle";
    }
}

static inline bool dual_softexcl_should_drop_peer(const dual_softexcl_t *s) {
    return s && s->enabled && s->phase == DUAL_SX_HOLD;
}

// Do not honor 0x11 / owns=00 / 0x0E they-own give-up while HOLD — that
// would undo exclusive. Firmware kicks the phone instead.
static inline bool dual_softexcl_should_honor_giveup(const dual_softexcl_t *s) {
    return !dual_softexcl_should_drop_peer(s);
}

// Steal reclaim / 0x10 hijack only when softexcl is off (fight fallback).
static inline bool dual_connect_should_fight_on_steal(bool softexcl_on,
                                                     bool usb_spk_open,
                                                     bool is_usb_streaming,
                                                     bool we_paused_after_giveup) {
    if (softexcl_on) return false;
    return dual_connect_should_reclaim_on_steal(usb_spk_open, is_usb_streaming,
                                                we_paused_after_giveup);
}

static inline dual_softexcl_phase_t dual_softexcl_step(dual_softexcl_t *s,
                                                      bool usb_wants_sink,
                                                      uint32_t now_ms) {
    if (!s) return DUAL_SX_IDLE;
    if (!s->enabled) {
        s->phase = DUAL_SX_IDLE;
        s->grace_until_ms = 0;
        return DUAL_SX_IDLE;
    }
    if (usb_wants_sink) {
        s->phase = DUAL_SX_HOLD;
        s->grace_until_ms = 0;
        return DUAL_SX_HOLD;
    }
    if (s->phase == DUAL_SX_HOLD) {
        s->phase = DUAL_SX_GRACE;
        s->grace_until_ms = now_ms + DUAL_SOFTEXCL_GRACE_MS;
        return DUAL_SX_GRACE;
    }
    if (s->phase == DUAL_SX_GRACE) {
        if ((int32_t)(now_ms - s->grace_until_ms) >= 0) {
            s->phase = DUAL_SX_IDLE;
            s->grace_until_ms = 0;
            return DUAL_SX_IDLE;
        }
        return DUAL_SX_GRACE;
    }
    return DUAL_SX_IDLE;
}

static inline bool dual_softexcl_mac_eq(const uint8_t a[6], const uint8_t b[6]) {
    return a && b && memcmp(a, b, 6) == 0;
}

static inline bool dual_softexcl_mac_nonzero(const uint8_t mac[6]) {
    static const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    static const uint8_t f[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!mac) return false;
    if (memcmp(mac, z, 6) == 0) return false;
    if (memcmp(mac, f, 6) == 0) return false;
    return true;
}

// Self (Pico) and the Max headset must never be HCI-disconnected.
static inline bool dual_softexcl_mac_is_protected(const uint8_t mac[6],
                                                  const uint8_t self[6],
                                                  const uint8_t max_addr[6]) {
    if (!dual_softexcl_mac_nonzero(mac)) return true;
    if (dual_softexcl_mac_nonzero(self) && dual_softexcl_mac_eq(mac, self)) {
        return true;
    }
    if (dual_softexcl_mac_nonzero(max_addr) && dual_softexcl_mac_eq(mac, max_addr)) {
        return true;
    }
    return false;
}

// Prefer 0x0E MEDIA/CALL peer ≠ self ≠ Max; else first 0x2E record that
// is not protected. Returns false if no phone-like peer is known.
static inline bool dual_softexcl_pick_phone_peer(const dual_audio_source_t *src,
                                                 bool src_known,
                                                 const dual_connected_device_t *devs,
                                                 int ndevs,
                                                 const uint8_t self[6],
                                                 const uint8_t max_addr[6],
                                                 uint8_t out[6]) {
    if (!out) return false;
    if (src_known && src &&
        (src->type == DUAL_AUDIO_SRC_MEDIA || src->type == DUAL_AUDIO_SRC_CALL) &&
        !dual_softexcl_mac_is_protected(src->mac, self, max_addr)) {
        memcpy(out, src->mac, 6);
        return true;
    }
    if (!devs || ndevs <= 0) return false;
    for (int i = 0; i < ndevs; i++) {
        if (dual_softexcl_mac_is_protected(devs[i].mac, self, max_addr)) continue;
        memcpy(out, devs[i].mac, 6);
        return true;
    }
    return false;
}

#endif // USBPODS_DUAL_CONNECT_POLICY_H
