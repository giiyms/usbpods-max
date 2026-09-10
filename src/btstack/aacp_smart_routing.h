// SPDX-License-Identifier: GPL-3.0-only
//
// LibrePods AACP 0x10 smart-routing builders (host-side, pure helpers).
// Source of truth: librepods AACPManager.kt create*Packet / ByteBuffer layout.
// HEADER 04 00 04 00 + opcode 10 00 + buffer (zero-padded to allocate size).
// btName = "Android" (LibrePods phone path length/behavior).
// No invented opcodes/payloads. Do not edit aacp_mic_dec.c.

#ifndef USBPODS_AACP_SMART_ROUTING_H
#define USBPODS_AACP_SMART_ROUTING_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "dual_connect_policy.h"

#define AACP_SR_OPCODE           0x10u
#define AACP_SR_HIJACK_BUF       106u
#define AACP_SR_MEDIA_BUF        138u
#define AACP_SR_SHOWUI_BUF       134u
#define AACP_SR_REVERSED_BUF     97u
#define AACP_SR_MEDIA_NEW_BUF    116u
#define AACP_SR_ADD_TIPI_BUF     90u

#define AACP_SR_HIJACK_LEN       (4u + 2u + AACP_SR_HIJACK_BUF)      /* 112 */
#define AACP_SR_MEDIA_LEN        (4u + 2u + AACP_SR_MEDIA_BUF)       /* 144 */
#define AACP_SR_SHOWUI_LEN       (4u + 2u + AACP_SR_SHOWUI_BUF)      /* 140 */
#define AACP_SR_REVERSED_LEN     (4u + 2u + AACP_SR_REVERSED_BUF)    /* 103 */
#define AACP_SR_MEDIA_NEW_LEN    (4u + 2u + AACP_SR_MEDIA_NEW_BUF)   /* 122 */
#define AACP_SR_ADD_TIPI_LEN     (4u + 2u + AACP_SR_ADD_TIPI_BUF)    /* 96 */
#define AACP_SR_MAX_LEN          AACP_SR_MEDIA_LEN

#define AACP_SR_BTNAME           "Android"
#define AACP_SR_MAC_ASCII_LEN    17u  /* "XX:XX:XX:XX:XX:XX" */

// Only send takeOver 0x10 hijack when USB wants the sink and anti-ping-pong
// is not armed (LibrePods #724 / dual_connect_should_reclaim_on_steal).
// 0x11 RX SetOwnershipToFalse is parsed in dual_connect_policy.h — never
// answer it with an immediate 0x10 burst.
static inline bool aacp_sr_should_send_hijack(bool usb_spk_open,
                                              bool is_usb_streaming,
                                              bool we_paused_after_giveup) {
    return dual_connect_should_reclaim_on_steal(usb_spk_open, is_usb_streaming,
                                                we_paused_after_giveup);
}

// Soft exclusive (default on) skips LibrePods 0x10 takeOver; reclaim/0x10
// remain the fight-path fallback when softexcl is off.
static inline bool aacp_sr_should_send_hijack_gated(bool softexcl_on,
                                                    bool usb_spk_open,
                                                    bool is_usb_streaming,
                                                    bool we_paused_after_giveup) {
    if (softexcl_on) return false;
    return aacp_sr_should_send_hijack(usb_spk_open, is_usb_streaming,
                                      we_paused_after_giveup);
}

