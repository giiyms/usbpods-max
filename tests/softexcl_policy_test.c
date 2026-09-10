// Host-side proof of soft-exclusive dual-connect policy.
// gcc -I. -O2 -o /tmp/softexcl_policy_test tests/softexcl_policy_test.c && /tmp/softexcl_policy_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/btstack/dual_connect_policy.h"
#include "src/btstack/aacp_smart_routing.h"

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

#define EQS(a, b, n, msg) do { if (memcmp((a), (b), (n)) != 0) { \
    fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while (0)

int main(void) {
    static const uint8_t k_self[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    static const uint8_t k_max[6]  = { 0x70, 0xF9, 0x4A, 0x8A, 0xD8, 0x54 };
    static const uint8_t k_phone[6] = { 0xFC, 0x31, 0x5D, 0xC9, 0x6D, 0xCC };
    static const uint8_t k_zero[6] = { 0, 0, 0, 0, 0, 0 };

    // Flash default on; only explicit 2 is off.
    EQ(dual_softexcl_flash_means_on(0x00), 1, "flash 00 → on");
    EQ(dual_softexcl_flash_means_on(0xFF), 1, "flash FF → on");
    EQ(dual_softexcl_flash_means_on(1), 1, "flash 1 → on");
    EQ(dual_softexcl_flash_means_on(2), 0, "flash 2 → off");

    dual_softexcl_t sx;
    dual_softexcl_init(&sx, true);
    EQ(sx.enabled, 1, "default enabled");
    EQ(sx.phase, DUAL_SX_IDLE, "start IDLE");
    EQ(dual_softexcl_should_drop_peer(&sx), 0, "IDLE no drop");
    EQ(dual_softexcl_should_honor_giveup(&sx), 1, "IDLE honor give-up");

    // USB rising → HOLD
    EQ(dual_softexcl_step(&sx, true, 1000), DUAL_SX_HOLD, "USB open → HOLD");
    EQ(dual_softexcl_should_drop_peer(&sx), 1, "HOLD drop");
    EQ(dual_softexcl_should_honor_giveup(&sx), 0, "HOLD skip give-up");
    EQ(strcmp(dual_softexcl_phase_name(sx.phase), "hold"), 0, "name hold");

    // Stay HOLD while USB wants sink
    EQ(dual_softexcl_step(&sx, true, 1500), DUAL_SX_HOLD, "stay HOLD");

    // USB idle → GRACE
    EQ(dual_softexcl_step(&sx, false, 2000), DUAL_SX_GRACE, "USB idle → GRACE");
    EQ(dual_softexcl_should_drop_peer(&sx), 0, "GRACE no drop");
    EQ(sx.grace_until_ms, 2000u + DUAL_SOFTEXCL_GRACE_MS, "grace deadline");
    EQ(dual_softexcl_step(&sx, false, 2000u + DUAL_SOFTEXCL_GRACE_MS - 1),
       DUAL_SX_GRACE, "still GRACE before deadline");

    // USB wants during GRACE → HOLD immediately
    EQ(dual_softexcl_step(&sx, true, 2500), DUAL_SX_HOLD, "GRACE + USB → HOLD");

    // Full idle path: HOLD → GRACE → IDLE
    EQ(dual_softexcl_step(&sx, false, 3000), DUAL_SX_GRACE, "idle → GRACE again");
    EQ(dual_softexcl_step(&sx, false, 3000u + DUAL_SOFTEXCL_GRACE_MS),
       DUAL_SX_IDLE, "grace elapsed → IDLE");
    EQ(dual_softexcl_should_drop_peer(&sx), 0, "IDLE after grace no drop");
    EQ(strcmp(dual_softexcl_phase_name(sx.phase), "idle"), 0, "name idle");

    // Disabled → IDLE, no drop even if USB wants
    dual_softexcl_step(&sx, true, 4000);
    dual_softexcl_set_enabled(&sx, false);
    EQ(sx.phase, DUAL_SX_IDLE, "disable clears HOLD");
    EQ(dual_softexcl_step(&sx, true, 4100), DUAL_SX_IDLE, "disabled stays IDLE");
    EQ(dual_softexcl_should_drop_peer(&sx), 0, "disabled no drop");

    // Fight: softexcl on alone does NOT skip reclaim (Max-only dual-connect).
    EQ(dual_connect_should_fight_on_steal(true, false, true, true, false), 1,
       "softexcl on, no kick win → fight");
    EQ(dual_connect_should_fight_on_steal(true, true, true, true, false), 0,
       "softexcl on, kick won Pico ACL → no fight");
    EQ(dual_connect_should_fight_on_steal(false, true, true, false, false), 1,
       "softexcl off → fight even if kick_won");
    EQ(dual_connect_should_fight_on_steal(false, false, true, true, true), 0,
       "softexcl off still honors anti-ping-pong");
    EQ(dual_softexcl_suppresses_fight(true, false), 0, "no kick → no suppress");
    EQ(dual_softexcl_suppresses_fight(true, true), 1, "kick won → suppress");
    EQ(dual_softexcl_suppresses_fight(false, true), 0, "off ignores kick_won");
    EQ(dual_connect_should_reclaim_on_steal(true, true, false), 1,
       "reclaim helper unchanged");

    EQ(aacp_sr_should_send_hijack_gated(false, true, true, false), 1,
       "no kick win → 0x10 still sent");
    EQ(aacp_sr_should_send_hijack_gated(true, true, true, false), 0,
       "kick won gates 0x10");
    EQ(aacp_sr_should_send_hijack_gated(false, true, false, false), 1,
       "softexcl off / no win still hijacks");

    // Protected MACs: self + Max; never drop unknown/zero
    EQ(dual_softexcl_mac_is_protected(k_self, k_self, k_max), 1, "self protected");
    EQ(dual_softexcl_mac_is_protected(k_max, k_self, k_max), 1, "Max protected");
    EQ(dual_softexcl_mac_is_protected(k_phone, k_self, k_max), 0, "phone droppable");
    EQ(dual_softexcl_mac_is_protected(k_zero, k_self, k_max), 1, "zero not dropped");
    EQ(dual_softexcl_mac_nonzero(k_phone), 1, "phone nonzero");
    EQ(dual_softexcl_mac_nonzero(k_zero), 0, "zero empty");

    // Pick phone from 0x0E MEDIA, not self / Max
    {
        dual_audio_source_t src;
        memcpy(src.mac, k_phone, 6);
        src.type = DUAL_AUDIO_SRC_MEDIA;
        uint8_t out[6];
        EQ(dual_softexcl_pick_phone_peer(&src, true, NULL, 0, k_self, k_max, out),
           1, "0x0E MEDIA phone");
        EQS(out, k_phone, 6, "0x0E picked phone");

        memcpy(src.mac, k_self, 6);
        EQ(dual_softexcl_pick_phone_peer(&src, true, NULL, 0, k_self, k_max, out),
           0, "0x0E self not phone");

        memcpy(src.mac, k_max, 6);
        EQ(dual_softexcl_pick_phone_peer(&src, true, NULL, 0, k_self, k_max, out),
           0, "0x0E Max not phone");

        src.type = DUAL_AUDIO_SRC_NONE;
        memcpy(src.mac, k_phone, 6);
        EQ(dual_softexcl_pick_phone_peer(&src, true, NULL, 0, k_self, k_max, out),
           0, "0x0E NONE not phone");
    }

    // Pick from 0x2E: skip self + Max, take iPhone
    {
        dual_connected_device_t devs[3];
        memset(devs, 0, sizeof devs);
        memcpy(devs[0].mac, k_self, 6);
        memcpy(devs[1].mac, k_max, 6);
        memcpy(devs[2].mac, k_phone, 6);
        uint8_t out[6];
        EQ(dual_softexcl_pick_phone_peer(NULL, false, devs, 3, k_self, k_max, out),
           1, "0x2E pick phone");
        EQS(out, k_phone, 6, "0x2E picked phone not Max");

        // 0x0E CALL wins over 0x2E order
        dual_audio_source_t src;
        memcpy(src.mac, k_phone, 6);
        src.type = DUAL_AUDIO_SRC_CALL;
        static const uint8_t other[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        memcpy(devs[2].mac, other, 6);
        EQ(dual_softexcl_pick_phone_peer(&src, true, devs, 3, k_self, k_max, out),
           1, "0x0E CALL preferred");
        EQS(out, k_phone, 6, "preferred 0x0E over 0x2E");
    }

    // Max-only HCI (self + Max, no phone ACL) → nothing droppable → fight
    {
        uint8_t only_max[2][6];
        memcpy(only_max[0], k_self, 6);
        memcpy(only_max[1], k_max, 6);
        EQ(dual_softexcl_count_droppable_acls(only_max, 2, k_self, k_max), 0,
           "Max-only: no droppable Pico ACL");
        EQ(dual_connect_should_fight_on_steal(true, false, true, true, false), 1,
           "Max-only: softexcl HOLD still fights");
    }
    {
        uint8_t with_phone[3][6];
        memcpy(with_phone[0], k_self, 6);
        memcpy(with_phone[1], k_max, 6);
        memcpy(with_phone[2], k_phone, 6);
        EQ(dual_softexcl_count_droppable_acls(with_phone, 3, k_self, k_max), 1,
           "phone ACL is droppable");
        EQ(dual_connect_should_fight_on_steal(true, true, true, true, false), 0,
           "after dropping phone ACL, skip fight");
    }

    printf("softexcl_policy_test: PASS "
           "(phase HOLD/GRACE/IDLE, flash default, fight unless kick won, "
           "protected Max/self, pick phone from 0x0E/0x2E, Max-only cooperates)\n");
    return 0;
}
