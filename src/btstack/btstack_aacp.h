// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Derived from librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AACP (Apple Accessory Protocol) channel for the AirPods hi-res mic:
// establishes L2CAP PSM 0x1001, runs the init handshake, START/STOPs the
// mic stream, and demuxes type-0x58 audio SDUs into AAC-ELD access units.
//
// Ported from librepods (linux-rust branch) aacp.rs / aacp_audio.rs.
// Control identifiers and packet layouts are from published LibrePods docs
// plus working Android/Linux send/parse — no invented opcodes.
//

#ifndef BTSTACK_AACP_H
#define BTSTACK_AACP_H

#include <stdint.h>
#include <stdbool.h>
#include "btstack.h"
#include "aacp_status.h"

// One-time setup. Call once from btstack_main() after l2cap_init().
void aacp_init(void);

// Initiate the AACP L2CAP connection (PSM 0x1001) to the given AirPods address.
// Call once the A2DP signaling connection to the same device is established.
// A short settle delay is applied internally before the outgoing connect.
void aacp_connect(bd_addr_t addr);

// Tear down the AACP channel if open.
void aacp_disconnect(void);

// True once the channel is open (handshake may still be in flight).
bool aacp_is_connected(void);

// --- Hi-res mic stream ---

// Send the mic START / STOP byte sequences. If the channel is not open yet,
// START is remembered and fired once it is. START also arms the per-second
// RX statistics report; STOP stops it.
void aacp_mic_start(void);
void aacp_mic_stop(void);

// True between aacp_mic_start() and aacp_mic_stop()/channel close.
bool aacp_mic_active(void);

// --- Status parsed from AACP notifications (librepods opcodes, no invented
// packets). Values of 0xFF mean "not yet reported". Noise/CA 0 = unknown.

void aacp_get_battery(uint8_t *left, uint8_t *right, uint8_t *case_bat, uint8_t *headset);
void aacp_get_ear(uint8_t *left, uint8_t *right);
uint8_t aacp_get_noise_mode(void);   // 1=Off 2=ANC 3=Transparency 4=Adaptive
uint8_t aacp_get_ca(void);           // 1=enabled 2=disabled (control id 0x28)

uint8_t aacp_get_owns(void);         // 1=we own, 2=not, 0=unknown
uint8_t aacp_get_auto_conn(void);    // id 0x20: 1=enabled 2=disabled
uint8_t aacp_get_allow_auto(void);   // id 0x36: never sent by us; 0 if unset
uint8_t aacp_get_ear_en(void);       // id 0x0A
uint8_t aacp_get_gestures(void);     // id 0x39 bitmask
uint8_t aacp_get_click_hold(void);   // id 0x16 first byte
uint8_t aacp_get_crown_dir(void);    // id 0x1C: 1=reversed 2=default
uint8_t aacp_get_auto_ans(void);     // id 0x1E: 1=on 2=off
uint8_t aacp_get_chime(void);        // id 0x1F 0..100
uint8_t aacp_get_adapt_vol(void);    // id 0x26
uint8_t aacp_get_sleep_det(void);    // id 0x35
uint8_t aacp_get_listen_mask(void);  // id 0x1A
void aacp_get_last19(uint8_t *type, uint8_t *bud);
const char *aacp_get_last19_hex(void);
const char *aacp_get_dev_name(void);
const char *aacp_get_dev_model(void);
const char *aacp_get_dev_serial(void);
const char *aacp_get_dev_fw(void);

// Re-send LibrePods 0x06 owns + 0x20 auto-conn. Call after dual-connect steal
// while AACP is still up. Does not invent hijack/0x0E blobs.
void aacp_reassert_ownership(void);

// Send documented control commands (opcode 0x0009). Returns false if the
// channel is not open. mode: 1-4 as above. ca: 1=on, 2=off.
bool aacp_set_noise_mode(uint8_t mode);
bool aacp_set_ca(uint8_t enable);
bool aacp_set_auto_conn(uint8_t enable);   // 1/2 — LibrePods 0x20
bool aacp_set_ear_en(uint8_t enable);
bool aacp_set_gestures(uint8_t mask);
bool aacp_set_click_hold(uint8_t mode);    // 0x01 noise, 0x05 Siri (both sides)
bool aacp_set_crown_dir(uint8_t dir);
bool aacp_set_auto_ans(uint8_t enable);
bool aacp_set_chime(uint8_t vol);
bool aacp_set_adapt_vol(uint8_t enable);
bool aacp_set_sleep_det(uint8_t enable);
bool aacp_set_listen_mask(uint8_t mask);
bool aacp_rename(const char *name);        // opcode 0x001A (LibrePods send path)

// Flash persist for host prefs. Call from the main loop only.
bool aacp_prefs_dirty(void);
void aacp_prefs_clear_dirty(void);
void aacp_prefs_fill(uint8_t *auto_ans, uint8_t *chime, uint8_t *adapt_vol,
                     uint8_t *sleep_det, uint8_t *crown_dir, uint8_t *listen_mask);
void aacp_prefs_apply_loaded(uint8_t auto_ans, uint8_t chime, uint8_t adapt_vol,
                             uint8_t sleep_det, uint8_t crown_dir, uint8_t listen_mask);

#endif // BTSTACK_AACP_H
