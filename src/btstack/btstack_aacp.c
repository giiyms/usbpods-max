// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Derived from librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AACP (Apple Accessory Protocol) channel — the AirPods hi-res mic transport.
//
// Opens an outgoing L2CAP channel on PSM 0x1001 over the existing ACL link,
// runs the fixed init sequence (handshake / SET_FEATURE_FLAGS /
// REQUEST_NOTIFICATIONS), then drives the microphone stream: a START opcode
// makes the AirPods push type-0x58 SDUs carrying AAC-ELD access units, which
// are demuxed here and handed to the decoder (aacp_mic_dec.c).
//
// All byte sequences and the 0x58 demux layout are ports of librepods
// (linux-rust branch, GPL-3.0) — see that project's aacp_audio.rs.
//
// LOGGING RULE: these handlers run in the BTstack run loop, where a burst of
// blocking log output has caused A2DP underruns. No per-packet logging on the
// audio path; telemetry is aggregated and printed at most once per second.
//

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_aacp.h"
#include "aacp_mic_dec.h"

#define AACP_PSM              0x1001
// Requested local MTU. BTstack clamps this to l2cap_max_mtu() =
// HCI_ACL_PAYLOAD_SIZE - 4 (currently 1691), which is what actually gets
// advertised. Measured audio SDUs top out around 354 bytes, well under that;
// if larger SDUs ever get dropped, raise HCI_ACL_PAYLOAD_SIZE in
// btstack_config.h rather than this constant.
#define AACP_LOCAL_MTU        4096
#define AACP_CONNECT_DELAY_MS 1500   // let A2DP settle before opening the channel
#define AACP_RETRY_DELAY_MS   3000   // delay between connect retries
#define AACP_MAX_ATTEMPTS     3      // initial attempt + retries per A2DP session

// Mic lifecycle: the ONLY trigger is the USB alt setting — main.c calls
// aacp_mic_start()/aacp_mic_stop() when the host opens/closes the recording
// stream. If the host opens the mic before the AACP channel is up (or the
// channel drops mid-capture), the start is remembered and fired as soon as
// the first control packet confirms the channel.

// --- Fixed init byte sequences (verbatim from librepods) ---

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

// --- Mic stream control byte sequences (verbatim from librepods) ---

static const uint8_t aacp_mic_start_bytes[] = {
    0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x01, 0x82, 0x00, 0x00, 0x00,
    0x04, 0x96, 0x00
};

static const uint8_t aacp_mic_stop_bytes[] = {
    0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x03, 0x01
};

// --- TX queue (ordered; one packet sent per CAN_SEND_NOW event) ---
// Packets are copied so callers can reuse stack/static templates.
typedef struct {
    uint8_t  data[32];
    uint16_t len;
} aacp_pkt_t;

#define AACP_TX_QUEUE_LEN 8
static aacp_pkt_t aacp_tx_queue[AACP_TX_QUEUE_LEN];
static uint8_t    aacp_tx_head = 0;   // next slot to send
static uint8_t    aacp_tx_count = 0;  // pending packets

static uint16_t   aacp_cid       = 0;
static bool       aacp_connected = false;
static bd_addr_t  aacp_addr;
static uint8_t    aacp_attempts  = 0;      // connect attempts this A2DP session
static bool       aacp_got_control = false; // first valid AACP control packet seen

static btstack_timer_source_t aacp_connect_timer;

// --- Mic stream state ---

// type-0x58 audio SDU layout: 22-byte header, then N x [ts:u32 LE][len:u8][au].
#define TYPE58_HEADER_LEN 22

static bool aacp_mic_running = false;   // START sent, STOP not yet sent
static bool aacp_mic_want    = false;   // host wants the mic (USB alt 1),
                                        // start deferred until channel is up
static btstack_timer_source_t aacp_mic_stats_timer;    // 1 Hz report

// Parsed notifications (0xFF = unknown). Reset on channel close.
static uint8_t aacp_bat_left  = 0xFF;
static uint8_t aacp_bat_right = 0xFF;
static uint8_t aacp_bat_case  = 0xFF;
static uint8_t aacp_bat_head  = 0xFF;
static uint8_t aacp_ear_left  = 0xFF;
static uint8_t aacp_ear_right = 0xFF;
static uint8_t aacp_noise_mode = 0;   // 0 unknown, 1-4 librepods ListeningMode
static uint8_t aacp_ca        = 0;    // 0 unknown, 1 on, 2 off

static void aacp_status_reset(void) {
    aacp_bat_left = aacp_bat_right = aacp_bat_case = aacp_bat_head = 0xFF;
    aacp_ear_left = aacp_ear_right = 0xFF;
    aacp_noise_mode = 0;
    aacp_ca = 0;
}

