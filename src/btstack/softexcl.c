// SPDX-License-Identifier: GPL-3.0-only
//
// Soft exclusive runtime. HCI disconnect of extra ACLs only — no Forget,
// no link-key wipe, never disconnect the Max or local address.
// Call gap_disconnect only from BTstack context (HCI handler, AACP/AVDTP
// handlers, or the softexcl poll timer). USB IRQ / CDC must not kick.

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "softexcl.h"
#include "btstack_hci.h"
#include "btstack_avdtp_source.h"
#include "btstack_aacp.h"
#include "../pico_w_led.h"

#define SX_ACL_MAX  2
#define SX_KICK_COOLDOWN_MS  400u
#define SX_POLL_MS           50u

static dual_softexcl_t sx;
static bool prefs_dirty;
static uint32_t last_kick_ms;
static bool last_kick_valid;
static btstack_timer_source_t sx_timer;

static struct {
    bool used;
    uint8_t addr[6];
    uint16_t handle;
} acls[SX_ACL_MAX];

static dual_audio_source_t src_cache;
static bool src_known;
static dual_connected_device_t devs_cache[DUAL_CONN_DEV_MAX];
static int devs_n;
static uint8_t phone_mac[6];
static bool phone_known;

static void sx_timer_handler(btstack_timer_source_t *ts);

