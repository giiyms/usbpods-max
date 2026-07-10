//
// AACP (Apple Accessory Protocol) channel for AirPods hi-res mic.
// Phase 1: establish L2CAP PSM 0x1001, run the init handshake, and hex-dump
// all received packets so we can confirm the AirPods accept our commands.
//
// Ported from librepods (linux-rust) aacp.rs handshake sequence. See HANDOFF.md §3.
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

#endif // BTSTACK_AACP_H