// RX statistics — cumulative since START, plus per-interval rates.
static struct {
    uint32_t sdu_total;     // audio SDUs seen
    uint32_t au_total;      // AUs demuxed
    uint32_t sdu_interval;  // audio SDUs this report interval
    uint32_t au_interval;   // AUs this report interval
    uint32_t au_bytes_interval;
    uint16_t au_min, au_max;        // AU size bounds (cumulative)
    uint16_t sdu_max;               // largest audio SDU seen (cumulative)
    uint16_t demux_short;           // SDUs whose trailer didn't parse cleanly
    uint32_t last_sdu_ms;           // for stall observation in the report
} mic_stats;

static void aacp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ------------------------------------------------------------------

void aacp_init(void) {
    aacp_cid         = 0;
    aacp_connected   = false;
    aacp_tx_head     = 0;
    aacp_tx_count    = 0;
    aacp_mic_running = false;
    aacp_status_reset();
}

static void aacp_do_connect(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (aacp_cid != 0) return;  // already connecting/connected
    aacp_attempts++;
    printf("[AACP] connecting to %s PSM 0x%04x (attempt %u/%u, local mtu %u) ...\n",
           bd_addr_to_str(aacp_addr), AACP_PSM, aacp_attempts, AACP_MAX_ATTEMPTS,
           btstack_min(AACP_LOCAL_MTU, l2cap_max_mtu()));
    uint8_t status = l2cap_create_channel(aacp_packet_handler, aacp_addr, AACP_PSM,
                                          AACP_LOCAL_MTU, &aacp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[AACP] l2cap_create_channel failed: 0x%02x\n", status);
        aacp_cid = 0;
    }
}

// Re-arm the connect timer for a retry, if attempts remain.
static void aacp_schedule_retry(void) {
    if (aacp_attempts >= AACP_MAX_ATTEMPTS) {
        printf("[AACP] giving up after %u attempts. If this persists: re-pair "
               "(BOOTSEL long press) so the AirPods pick up our DID record.\n", aacp_attempts);
        return;
    }
    printf("[AACP] retrying in %d ms\n", AACP_RETRY_DELAY_MS);
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    btstack_run_loop_set_timer_handler(&aacp_connect_timer, aacp_do_connect);
    btstack_run_loop_set_timer(&aacp_connect_timer, AACP_RETRY_DELAY_MS);
    btstack_run_loop_add_timer(&aacp_connect_timer);
}

void aacp_connect(bd_addr_t addr) {
    if (aacp_cid != 0) {
        printf("[AACP] connect ignored, channel already active (cid 0x%04x)\n", aacp_cid);
        return;
    }
    memcpy(aacp_addr, addr, sizeof(bd_addr_t));
    aacp_attempts    = 0;
    aacp_got_control = false;
    printf("[AACP] scheduling connect in %d ms\n", AACP_CONNECT_DELAY_MS);
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    btstack_run_loop_set_timer_handler(&aacp_connect_timer, aacp_do_connect);
    btstack_run_loop_set_timer(&aacp_connect_timer, AACP_CONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&aacp_connect_timer);
}

static void aacp_mic_teardown(void);   // fwd

void aacp_disconnect(void) {
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    aacp_mic_teardown();
    if (aacp_cid != 0) {
        l2cap_disconnect(aacp_cid);
    }
    aacp_cid       = 0;
    aacp_connected = false;
    aacp_tx_head   = 0;
    aacp_tx_count  = 0;
    aacp_status_reset();
}

bool aacp_is_connected(void) {
    return aacp_connected;
}

// ------------------------------------------------------------------
// TX queue

static bool aacp_tx_enqueue(const uint8_t *data, uint16_t len) {
    if (len > sizeof(aacp_tx_queue[0].data)) {
        printf("[AACP] TX packet too large (%u)\n", len);
        return false;
    }
    if (aacp_tx_count >= AACP_TX_QUEUE_LEN) {
        printf("[AACP] TX queue full, dropping %u-byte packet\n", len);
        return false;
    }
    uint8_t slot = (uint8_t)((aacp_tx_head + aacp_tx_count) % AACP_TX_QUEUE_LEN);
    memcpy(aacp_tx_queue[slot].data, data, len);
    aacp_tx_queue[slot].len  = len;
    aacp_tx_count++;
    if (aacp_cid != 0) l2cap_request_can_send_now_event(aacp_cid);
    return true;
}

