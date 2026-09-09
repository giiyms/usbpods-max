// Host-side proof of dual-connect ownership policy.
// gcc -I. -O2 -o /tmp/dual_connect_policy_test tests/dual_connect_policy_test.c && /tmp/dual_connect_policy_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/btstack/dual_connect_policy.h"
#include "src/btstack/avdtp_reclaim.h"

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

#define EQS(a, b, n, msg) do { if (memcmp((a), (b), (n)) != 0) { \
    fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while (0)

int main(void) {
    // 1) idle no-fight
    EQ(dual_connect_should_reclaim_on_steal(false, false, false), 0, "idle no-fight");
    EQ(dual_connect_state_on_unexpected_pause(false, false, false), DUAL_USB_IDLE, "idle state");

    // 2) steal → claim (USB wants sink) + exact OWNS claim bytes
    EQ(dual_connect_should_reclaim_on_steal(true, false, false), 1, "steal reclaim usb open");
    EQ(dual_connect_should_reclaim_on_steal(false, true, false), 1, "steal reclaim usb streaming");
    EQ(dual_connect_state_on_unexpected_pause(true, false, false), DUAL_RECLAIMING, "steal→RECLAIMING");
    {
        uint8_t claim[DUAL_CTRL_FRAME_LEN];
        uint8_t giveup[DUAL_CTRL_FRAME_LEN];
        dual_connect_build_owns_claim(claim);
        dual_connect_build_owns_giveup(giveup);
        static const uint8_t want_claim[11] = {
            0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00
        };
        static const uint8_t want_giveup[11] = {
            0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00
        };
        EQS(claim, want_claim, 11, "OWNS claim exact bytes");
        EQS(giveup, want_giveup, 11, "OWNS giveup exact bytes");
    }

    // 3) status 129 via existing reclaim timing helpers (same as avdtp_reclaim_test)
    {
        uint32_t d129 = avdtp_reclaim_backoff_ms((uint8_t) AVDTP_RECLAIM_STATUS_RESOURCE, 0);
        EQ(d129, AVDTP_RECLAIM_STATUS129_MS, "status 129 backoff 5 s");
        EQ(avdtp_reclaim_arm_delay_ms(true), AVDTP_RECLAIM_STEAL_DELAY_MS, "steal arm 2 s");
    }

    // 4) owns00 stop (peer playing — earbuds arbitrating)
    EQ(dual_connect_should_stop_reclaim(true, 0x00), 1, "owns00 stop");
    EQ(dual_connect_should_stop_reclaim(true, 0x01), 0, "owns01 keep reclaiming");
    EQ(dual_connect_should_stop_reclaim(false, 0x00), 0, "owns00 ignored when idle");

    // 5) 0x0E give-up: peer MEDIA MAC ≠ us
    {
        // Header 04 00 04 00 0E 00 | MAC reversed | type
        // Display MAC AA:BB:CC:DD:EE:FF → packet bytes FF EE DD CC BB AA
        uint8_t pkt[13] = {
            0x04, 0x00, 0x04, 0x00, 0x0E, 0x00,
            0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA,
            DUAL_AUDIO_SRC_MEDIA
        };
        dual_audio_source_t src;
        EQ(dual_connect_parse_0e(pkt, sizeof pkt, &src), 1, "0x0E parse ok");
        static const uint8_t peer[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        EQS(src.mac, peer, 6, "0x0E MAC reversed");
        EQ(src.type, DUAL_AUDIO_SRC_MEDIA, "0x0E type MEDIA");
        static const uint8_t us[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
        EQ(dual_connect_0e_means_they_own(&src, us), 1, "0x0E peer MEDIA → give-up");
        EQ(dual_connect_0e_means_they_own(&src, peer), 0, "0x0E our MAC → not they");
    }

    // 6) anti-ping-pong: after give-up pause, auto-resume must not reclaim
    EQ(dual_connect_anti_ping_pong_blocks_reclaim(true), 1, "anti-ping-pong armed");
    EQ(dual_connect_should_reclaim_on_steal(true, true, true), 0, "anti-ping-pong blocks reclaim");
    EQ(dual_connect_state_on_unexpected_pause(true, true, true), DUAL_GIVE_UP, "stay GIVE_UP");

    // 7) post-reclaim Play gated on we_paused_for_steal
    EQ(dual_connect_allow_play_after_reclaim(true), 1, "Play ok after steal pause");
    EQ(dual_connect_allow_play_after_reclaim(false), 0, "Play gated without steal pause");

    // 8) 0x2E parse: count at [8], 8-byte records
    {
        // Two devices: iPhone + us
        uint8_t pkt[9 + 16] = {
            0x04, 0x00, 0x04, 0x00, 0x2E, 0x00, 0x00, 0x00,
            0x02, // count
            // rec0
            0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02,
            // rec1
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x03, 0x04
        };
        dual_connected_device_t devs[DUAL_CONN_DEV_MAX];
        int n = dual_connect_parse_0x2e(pkt, sizeof pkt, devs, DUAL_CONN_DEV_MAX);
        EQ(n, 2, "0x2E count 2");
        static const uint8_t m0[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        static const uint8_t m1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
        EQS(devs[0].mac, m0, 6, "0x2E mac0");
        EQ(devs[0].info1, 0x01, "0x2E info1");
        EQ(devs[0].info2, 0x02, "0x2E info2");
        EQS(devs[1].mac, m1, 6, "0x2E mac1");
    }

    // 9) session OWNS + 0x20 autocon builders (LibrePods takeOver pair)
    {
        uint8_t owns[DUAL_CTRL_FRAME_LEN];
        uint8_t ac[DUAL_CTRL_FRAME_LEN];
        dual_connect_build_owns_claim(owns);
        dual_connect_build_autocon(ac, 1);
        EQ(owns[6], DUAL_OWNS_ID, "session OWNS id");
        EQ(owns[7], DUAL_OWNS_CLAIM_VAL, "session OWNS=1");
        EQ(ac[6], DUAL_AUTOCON_ID, "session 0x20 id");
        EQ(ac[7], DUAL_AUTOCON_ON, "session 0x20=01");
        static const uint8_t hdr[6] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00 };
        EQS(owns, hdr, 6, "OWNS control hdr");
        EQS(ac, hdr, 6, "0x20 control hdr");
    }

    // 10) 0x10 smart-routing: see tests/aacp_smart_routing_test.c

    // 11) AACP 0x11 SetOwnershipToFalse — pause + we_paused; no hijack burst
    {
        static const char *k_false = "audioRoutingSetOwnershipToFalse";
        uint8_t pkt[80];
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 0x04; pkt[1] = 0x00; pkt[2] = 0x04; pkt[3] = 0x00;
        pkt[4] = DUAL_SR_RESP_OPCODE; pkt[5] = 0x00;
        // Display MAC AA:BB:CC:DD:EE:FF → packet bytes FF EE DD CC BB AA
        pkt[6] = 0xFF; pkt[7] = 0xEE; pkt[8] = 0xDD;
        pkt[9] = 0xCC; pkt[10] = 0xBB; pkt[11] = 0xAA;
        memcpy(pkt + 12, k_false, 31);
        size_t plen = 12 + 31;

        EQ(dual_connect_0x11_is_ownership_false(pkt, plen), 1, "0x11 own-false");
        EQ(dual_connect_0x11_is_reverse_banner(pkt, plen), 0, "0x11 no reverse");
        {
            uint8_t sender[6];
            EQ(dual_connect_parse_0x11_sender(pkt, plen, sender), 1, "0x11 sender ok");
            static const uint8_t peer[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
            EQS(sender, peer, 6, "0x11 MAC reversed");
        }

        dual_connect_0x11_decision_t d =
            dual_connect_decide_0x11(pkt, plen, true, true);
        EQ(d.recognized, 1, "0x11 recognized");
        EQ(d.pause_media, 1, "0x11 pause while USB wants sink");
        EQ(d.set_we_paused, 1, "0x11 we_paused (anti-ping-pong)");
        EQ(d.send_owns_giveup, 1, "0x11 OWNS=00");
        EQ(d.send_hijack, 0, "0x11 no immediate 0x10 hijack");
        EQ(d.state, DUAL_THEY_OWN, "0x11 → THEY_OWN");

        // Reclaim gated while we_paused; 0x10 hijack shares the same gate.
        EQ(dual_connect_should_reclaim_on_steal(true, true, d.set_we_paused), 0,
           "0x11 reclaim gated");
        EQ(dual_connect_anti_ping_pong_blocks_reclaim(d.set_we_paused), 1,
           "0x11 anti-ping-pong armed");

        // USB idle: still give up / we_paused, but no HID Pause.
        dual_connect_0x11_decision_t idle =
            dual_connect_decide_0x11(pkt, plen, false, false);
        EQ(idle.recognized, 1, "0x11 idle recognized");
        EQ(idle.pause_media, 0, "0x11 USB idle → no HID pause");
        EQ(idle.set_we_paused, 1, "0x11 idle still we_paused");
        EQ(idle.state, DUAL_THEY_OWN, "0x11 idle → THEY_OWN");
        EQ(dual_connect_should_reclaim_on_steal(false, false, idle.set_we_paused),
           0, "0x11 idle no reclaim");

        // Anti-ping-pong clears on USB streaming rising edge, then reclaim ok.
        EQ(dual_connect_should_clear_anti_ping_pong(true, true, false), 1,
           "clear on USB streaming 0→1");
        EQ(dual_connect_should_clear_anti_ping_pong(true, false, true), 1,
           "clear on USB speaker 0→1");
        EQ(dual_connect_should_clear_anti_ping_pong(true, false, false), 0,
           "idle open alt-set does not clear");
        EQ(dual_connect_should_clear_anti_ping_pong(false, true, true), 0,
           "nothing to clear");
        EQ(dual_connect_should_reclaim_on_steal(true, true, false), 1,
           "reclaim after anti-ping-pong clears");

        // ReverseBannerTapped: still give-up, no hijack (no reverse UI).
        memcpy(pkt + 12 + 31, DUAL_0X11_REVERSE_BANNER_KEY, 19);
        size_t rlen = plen + 19;
        EQ(dual_connect_0x11_is_reverse_banner(pkt, rlen), 1, "0x11 reverse");
        dual_connect_0x11_decision_t rev =
            dual_connect_decide_0x11(pkt, rlen, true, false);
        EQ(rev.reverse_banner, 1, "0x11 reverse flag");
        EQ(rev.send_hijack, 0, "0x11 reverse still no hijack");
        EQ(rev.state, DUAL_THEY_OWN, "0x11 reverse → THEY_OWN");

        // 0x10 hijack TX (same string, wrong opcode) is not 0x11 RX.
        pkt[4] = 0x10;
        EQ(dual_connect_0x11_is_ownership_false(pkt, rlen), 0, "0x10 ≠ 0x11");
        dual_connect_0x11_decision_t not11 =
            dual_connect_decide_0x11(pkt, rlen, true, true);
        EQ(not11.recognized, 0, "0x10 packet not ownership-false RX");
        EQ(not11.send_hijack, 0, "unrecognized 0x11 path does not hijack");
    }

    printf("dual_connect_policy_test: PASS "
           "(idle, steal→claim, status129, owns00, 0x0E, anti-ping-pong, "
           "play-gate, 0x2E, OWNS+0x20, 0x11 SetOwnershipToFalse; "
           "0x10 in aacp_smart_routing_test)\n");
    return 0;
}