static void sx_log_mac(const char *tag, const uint8_t mac[6]) {
    printf("[SX] %s %02X:%02X:%02X:%02X:%02X:%02X\n",
           tag, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void fill_protected(uint8_t self[6], uint8_t maxa[6]) {
    bd_addr_t local;
    gap_local_bd_addr(local);
    memcpy(self, local, 6);
    memcpy(maxa, *get_device_addr(), 6);
}

static void acl_remove_handle(uint16_t handle) {
    for (int i = 0; i < SX_ACL_MAX; i++) {
        if (acls[i].used && acls[i].handle == handle) {
            acls[i].used = false;
        }
    }
}

static void acl_add(const uint8_t addr[6], uint16_t handle) {
    acl_remove_handle(handle);
    int slot = -1;
    for (int i = 0; i < SX_ACL_MAX; i++) {
        if (!acls[i].used) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    acls[slot].used = true;
    memcpy(acls[slot].addr, addr, 6);
    acls[slot].handle = handle;
}

static bool acl_has_mac(const uint8_t mac[6]) {
    for (int i = 0; i < SX_ACL_MAX; i++) {
        if (acls[i].used && dual_softexcl_mac_eq(acls[i].addr, mac)) return true;
    }
    return false;
}

static void refresh_phone_from_aacp(void) {
    uint8_t mac[6];
    uint8_t type = 0;
    bool known = false;
    aacp_get_audio_src(mac, &type, &known);
    if (known) {
        memcpy(src_cache.mac, mac, 6);
        src_cache.type = type;
        src_known = true;
    }
    int n = aacp_get_connected_device_count();
    if (n > DUAL_CONN_DEV_MAX) n = DUAL_CONN_DEV_MAX;
    devs_n = 0;
    for (int i = 0; i < n; i++) {
        if (aacp_get_connected_device(i, devs_cache[devs_n].mac,
                                      &devs_cache[devs_n].info1,
                                      &devs_cache[devs_n].info2)) {
            devs_n++;
        }
    }
}

static void refresh_phone(void) {
    uint8_t self[6], maxa[6], out[6];
    fill_protected(self, maxa);
    refresh_phone_from_aacp();
    if (dual_softexcl_pick_phone_peer(&src_cache, src_known, devs_cache, devs_n,
                                      self, maxa, out)) {
        memcpy(phone_mac, out, 6);
        phone_known = true;
    }
}

static void sx_step_now(uint32_t now_ms) {
    bool wants = dual_connect_usb_wants_sink(avdtp_usb_speaker_is_open(),
                                             avdtp_usb_is_streaming());
    dual_softexcl_step(&sx, wants, now_ms);
}

static void kick(uint32_t now_ms) {
    sx_step_now(now_ms);
    if (!dual_softexcl_should_drop_peer(&sx)) return;
    if (last_kick_valid &&
        (int32_t)(now_ms - last_kick_ms) < (int32_t) SX_KICK_COOLDOWN_MS) {
        return;
    }
    last_kick_ms = now_ms;
    last_kick_valid = true;

    uint8_t self[6], maxa[6];
    fill_protected(self, maxa);
    refresh_phone();

    int dropped = 0;
    for (int i = 0; i < SX_ACL_MAX; i++) {
        if (!acls[i].used) continue;
        if (dual_softexcl_mac_is_protected(acls[i].addr, self, maxa)) continue;
        printf("[SX] HCI disconnect extra ACL "
               "%02X:%02X:%02X:%02X:%02X:%02X handle 0x%04x (bond kept)\n",
               acls[i].addr[0], acls[i].addr[1], acls[i].addr[2],
               acls[i].addr[3], acls[i].addr[4], acls[i].addr[5],
               (unsigned) acls[i].handle);
        gap_disconnect(acls[i].handle);
        dropped++;
    }

    if (phone_known) {
        if (dual_softexcl_mac_is_protected(phone_mac, self, maxa)) {
            printf("[SX] learned peer is Max/self — not disconnecting\n");
        } else if (!acl_has_mac(phone_mac)) {
            sx_log_mac("no Pico ACL to phone", phone_mac);
            printf("[SX] Max-side dual-connect cannot be HCI-dropped; "
                   "`softexcl off` restores reclaim/0x10 fight\n");
        }
    } else if (dropped == 0) {
        printf("[SX] HOLD, no extra Pico ACL (iPhone is not on this HCI)\n");
    }
}

void softexcl_init(void) {
    host_prefs_t prefs;
    read_host_prefs_flash(&prefs);
    bool on = dual_softexcl_flash_means_on(prefs.softexcl);
    dual_softexcl_init(&sx, on);
    prefs_dirty = false;
    last_kick_valid = false;
    memset(acls, 0, sizeof(acls));
    src_known = false;
    devs_n = 0;
    phone_known = false;
    printf("[SX] soft exclusive %s (default on; CDC `softexcl on|off`; "
           "Quiet toggle). iPhone stays paired.\n",
           on ? "on" : "off");
    btstack_run_loop_set_timer_handler(&sx_timer, sx_timer_handler);
    btstack_run_loop_set_timer(&sx_timer, SX_POLL_MS);
    btstack_run_loop_add_timer(&sx_timer);
}

bool softexcl_enabled(void) { return sx.enabled; }

void softexcl_set_enabled(bool on) {
    bool was = sx.enabled;
    dual_softexcl_set_enabled(&sx, on);
    if (was != on) {
        prefs_dirty = true;
        printf("[SX] softexcl %s (RAM now; flash persist when AACP idle)\n",
               on ? "on" : "off");
    }
}

bool softexcl_prefs_dirty(void) { return prefs_dirty; }
void softexcl_prefs_clear_dirty(void) { prefs_dirty = false; }
uint8_t softexcl_flash_value(void) { return sx.enabled ? 1u : 2u; }

dual_softexcl_phase_t softexcl_phase(void) { return sx.phase; }
const char *softexcl_phase_str(void) { return dual_softexcl_phase_name(sx.phase); }

bool softexcl_hold_blocks_giveup(void) {
    sx_step_now(btstack_run_loop_get_time_ms());
    return !dual_softexcl_should_honor_giveup(&sx);
}

void softexcl_poll(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    dual_softexcl_phase_t prev = sx.phase;
    sx_step_now(now);
    if (sx.phase != prev) {
        printf("[SX] phase %s → %s (spk=%u stream=%u)\n",
               dual_softexcl_phase_name(prev),
               dual_softexcl_phase_name(sx.phase),
               avdtp_usb_speaker_is_open() ? 1 : 0,
               avdtp_usb_is_streaming() ? 1 : 0);
        if (sx.phase == DUAL_SX_GRACE) {
            printf("[SX] USB idle — GRACE %u ms then phone may return\n",
                   (unsigned) DUAL_SOFTEXCL_GRACE_MS);
        }
        if (sx.phase == DUAL_SX_IDLE) {
            printf("[SX] IDLE — phone reconnect allowed (bond kept)\n");
        }
    }
    if (dual_softexcl_should_drop_peer(&sx)) {
        kick(now);
    }
}

static void sx_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    softexcl_poll();
    btstack_run_loop_set_timer_handler(&sx_timer, sx_timer_handler);
    btstack_run_loop_set_timer(&sx_timer, SX_POLL_MS);
    btstack_run_loop_add_timer(&sx_timer);
}

void softexcl_kick_now(void) {
    kick(btstack_run_loop_get_time_ms());
}

void softexcl_hci_connection_complete(const uint8_t addr[6], uint16_t handle,
                                      uint8_t status, uint8_t link_type) {
    if (status != 0 || !addr) return;
    if (link_type != 0x01u) return; /* HCI_LINK_TYPE_ACL */
    acl_add(addr, handle);
    uint8_t self[6], maxa[6];
    fill_protected(self, maxa);
    printf("[SX] ACL up %02X:%02X:%02X:%02X:%02X:%02X handle 0x%04x\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
           (unsigned) handle);
    sx_step_now(btstack_run_loop_get_time_ms());
    if (dual_softexcl_should_drop_peer(&sx) &&
        !dual_softexcl_mac_is_protected(addr, self, maxa)) {
        printf("[SX] refuse extra ACL while HOLD (disconnect, bond kept)\n");
        gap_disconnect(handle);
    }
}

void softexcl_hci_disconnection_complete(uint16_t handle) {
    acl_remove_handle(handle);
}

void softexcl_note_audio_src(const uint8_t mac[6], uint8_t type) {
    if (!mac) return;
    memcpy(src_cache.mac, mac, 6);
    src_cache.type = type;
    src_known = true;
    softexcl_note_peer_mac(mac);
}

void softexcl_note_connected_devices(const dual_connected_device_t *devs, int n) {
    if (!devs || n < 0) return;
    if (n > DUAL_CONN_DEV_MAX) n = DUAL_CONN_DEV_MAX;
    memset(devs_cache, 0, sizeof(devs_cache));
    memcpy(devs_cache, devs, (size_t)n * sizeof(devs[0]));
    devs_n = n;
    refresh_phone();
}

void softexcl_note_peer_mac(const uint8_t mac[6]) {
    uint8_t self[6], maxa[6];
    fill_protected(self, maxa);
    if (dual_softexcl_mac_is_protected(mac, self, maxa)) return;
    memcpy(phone_mac, mac, 6);
    phone_known = true;
}