static void aacp_handle_can_send_now(void) {
    if (aacp_tx_count == 0) return;
    aacp_pkt_t *p = &aacp_tx_queue[aacp_tx_head];
    uint8_t status = l2cap_send(aacp_cid, p->data, p->len);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[AACP] l2cap_send failed: 0x%02x (retrying)\n", status);
        l2cap_request_can_send_now_event(aacp_cid);  // retry same packet
        return;
    }
    printf("[AACP] sent %u bytes (%u queued)\n", p->len, (unsigned)(aacp_tx_count - 1));
    aacp_tx_head = (uint8_t)((aacp_tx_head + 1) % AACP_TX_QUEUE_LEN);
    aacp_tx_count--;
    if (aacp_tx_count > 0) l2cap_request_can_send_now_event(aacp_cid);
}

static void aacp_queue_init_sequence(void) {
    aacp_tx_enqueue(aacp_handshake,             sizeof(aacp_handshake));
    aacp_tx_enqueue(aacp_set_feature_flags,     sizeof(aacp_set_feature_flags));
    aacp_tx_enqueue(aacp_request_notifications, sizeof(aacp_request_notifications));
}

// ------------------------------------------------------------------
// Mic stream

// 1 Hz statistics report. Two short lines per second — safe log volume.
// Doubles as the stream stall watchdog: no audio SDU for >2000ms while
// capture is active → STOP + START to restart the stream.
static void aacp_mic_stats_report(btstack_timer_source_t *ts) {
    uint32_t now = btstack_run_loop_get_time_ms();
    // Covers BOTH stall cases: stream died mid-capture AND "START ignored"
    // (no SDU ever arrived — happens when START lands too soon after the
    // AACP handshake, e.g. host opened the mic before/during BT connect;
    // last_sdu_ms is initialized to the START time so the retry kicks in
    // 2s later either way).
    bool stalled = (now - mic_stats.last_sdu_ms) > 2000;
    uint32_t avg = mic_stats.au_interval ? (mic_stats.au_bytes_interval / mic_stats.au_interval) : 0;
    printf("[MIC] sdu/s=%lu au/s=%lu au min/avg/max=%u/%lu/%u sdu_max=%u total_au=%lu%s%s\n",
           (unsigned long) mic_stats.sdu_interval,
           (unsigned long) mic_stats.au_interval,
           mic_stats.au_min == 0xFFFF ? 0 : mic_stats.au_min,
           (unsigned long) avg,
           mic_stats.au_max,
           mic_stats.sdu_max,
           (unsigned long) mic_stats.au_total,
           mic_stats.demux_short ? " DEMUX_SHORT!" : "",
           stalled ? " STALLED>2s" : "");
    aacp_mic_dec_report();
    mic_stats.sdu_interval = 0;
    mic_stats.au_interval = 0;
    mic_stats.au_bytes_interval = 0;

    if (stalled && aacp_cid != 0 && aacp_connected) {
        printf("[MIC] stall watchdog: restarting stream (STOP + START)\n");
        aacp_tx_enqueue(aacp_mic_stop_bytes,  sizeof(aacp_mic_stop_bytes));
        aacp_tx_enqueue(aacp_mic_start_bytes, sizeof(aacp_mic_start_bytes));
        mic_stats.last_sdu_ms = now;   // give the restart a fresh 2s window
    }

    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
}

void aacp_mic_start(void) {
    aacp_mic_want = true;
    if (aacp_cid == 0 || !aacp_connected) {
        printf("[MIC] start deferred — AACP channel not open yet\n");
        return;
    }
    if (aacp_mic_running) return;
    aacp_mic_running = true;

    memset(&mic_stats, 0, sizeof(mic_stats));
    mic_stats.au_min = 0xFFFF;
    mic_stats.last_sdu_ms = btstack_run_loop_get_time_ms();

    // The decoder was opened at boot (thread context — opening it here, in
    // the BTstack callback, hung the system). Stats-only if boot init failed.
    if (!aacp_mic_dec_ready()) {
        printf("[MIC] WARNING: decoder unavailable, continuing stats-only\n");
    }
    aacp_mic_dec_reset_stats();
    aacp_mic_pcm_reset();   // fresh USB PCM session

    printf("[MIC] sending START (hi-res mic, AAC-ELD mono 64kHz expected)\n");
    aacp_tx_enqueue(aacp_mic_start_bytes, sizeof(aacp_mic_start_bytes));

    btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
    btstack_run_loop_set_timer_handler(&aacp_mic_stats_timer, aacp_mic_stats_report);
    btstack_run_loop_set_timer(&aacp_mic_stats_timer, 1000);
    btstack_run_loop_add_timer(&aacp_mic_stats_timer);
}

