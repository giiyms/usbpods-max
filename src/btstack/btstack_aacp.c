//
// AACP (Apple Accessory Protocol) channel — Phase 1.
//
// Establishes an outgoing L2CAP channel on PSM 0x1001 over the existing ACL
// link to the AirPods, then runs the fixed init sequence and hex-dumps every
// received packet. Success criterion (HANDOFF §7 Phase 1): AACP control
// packets such as BATTERY_INFO (0x04) start arriving.
//
// Byte sequences are taken verbatim from librepods (linux-rust) — see HANDOFF §3.2.
//

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_aacp.h"

#define AACP_PSM              0x1001
#define AACP_LOCAL_MTU        4096   // 0x58 audio SDU can exceed 1KB (HANDOFF §3.1)
#define AACP_CONNECT_DELAY_MS 1500   // let A2DP settle before opening the channel

// --- Fixed init byte sequences (HANDOFF §3.2) ---

// 1) Handshake: 16 bytes sent raw (the 04 00 04 00 data header is part of it).
static const uint8_t aacp_handshake[] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 2) SET_FEATURE_FLAGS (opcode 0x004D): 04 00 04 00 | 4D 00 | FF 00 00 00 00 00 00 00
static const uint8_t aacp_set_feature_flags[] = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00,
    0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 3) REQUEST_NOTIFICATIONS (opcode 0x000F): 04 00 04 00 | 0F 00 | FF FF FF FF
static const uint8_t aacp_request_notifications[] = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF
};

// --- Init send sequence (ordered; sent one packet per CAN_SEND_NOW) ---
typedef struct {
    const uint8_t *data;
    uint16_t       len;
} aacp_pkt_t;

static aacp_pkt_t aacp_init_seq[3];
static uint8_t    aacp_init_seq_len = 0;
static uint8_t    aacp_init_seq_idx = 0;

static uint16_t   aacp_cid       = 0;
static bool       aacp_connected = false;
static bd_addr_t  aacp_addr;

static btstack_timer_source_t aacp_connect_timer;

static void aacp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ------------------------------------------------------------------

void aacp_init(void) {
    aacp_cid          = 0;
    aacp_connected    = false;
    aacp_init_seq_len = 0;
    aacp_init_seq_idx = 0;
}

static void aacp_do_connect(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (aacp_cid != 0) return;  // already connecting/connected
    printf("[AACP] connecting to %s PSM 0x%04x ...\n", bd_addr_to_str(aacp_addr), AACP_PSM);
    uint8_t status = l2cap_create_channel(aacp_packet_handler, aacp_addr, AACP_PSM,
                                          AACP_LOCAL_MTU, &aacp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[AACP] l2cap_create_channel failed: 0x%02x\n", status);
        aacp_cid = 0;
    }
}

void aacp_connect(bd_addr_t addr) {
    if (aacp_cid != 0) {
        printf("[AACP] connect ignored, channel already active (cid 0x%04x)\n", aacp_cid);
        return;
    }
    memcpy(aacp_addr, addr, sizeof(bd_addr_t));
    printf("[AACP] scheduling connect in %d ms\n", AACP_CONNECT_DELAY_MS);
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    btstack_run_loop_set_timer_handler(&aacp_connect_timer, aacp_do_connect);
    btstack_run_loop_set_timer(&aacp_connect_timer, AACP_CONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&aacp_connect_timer);
}

void aacp_disconnect(void) {
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    if (aacp_cid != 0) {
        l2cap_disconnect(aacp_cid);
    }
    aacp_cid       = 0;
    aacp_connected = false;
}

bool aacp_is_connected(void) {
    return aacp_connected;
}

// ------------------------------------------------------------------

static void aacp_queue_init_sequence(void) {
    aacp_init_seq[0].data = aacp_handshake;            aacp_init_seq[0].len = sizeof(aacp_handshake);
    aacp_init_seq[1].data = aacp_set_feature_flags;    aacp_init_seq[1].len = sizeof(aacp_set_feature_flags);
    aacp_init_seq[2].data = aacp_request_notifications; aacp_init_seq[2].len = sizeof(aacp_request_notifications);
    aacp_init_seq_len = 3;
    aacp_init_seq_idx = 0;
}

static void aacp_send_next(void) {
    if (aacp_init_seq_idx < aacp_init_seq_len) {
        l2cap_request_can_send_now_event(aacp_cid);
    }
}

static void aacp_handle_can_send_now(void) {
    if (aacp_init_seq_idx >= aacp_init_seq_len) return;
    aacp_pkt_t *p = &aacp_init_seq[aacp_init_seq_idx];
    uint8_t status = l2cap_send(aacp_cid, (uint8_t *) p->data, p->len);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[AACP] l2cap_send failed at step %u: 0x%02x (retrying)\n", aacp_init_seq_idx, status);
        l2cap_request_can_send_now_event(aacp_cid);  // retry same step
        return;
    }
    printf("[AACP] sent init step %u/%u (%u bytes)\n",
           aacp_init_seq_idx + 1, aacp_init_seq_len, p->len);
    aacp_init_seq_idx++;
    aacp_send_next();
}

static void aacp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case L2CAP_EVENT_CHANNEL_OPENED: {
                    uint8_t  status = l2cap_event_channel_opened_get_status(packet);
                    uint16_t cid    = l2cap_event_channel_opened_get_local_cid(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("[AACP] channel open FAILED: status 0x%02x\n", status);
                        aacp_cid       = 0;
                        aacp_connected = false;
                        return;
                    }
                    aacp_cid       = cid;
                    aacp_connected = true;
                    printf("[AACP] channel OPEN cid 0x%04x remote_mtu %u — starting handshake\n",
                           cid, l2cap_event_channel_opened_get_remote_mtu(packet));
                    aacp_queue_init_sequence();
                    aacp_send_next();
                    break;
                }
                case L2CAP_EVENT_CAN_SEND_NOW:
                    aacp_handle_can_send_now();
                    break;
                case L2CAP_EVENT_CHANNEL_CLOSED:
                    printf("[AACP] channel CLOSED\n");
                    aacp_cid       = 0;
                    aacp_connected = false;
                    break;
                default:
                    break;
            }
            break;

        case L2CAP_DATA_PACKET:
            // Phase 1: dump everything. Phase 2 will branch here on 0x58 audio SDUs.
            printf("[AACP] RX %u bytes:\n", size);
            printf_hexdump(packet, size);
            break;

        default:
            break;
    }
}