// Format BD_ADDR bytes (display order) as LibrePods "%02X:%02X:..." ASCII.
static inline void aacp_sr_mac_to_ascii(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static inline void aacp_sr_put_mac_le_rev(uint8_t *dst, const uint8_t mac[6]) {
    dst[0] = mac[5];
    dst[1] = mac[4];
    dst[2] = mac[3];
    dst[3] = mac[2];
    dst[4] = mac[1];
    dst[5] = mac[0];
}

static inline void aacp_sr_hdr_op(uint8_t *out) {
    out[0] = 0x04; out[1] = 0x00; out[2] = 0x04; out[3] = 0x00;
    out[4] = AACP_SR_OPCODE; out[5] = 0x00;
}

// createHijackRequestPacket — allocate(106), len 62 00, 01 E5, localscore/…
static inline size_t aacp_sr_build_hijack(uint8_t out[AACP_SR_HIJACK_LEN],
                                          const uint8_t target_mac[6]) {
    memset(out, 0, AACP_SR_HIJACK_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x62; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE5;
    b[p++] = 0x4A;
    memcpy(b + p, "localscore", 10); p += 10;
    b[p++] = 0x30; b[p++] = 0x64;
    b[p++] = 0x46;
    memcpy(b + p, "reason", 6); p += 6;
    b[p++] = 0x48;
    memcpy(b + p, "Hijackv2", 8); p += 8;
    b[p++] = 0x51;
    memcpy(b + p, "audioRoutingScore", 17); p += 17;
    b[p++] = 0x31; b[p++] = 0x2D; b[p++] = 0x01; b[p++] = 0x5F;
    memcpy(b + p, "audioRoutingSetOwnershipToFalse", 31); p += 31;
    b[p++] = 0x01;
    b[p++] = 0x4B;
    memcpy(b + p, "remotescore", 11); p += 11;
    b[p++] = 0xA5;
    (void)p;
    return AACP_SR_HIJACK_LEN;
}

// createMediaInformationPacket — allocate(138). LibrePods quirk: no 0x46
// before "btName". btName "Android". PlayingApp = com.google.ios.youtube.
static inline size_t aacp_sr_build_media_info(uint8_t out[AACP_SR_MEDIA_LEN],
                                              const uint8_t self_mac[6],
                                              const uint8_t target_mac[6],
                                              bool streaming) {
    memset(out, 0, AACP_SR_MEDIA_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    char self_ascii[18];
    aacp_sr_mac_to_ascii(self_mac, self_ascii);
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x82; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE5; b[p++] = 0x4A;
    memcpy(b + p, "PlayingApp", 10); p += 10;
    b[p++] = 0x56;
    memcpy(b + p, "com.google.ios.youtube", 22); p += 22;
    b[p++] = 0x52;
    memcpy(b + p, "HostStreamingState", 18); p += 18;
    b[p++] = 0x42;
    if (streaming) {
        memcpy(b + p, "YES", 3); p += 3;
    } else {
        memcpy(b + p, "NO", 2); p += 2;
    }
    b[p++] = 0x49;
    memcpy(b + p, "btAddress", 9); p += 9;
    b[p++] = 0x51;
    memcpy(b + p, self_ascii, AACP_SR_MAC_ASCII_LEN); p += AACP_SR_MAC_ASCII_LEN;
    /* intentional: no 0x46 separator before btName (AACPManager.kt) */
    memcpy(b + p, "btName", 6); p += 6;
    b[p++] = 0x47;
    memcpy(b + p, AACP_SR_BTNAME, 7); p += 7;
    b[p++] = 0x58;
    memcpy(b + p, "otherDevice", 11); p += 11;
    memcpy(b + p, "AudioCategory", 13); p += 13;
    b[p++] = 0x31; b[p++] = 0x2D; b[p++] = 0x01;
    (void)p;
    return AACP_SR_MEDIA_LEN;
}

// createSmartRoutingShowUIPacket — allocate(134).
// Kotlin `buffer.put(0x31, 0x2D)` is ByteBuffer.put(int index, byte) at
// index 0x31 (overwrites final 'e' of "localscore" with 0x2D); then relative
// 0x01 0x46. Verbatim ByteBuffer semantics — not an invented fix-up.
static inline size_t aacp_sr_build_show_ui(uint8_t out[AACP_SR_SHOWUI_LEN],
                                           const uint8_t target_mac[6]) {
    memset(out, 0, AACP_SR_SHOWUI_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x7E; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE6; b[p++] = 0x5B;
    memcpy(b + p, "SmartRoutingKeyShowNearbyUI", 27); p += 27;
    b[p++] = 0x01;
    b[p++] = 0x4A;
    memcpy(b + p, "localscore", 10); p += 10;
    /* absolute put(0x31, 0x2D): write 0x2D at buffer index 0x31, pos unchanged */
    b[0x31] = 0x2D;
    b[p++] = 0x01;
    b[p++] = 0x46;
    memcpy(b + p, "reasonHhijackv2", 15); p += 15;
    b[p++] = 0x51;
    memcpy(b + p, "audioRoutingScore", 17); p += 17;
    b[p++] = 0xA2;
    b[p++] = 0x5F;
    memcpy(b + p, "audioRoutingSetOwnershipToFalse", 31); p += 31;
    b[p++] = 0x01;
    b[p++] = 0x4B;
    memcpy(b + p, "remotescore", 11); p += 11;
    b[p++] = 0xA2;
    (void)p;
    return AACP_SR_SHOWUI_LEN;
}

// createHijackReversedPacket — allocate(97). Optional reverse-banner path.
static inline size_t aacp_sr_build_hijack_reversed(uint8_t out[AACP_SR_REVERSED_LEN],
                                                   const uint8_t target_mac[6]) {
    memset(out, 0, AACP_SR_REVERSED_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x59; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE3;
    b[p++] = 0x5F;
    memcpy(b + p, "audioRoutingSetOwnershipToFalse", 31); p += 31;
    b[p++] = 0x01;
    b[p++] = 0x59;
    memcpy(b + p, "audioRoutingShowReverseUI", 25); p += 25;
    b[p++] = 0x01;
    b[p++] = 0x46;
    memcpy(b + p, "reason", 6); p += 6;
    b[p++] = 0x53;
    memcpy(b + p, "ReverseBannerTapped", 19); p += 19;
    (void)p;
    return AACP_SR_REVERSED_LEN;
}

// createMediaInformationNewDevicePacket — allocate(116).
static inline size_t aacp_sr_build_media_new_device(uint8_t out[AACP_SR_MEDIA_NEW_LEN],
                                                    const uint8_t self_mac[6],
                                                    const uint8_t target_mac[6]) {
    memset(out, 0, AACP_SR_MEDIA_NEW_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    char self_ascii[18];
    aacp_sr_mac_to_ascii(self_mac, self_ascii);
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x6C; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE5; b[p++] = 0x4A;
    memcpy(b + p, "playingApp", 10); p += 10;
    b[p++] = 0x42;
    memcpy(b + p, "NA", 2); p += 2;
    b[p++] = 0x52;
    memcpy(b + p, "hostStreamingState", 18); p += 18;
    b[p++] = 0x42;
    memcpy(b + p, "NO", 2); p += 2;
    b[p++] = 0x49;
    memcpy(b + p, "btAddress", 9); p += 9;
    b[p++] = 0x51;
    memcpy(b + p, self_ascii, AACP_SR_MAC_ASCII_LEN); p += AACP_SR_MAC_ASCII_LEN;
    b[p++] = 0x46;
    memcpy(b + p, "btName", 6); p += 6;
    b[p++] = 0x47;
    memcpy(b + p, AACP_SR_BTNAME, 7); p += 7;
    b[p++] = 0x58;
    memcpy(b + p, "otherDevice", 11); p += 11;
    memcpy(b + p, "AudioCategory", 13); p += 13;
    b[p++] = 0x30; b[p++] = 0x64;
    (void)p;
    return AACP_SR_MEDIA_NEW_LEN;
}

// createAddTiPiDevicePacket — allocate(90).
static inline size_t aacp_sr_build_add_tipi(uint8_t out[AACP_SR_ADD_TIPI_LEN],
                                            const uint8_t self_mac[6],
                                            const uint8_t target_mac[6]) {
    memset(out, 0, AACP_SR_ADD_TIPI_LEN);
    aacp_sr_hdr_op(out);
    uint8_t *b = out + 6;
    size_t p = 0;
    char self_ascii[18];
    aacp_sr_mac_to_ascii(self_mac, self_ascii);
    aacp_sr_put_mac_le_rev(b + p, target_mac); p += 6;
    b[p++] = 0x52; b[p++] = 0x00;
    b[p++] = 0x01; b[p++] = 0xE5;
    b[p++] = 0x48;
    memcpy(b + p, "idleTime", 8); p += 8;
    b[p++] = 0x08; b[p++] = 0x47;
    memcpy(b + p, "newTipi", 7); p += 7;
    b[p++] = 0x01; b[p++] = 0x49;
    memcpy(b + p, "btAddress", 9); p += 9;
    b[p++] = 0x51;
    memcpy(b + p, self_ascii, AACP_SR_MAC_ASCII_LEN); p += AACP_SR_MAC_ASCII_LEN;
    b[p++] = 0x46;
    memcpy(b + p, "btName", 6); p += 6;
    b[p++] = 0x47;
    memcpy(b + p, AACP_SR_BTNAME, 7); p += 7;
    b[p++] = 0x50;
    memcpy(b + p, "nearbyAudioScore", 16); p += 16;
    b[p++] = 0x0E;
    (void)p;
    return AACP_SR_ADD_TIPI_LEN;
}

#endif /* USBPODS_AACP_SMART_ROUTING_H */
