//
// AACP (Apple Accessory Protocol) channel for AirPods hi-res mic.
// Phase 1: establish L2CAP PSM 0x1001, run the init handshake, and hex-dump
// all received packets so we can confirm the AirPods accept our commands.
// Phase 2: START/STOP the hi-res mic stream and demux type-0x58 audio SDUs
// into AAC-ELD access units, with per-second RX statistics.
//
// Ported from librepods (linux-rust) aacp.rs / aacp_audio.rs. See HANDOFF.md §3.
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

// --- Phase 2: hi-res mic stream ---

// Send the mic START / STOP byte sequences (HANDOFF §3.3). No-ops if the
// channel is not open. START also arms the per-second RX statistics report;
// STOP stops it.
void aacp_mic_start(void);
void aacp_mic_stop(void);

// True between aacp_mic_start() and aacp_mic_stop()/channel close.
bool aacp_mic_active(void);

#endif // BTSTACK_AACP_H