void aacp_mic_stop(void) {
    aacp_mic_want = false;
    if (!aacp_mic_running) return;
    printf("[MIC] sending STOP (total: %lu SDUs, %lu AUs)\n",
           (unsigned long) mic_stats.sdu_total, (unsigned long) mic_stats.au_total);
    if (aacp_cid != 0 && aacp_connected) {
        aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes));
    }
    aacp_mic_teardown();
}

bool aacp_mic_active(void) {
    return aacp_mic_running;
}

// Stop timers / clear state without sending anything (channel may be gone).
static void aacp_mic_teardown(void) {
    aacp_mic_running = false;
    btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
}

// --- type-0x58 audio SDU handling (port of librepods aacp_audio.rs) ---

// Predicate for 0x58 *audio* frames (subtype 0x0001).
static bool aacp_is_audio_sdu(const uint8_t *sdu, uint16_t size) {
    return size >= 8
        && sdu[0] == 0x04
        && sdu[2] == 0x04
        && little_endian_read_16(sdu, 4) == 0x0058
        && little_endian_read_16(sdu, 6) == 0x0001;
}

// Walk the sub-frames of one 0x58 audio SDU: 22-byte header, then
// N x [timestamp:u32 LE][au_len:u8][AU bytes]. Each AU goes to the AAC-ELD
// decoder; size statistics feed the 1 Hz report.
static void aacp_handle_audio_sdu(const uint8_t *sdu, uint16_t size) {
    mic_stats.sdu_total++;
    mic_stats.sdu_interval++;
    if (size > mic_stats.sdu_max) mic_stats.sdu_max = size;
    mic_stats.last_sdu_ms = btstack_run_loop_get_time_ms();

    uint16_t off = TYPE58_HEADER_LEN;
    while ((uint32_t) off + 5 <= size) {
        uint8_t au_len = sdu[off + 4];
        uint16_t start = off + 5;
        uint32_t end   = (uint32_t) start + au_len;
        if (end > size) {          // truncated trailer — count and stop
            mic_stats.demux_short++;
            break;
        }
        aacp_mic_dec_decode(&sdu[start], au_len);
        mic_stats.au_total++;
        mic_stats.au_interval++;
        mic_stats.au_bytes_interval += au_len;
        if (au_len < mic_stats.au_min) mic_stats.au_min = au_len;
        if (au_len > mic_stats.au_max) mic_stats.au_max = au_len;
        off = (uint16_t) end;
    }
}

// ------------------------------------------------------------------
// Notification parse (librepods docs — see AACP-FEATURES.md)
// Header: 04 00 04 00 [opcode u16le] [payload]
// Battery (opcode 0x0004): AAP Definitions —
//   [count] ([component] 01 [level] [status] 01)*
//   component: Right=0x02 Left=0x04 Case=0x08
// Control (opcode 0x0009):  [identifier] [data1] [data2] ...
//   0x0D ListeningMode: 1=Off 2=ANC 3=Transparency 4=Adaptive
//   0x28 Conversation Detect: 1=enabled 2=disabled
// Ear detection (opcode 0x0006): first two payload bytes are left/right
//   placement in the layout used by librepods / AAP Definitions.
// Logging only on *change* — these handlers share the A2DP run loop.

static void aacp_handle_control(const uint8_t *pkt, uint16_t size) {
    uint16_t opcode = little_endian_read_16(pkt, 4);
    const uint8_t *p = pkt + 6;
    uint16_t n = (uint16_t)(size - 6);

    if (opcode == 0x0004 && n >= 1) {
        uint8_t count = p[0];
        uint16_t off = 1;
        uint8_t l = aacp_bat_left, r = aacp_bat_right, c = aacp_bat_case, h = aacp_bat_head;
        for (uint8_t i = 0; i < count && (uint16_t)(off + 5) <= n; i++, off += 5) {
            uint8_t comp  = p[off];
            uint8_t level = p[off + 2];
            if (comp == 0x02) r = level;
            else if (comp == 0x04) l = level;
            else if (comp == 0x08) c = level;
            else h = level;   // AirPods Max may report a single headset component
        }
        if (l != aacp_bat_left || r != aacp_bat_right || c != aacp_bat_case || h != aacp_bat_head) {
            aacp_bat_left = l; aacp_bat_right = r; aacp_bat_case = c; aacp_bat_head = h;
            printf("[AACP] battery L=%u R=%u case=%u headset=%u\n",
                   (unsigned) l, (unsigned) r, (unsigned) c, (unsigned) h);
        }
        return;
    }

    if (opcode == 0x0009 && n >= 2) {
        uint8_t id = p[0];
        uint8_t v  = p[1];
        if (id == 0x0D && v >= 1 && v <= 4 && v != aacp_noise_mode) {
            aacp_noise_mode = v;
            printf("[AACP] noise-control %u (1=off 2=anc 3=trans 4=adaptive)\n", (unsigned) v);
        } else if (id == 0x28 && (v == 1 || v == 2) && v != aacp_ca) {
            aacp_ca = v;
            printf("[AACP] conversation-awareness %s\n", v == 1 ? "on" : "off");
        }
        return;
    }

    if (opcode == 0x0006 && n >= 1) {
        uint8_t el = p[0];
        uint8_t er = (n >= 2) ? p[1] : p[0];
        if (el != aacp_ear_left || er != aacp_ear_right) {
            aacp_ear_left = el;
            aacp_ear_right = er;
            printf("[AACP] ear L=%u R=%u\n", (unsigned) el, (unsigned) er);
        }
        return;
    }

    if (opcode == 0x004B && n >= 1) {
        // Speaking/duck event — layout varies; do not decode levels here.
        // Config on/off is control identifier 0x28 (above).
        return;
    }
}

