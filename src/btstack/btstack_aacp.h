// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Derived from librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AACP (Apple Accessory Protocol) channel for the AirPods hi-res mic:
// establishes L2CAP PSM 0x1001, runs the init handshake, START/STOPs the
// mic stream, and demuxes type-0x58 audio SDUs into AAC-ELD access units.
//
// Ported from librepods (linux-rust branch) aacp.rs / aacp_audio.rs.
//

#ifndef BTSTACK_AACP_H
#define BTSTACK_AACP_H

#include <stdint.h>
#include <stdbool.h>
#include "btstack.h"

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

// Send documented control commands (opcode 0x0009). Returns false if the
// channel is not open. mode: 1-4 as above. ca: 1=on, 2=off.
bool aacp_set_noise_mode(uint8_t mode);
bool aacp_set_ca(uint8_t enable);

#endif // BTSTACK_AACP_H
