// SPDX-License-Identifier: GPL-3.0-only
//
// Soft exclusive: while USB wants the sink, drop extra Pico HCI ACLs that
// are not the AirPods Max / not self. Keep the iPhone bond (never Forget,
// never wipe keys, never HCI-drop the Max). Fight path (AVDTP reclaim /
// LibrePods 0x10) is the fallback when softexcl is off.
//
#ifndef USBPODS_SOFTEXCL_H
#define USBPODS_SOFTEXCL_H

#include <stdint.h>
#include <stdbool.h>
#include "dual_connect_policy.h"

void softexcl_init(void);

bool softexcl_enabled(void);
void softexcl_set_enabled(bool on);

bool softexcl_prefs_dirty(void);
void softexcl_prefs_clear_dirty(void);
uint8_t softexcl_flash_value(void); // 1=on, 2=off

dual_softexcl_phase_t softexcl_phase(void);
const char *softexcl_phase_str(void);

// True in HOLD: skip 0x11 / owns=00 they-own give-up and kick instead.
bool softexcl_hold_blocks_giveup(void);

// Call from the main loop with the BTstack lock held. Steps HOLD/GRACE/IDLE
// from live USB speaker/stream flags and disconnects extra ACLs while HOLD.
void softexcl_poll(void);

// Same as poll's HOLD kick; also steps the machine first (USB rising).
void softexcl_kick_now(void);

void softexcl_hci_connection_complete(const uint8_t addr[6], uint16_t handle,
                                      uint8_t status, uint8_t link_type);
void softexcl_hci_disconnection_complete(uint16_t handle);

void softexcl_note_audio_src(const uint8_t mac[6], uint8_t type);
void softexcl_note_connected_devices(const dual_connected_device_t *devs, int n);
void softexcl_note_peer_mac(const uint8_t mac[6]);

#endif