static bool aacp_send_control_cmd(uint8_t id, uint8_t v1, uint8_t v2) {
    if (aacp_cid == 0 || !aacp_connected) return false;
    uint8_t pkt[] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00,
        id, v1, v2, 0x00, 0x00
    };
    return aacp_tx_enqueue(pkt, sizeof(pkt));
}

void aacp_get_battery(uint8_t *left, uint8_t *right, uint8_t *case_bat, uint8_t *headset) {
    if (left) *left = aacp_bat_left;
    if (right) *right = aacp_bat_right;
    if (case_bat) *case_bat = aacp_bat_case;
    if (headset) *headset = aacp_bat_head;
}

void aacp_get_ear(uint8_t *left, uint8_t *right) {
    if (left) *left = aacp_ear_left;
    if (right) *right = aacp_ear_right;
}

uint8_t aacp_get_noise_mode(void) { return aacp_noise_mode; }
uint8_t aacp_get_ca(void)         { return aacp_ca; }

bool aacp_set_noise_mode(uint8_t mode) {
    if (mode < 1 || mode > 4) return false;
    if (!aacp_send_control_cmd(0x0D, mode, 0x00)) return false;
    aacp_noise_mode = mode;   // optimistic; AirPods echo via 0x0009
    return true;
}

bool aacp_set_ca(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    if (!aacp_send_control_cmd(0x28, enable, 0x00)) return false;
    aacp_ca = enable;
    return true;
}

// ------------------------------------------------------------------

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
                        aacp_schedule_retry();
                        return;
                    }
                    aacp_cid       = cid;
                    aacp_connected = true;
                    printf("[AACP] channel OPEN cid 0x%04x remote_mtu %u — starting handshake\n",
                           cid, l2cap_event_channel_opened_get_remote_mtu(packet));
                    aacp_queue_init_sequence();
                    break;
                }
                case L2CAP_EVENT_CAN_SEND_NOW:
                    aacp_handle_can_send_now();
                    break;
                case L2CAP_EVENT_CHANNEL_CLOSED:
                    printf("[AACP] channel CLOSED\n");
                    aacp_cid       = 0;
                    aacp_connected = false;
                    aacp_tx_head   = 0;
                    aacp_tx_count  = 0;
                    aacp_mic_teardown();
                    aacp_status_reset();
                    break;
                default:
                    break;
            }
            break;

        case L2CAP_DATA_PACKET:
            // Audio SDUs first (hot path once the mic runs — stats only, no logs).
            if (aacp_is_audio_sdu(packet, size)) {
                aacp_handle_audio_sdu(packet, size);
                break;
            }

            // Control packets: 04 00 04 00 data header + u16le opcode.
            // Not logged per packet — the AirPods send occasional multi-
            // hundred-byte info bursts that must not stall the run loop.
            if (size >= 6 && packet[0] == 0x04 && packet[1] == 0x00 &&
                packet[2] == 0x04 && packet[3] == 0x00) {
                aacp_handle_control(packet, size);
                if (!aacp_got_control) {
                    aacp_got_control = true;
                    printf("[AACP] control channel confirmed (first packet)\n");
                    // Host already holds the mic open (USB alt 1)? Fire the
                    // deferred START now that the channel is confirmed.
                    if (aacp_mic_want && !aacp_mic_running) {
                        printf("[MIC] firing deferred START\n");
                        aacp_mic_start();
                    }
                }
            }
            break;

        default:
            break;
    }
}
