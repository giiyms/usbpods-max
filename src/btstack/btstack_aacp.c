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
#include <stddef.h>

#include "btstack.h"
#include "btstack_aacp.h"
#include "aacp_ear.h"
#include "aacp_mic_dec.h"
#include "../hid_consumer.h"
#include "../pico_w_led.h"

// A2DP streaming is the play-status signal (play_info.status is never updated).
bool check_is_streaming(void);

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
    uint8_t  data[64];
    uint16_t len;
} aacp_pkt_t;

#define AACP_TX_QUEUE_LEN 24
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
static uint8_t aacp_owns      = 0;    // 0 unk, 1 we own, 2 not
static uint8_t aacp_auto_conn = 0;    // id 0x20
static uint8_t aacp_allow_auto = 0;   // id 0x36
static uint8_t aacp_ear_en    = 0;    // id 0x0A
static uint8_t aacp_gestures  = 0;    // id 0x39
static uint8_t aacp_click_hold = 0;
static uint8_t aacp_crown_dir = 2;    // 0x1C default
static uint8_t aacp_auto_ans  = 2;
static uint8_t aacp_chime     = 50;
static uint8_t aacp_adapt_vol = 2;
static uint8_t aacp_sleep_det = 1;
static uint8_t aacp_listen_mask = 0x0F;
static uint8_t aacp_ca_duck_q8 = 255; // 255 = full (no duck)
static uint8_t aacp_ca_level  = 0;
static char    aacp_dev_name[32];
static char    aacp_dev_model[16];
static char    aacp_dev_serial[16];
static char    aacp_dev_fw[24];
static char    aacp_last19_hex[28];
static uint8_t aacp_last19_type = 0;
static uint8_t aacp_last19_bud  = 0;
static uint32_t aacp_imu_last_log_ms = 0;

// Auto-pause: AAP Definitions 0x00=InEar (ON HEAD), 0x01=Out, 0x02=InCase.
// Max 2 0x00/0x00 while worn is ON. Debounce: 200 ms off-head, 1500 ms
// on-head after we paused (Max sensor bounce L=0 R=0 after take-off).
// Apple Max pauses if you lift ONE cup (support.apple.com/108364). LibrePods
// linux default PauseWhenOneRemoved. Never HID Play/Pause toggle.
// Take-off Pause only if A2DP is streaming (resume_pending); put-on Play
// only if resume_pending. First packet: no HID.
static bool aacp_ear_known = false;
static bool aacp_ear_off_head = false;
static bool aacp_ear_pending = false;
static bool aacp_ear_pending_off = false;
static bool aacp_resume_pending = false;
static btstack_timer_source_t aacp_ear_timer;

static bool aacp_send_control_cmd(uint8_t id, uint8_t v1, uint8_t v2);
static bool aacp_prefs_need_persist = false;

static void aacp_status_reset(void) {
    aacp_bat_left = aacp_bat_right = aacp_bat_case = aacp_bat_head = 0xFF;
    aacp_ear_left = aacp_ear_right = 0xFF;
    aacp_noise_mode = 0;
    aacp_ca = 0;
    aacp_owns = 0;
    aacp_ca_duck_q8 = 255;
    aacp_ca_level = 0;
    aacp_ear_known = false;
    aacp_ear_off_head = false;
    aacp_ear_pending = false;
    aacp_resume_pending = false;
    aacp_last19_hex[0] = 0;
    aacp_last19_type = 0;
    aacp_last19_bud = 0;
    aacp_dev_name[0] = aacp_dev_model[0] = aacp_dev_serial[0] = aacp_dev_fw[0] = 0;
    btstack_run_loop_remove_timer(&aacp_ear_timer);
}

static void aacp_mark_prefs(void) {
    aacp_prefs_need_persist = true;
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
    host_prefs_t prefs;
    read_host_prefs_flash(&prefs);
    aacp_auto_ans    = prefs.auto_ans;
    aacp_chime       = prefs.chime;
    aacp_adapt_vol   = prefs.adapt_vol;
    aacp_sleep_det   = prefs.sleep_det;
    aacp_crown_dir   = prefs.crown_dir;
    aacp_listen_mask = prefs.listen_mask;
    aacp_auto_conn   = 1;   // LibrePods automaticConnectionEnabled default true
    aacp_ear_en      = 1;
    aacp_gestures    = 0x0F;
    aacp_click_hold  = 0x01;
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

static void aacp_queue_host_ownership(void) {
    // Replay on every new AACP session (LibrePods does this after handshake).
    // Frame: 04 00 04 00 09 00 [id] [data1] [data2] 00 00
    // Boolean mapping from Android sendControlCommand(Boolean): on=0x01 off=0x02.
    //
    // 0x20 Connect Automatically = 0x01 (enabled). Android default
    // automaticConnectionEnabled=true. This keeps THIS host. Do not send 0x02.
    // 0x36 Allow Auto Connect is NOT sent: Android comment on ALLOW_AUTO_CONNECT
    // is "AUTOMATIC_CONNECTION is the only one used".
    aacp_send_control_cmd(0x06, 0x01, 0x00);                 // Owns connection
    aacp_send_control_cmd(0x20, aacp_auto_conn ? aacp_auto_conn : 0x01, 0x00);
    aacp_send_control_cmd(0x0A, aacp_ear_en ? aacp_ear_en : 0x01, 0x00);
    aacp_send_control_cmd(0x39, aacp_gestures ? aacp_gestures : 0x0F, 0x00);
    aacp_send_control_cmd(0x1A, aacp_listen_mask ? aacp_listen_mask : 0x0F, 0x00);
    aacp_send_control_cmd(0x34, 0x01, 0x00);                 // Allow Off
    aacp_send_control_cmd(0x16, 0x01, 0x01);                 // ClickHold: noise both sides
    aacp_send_control_cmd(0x17, 0x00, 0x00);                 // DoubleClickInterval default
    aacp_send_control_cmd(0x18, 0x00, 0x00);                 // ClickHoldInterval default
    aacp_send_control_cmd(0x1C, aacp_crown_dir ? aacp_crown_dir : 0x02, 0x00);
    aacp_send_control_cmd(0x1E, aacp_auto_ans ? aacp_auto_ans : 0x02, 0x00);
    aacp_send_control_cmd(0x1F, aacp_chime <= 100 ? aacp_chime : 50, 0x00);
    aacp_send_control_cmd(0x26, aacp_adapt_vol ? aacp_adapt_vol : 0x02, 0x00);
    aacp_send_control_cmd(0x35, aacp_sleep_det ? aacp_sleep_det : 0x01, 0x00);
    aacp_owns = 1;
    aacp_click_hold = 0x01;
    printf("[AACP] session replay: own=1 auto-conn=0x%02x ear=on gestures=0x%02x "
           "listen=0x%02x allow-off=1 hold=noise crown=0x%02x autoans=0x%02x "
           "chime=%u adaptvol=0x%02x sleep=0x%02x\n",
           (unsigned) aacp_auto_conn, (unsigned) aacp_gestures,
           (unsigned) aacp_listen_mask, (unsigned) aacp_crown_dir,
           (unsigned) aacp_auto_ans, (unsigned) aacp_chime,
           (unsigned) aacp_adapt_vol, (unsigned) aacp_sleep_det);
    printf("[AACP] skip 0x05 Button Send Mode (LibrePods enum only, never sent)\n");
    printf("[AACP] skip 0x14/0x15 click modes (no packet values; host maps 0x0019)\n");
    printf("[AACP] skip 0x36 Allow Auto Connect (Android: 0x20 is the one used)\n");
    printf("[AACP] skip head gestures (no Classic AACP packet in LibrePods)\n");
}

static void aacp_queue_init_sequence(void) {
    aacp_tx_enqueue(aacp_handshake,             sizeof(aacp_handshake));
    aacp_tx_enqueue(aacp_set_feature_flags,     sizeof(aacp_set_feature_flags));
    aacp_tx_enqueue(aacp_request_notifications, sizeof(aacp_request_notifications));
    aacp_queue_host_ownership();
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
// Notification parse (LibrePods docs + working Android/Linux send/parse)
// Header: 04 00 04 00 [opcode u16le] [payload]
// Logging only on *change* for noisy reports — these handlers share A2DP.

static void aacp_dump_hex(const char *tag, const uint8_t *p, uint16_t n) {
    char line[120];
    uint16_t cap = n > 24 ? 24 : n;
    int o = snprintf(line, sizeof(line), "[AACP] %s n=%u:", tag, (unsigned) n);
    for (uint16_t i = 0; i < cap && o < (int) sizeof(line) - 4; i++) {
        o += snprintf(line + o, sizeof(line) - (size_t) o, " %02X", p[i]);
    }
    if (n > cap && o < (int) sizeof(line) - 4) {
        snprintf(line + o, sizeof(line) - (size_t) o, " …");
    }
    printf("%s\n", line);
}

static uint8_t aacp_ca_duck_from_level(uint8_t level) {
    // AAP Definitions 0x004B: 01/02 started (duck a lot), 03 stopped (restore),
    // 08/09 normal, intermediate interpolate. Do not touch the mic path.
    if (level == 0x01 || level == 0x02) return 38;   // ~15%
    if (level == 0x03 || level == 0x08 || level == 0x09) return 255;
    if (level >= 0x04 && level <= 0x07) {
        // 4..7 between ducked (38) and full (255)
        uint16_t t = (uint16_t)(level - 4); // 0..3
        return (uint8_t)(38 + (t * (255 - 38)) / 3);
    }
    return 255;
}

static void aacp_ear_timer_fired(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!aacp_ear_pending) return;
    aacp_ear_pending = false;
    bool now_off = aacp_ear_pending_off;
    if (!aacp_ear_known) {
        aacp_ear_known = true;
        aacp_ear_off_head = now_off;
        printf("[AACP] ear HID skip (first packet after connect, off-head=%u)\n",
               (unsigned) now_off);
        return; // first ear packet after connect: do not start Music
    }
    if (now_off == aacp_ear_off_head) {
        printf("[AACP] ear HID skip (no change, off-head=%u)\n", (unsigned) now_off);
        return;
    }
    aacp_ear_off_head = now_off;

    // Ear-detect enable 0x0A: 0x02 = off (LibrePods Boolean). Do not HID.
    if (aacp_ear_en == 0x02) {
        printf("[AACP] ear HID skip (ear detection disabled, off-head=%u)\n",
               (unsigned) now_off);
        return;
    }

    // 0x00 = on-head (AAP Definitions). Do not invert. Discrete Play/Pause
    // — never HID Play/Pause toggle (that starts Music when already paused).
    if (now_off) {
        // Take off: Pause only if media is playing, then remember to resume.
        // play_info.status is never updated; A2DP streaming is the playing signal.
        bool playing = check_is_streaming();
        if (playing) {
            hid_consumer_pause();
            aacp_resume_pending = true;
            printf("[AACP] ear OFF-HEAD → HID Pause (resume pending)\n");
        } else {
            printf("[AACP] ear OFF-HEAD skip HID Pause (A2DP not streaming)\n");
        }
    } else {
        // Put on: Play only if we paused on take-off. Stay paused otherwise.
        if (aacp_resume_pending) {
            hid_consumer_play();
            aacp_resume_pending = false;
            printf("[AACP] ear ON-HEAD → HID Play (resume, stable %u ms)\n",
                   (unsigned) AACP_EAR_RESUME_STABLE_MS);
        } else {
            printf("[AACP] ear ON-HEAD skip HID Play (stay paused, no resume_pending)\n");
        }
    }
}

static void aacp_ear_consider(uint8_t el, uint8_t er) {
    // 0x00 = In Ear = ON HEAD (AAP Definitions / linux eardetection.hpp).
    // Live Max 2 often reports L=0 R=0 while worn — that is ON, not a miss.
    // 0xFF unknown: do not pause.
    if (el == 0xFF || er == 0xFF) return;
    bool off = aacp_ear_is_off_head(el, er);
    if (aacp_ear_pending && aacp_ear_pending_off == off) return;
    if (aacp_ear_known && !aacp_ear_pending && aacp_ear_off_head == off) return;
    aacp_ear_pending = true;
    aacp_ear_pending_off = off;
    uint32_t delay = aacp_ear_commit_delay_ms(off, aacp_resume_pending);
    if (!off && aacp_resume_pending) {
        printf("[AACP] ear debounce %u ms → off-head=0 (resume wait, ignore bounce)\n",
               (unsigned) delay);
    } else {
        printf("[AACP] ear debounce %u ms → off-head=%u (either cup out)\n",
               (unsigned) delay, (unsigned) off);
    }
    btstack_run_loop_remove_timer(&aacp_ear_timer);
    btstack_run_loop_set_timer_handler(&aacp_ear_timer, aacp_ear_timer_fired);
    btstack_run_loop_set_timer(&aacp_ear_timer, delay);
    btstack_run_loop_add_timer(&aacp_ear_timer);
}

static void aacp_copy_cstr(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool aacp_field_is_text(const uint8_t *s, uint16_t n) {
    if (n == 0) return false;
    uint16_t printable = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) printable++;
        else printable++;
    }
    return printable >= 2;
}

static bool aacp_field_is_utf16le(const uint8_t *s, uint16_t n) {
    if (n < 4 || (n % 2) != 0) return false;
    for (uint16_t i = 0; i + 1 < n; i += 2) {
        if (s[i + 1] != 0) return false;
        if (s[i] < 0x20 || s[i] >= 0x7F) return false;
    }
    return true;
}

static void aacp_parse_device_info(const uint8_t *p, uint16_t n) {
    // AAP Definitions 0x001D: null-terminated strings after a short binary
    // prefix (example starts `02 d5 00 04 00` then "AirPods Pro"). Live Max 2
    // was parsed as C-strings and put garbage in name/model, serial=A3454,
    // fw='Apple Inc.'. Skip non-text; accept UTF-8 or UTF-16LE ASCII.
    // Order: name, model, manufacturer, serial, firmware.
    char fields[5][32];
    memset(fields, 0, sizeof(fields));
    uint8_t got = 0;
    uint16_t i = 0;
    while (i < n && got < 5) {
        while (i < n && p[i] == 0) i++;
        if (i >= n) break;
        uint16_t start = i;
        while (i < n && p[i] != 0) i++;
        uint16_t len = (uint16_t)(i - start);
        if (i < n && p[i] == 0) i++;

        char tmp[32];
        tmp[0] = 0;
        if (aacp_field_is_utf16le(&p[start], len)) {
            uint16_t o = 0;
            for (uint16_t k = 0; k + 1 < len && o < sizeof(tmp) - 1; k += 2) {
                tmp[o++] = (char) p[start + k];
            }
            tmp[o] = 0;
        } else if (aacp_field_is_text(&p[start], len)) {
            uint16_t o = len < sizeof(tmp) - 1 ? len : (uint16_t)(sizeof(tmp) - 1);
            memcpy(tmp, &p[start], o);
            tmp[o] = 0;
        } else {
            continue;
        }
        memcpy(fields[got], tmp, sizeof(fields[0]));
        got++;
    }
    aacp_copy_cstr(aacp_dev_name, sizeof(aacp_dev_name), fields[0]);
    aacp_copy_cstr(aacp_dev_model, sizeof(aacp_dev_model), fields[1]);
    aacp_copy_cstr(aacp_dev_serial, sizeof(aacp_dev_serial), fields[3]);
    aacp_copy_cstr(aacp_dev_fw, sizeof(aacp_dev_fw), fields[4]);
    printf("[AACP] device-info name='%s' model='%s' serial='%s' fw='%s'\n",
           aacp_dev_name, aacp_dev_model, aacp_dev_serial, aacp_dev_fw);
}

static void aacp_handle_stem(uint8_t type, uint8_t bud) {
    aacp_last19_type = type;
    aacp_last19_bud = bud;
    snprintf(aacp_last19_hex, sizeof(aacp_last19_hex), "%02X:%02X", type, bud);
    // StemAction.kt defaults: single=play/pause, double=next, triple=prev,
    // long=cycle noise. 0x16 ClickHold=noise means the headset already cycles
    // on hold — do not send a second 0x0D.
    switch (type) {
        case 0x05: hid_consumer_play_pause(); break;
        case 0x06: hid_consumer_next(); break;
        case 0x07: hid_consumer_prev(); break;
        case 0x08:
            if (aacp_click_hold != 0x01) {
                uint8_t cur = aacp_noise_mode ? aacp_noise_mode : 1;
                uint8_t mask = aacp_listen_mask ? aacp_listen_mask : 0x0F;
                uint8_t bits[4] = { 0x01, 0x02, 0x04, 0x08 };
                uint8_t next = cur;
                for (int i = 0; i < 4; i++) {
                    uint8_t try = (uint8_t)(((cur - 1 + 1 + i) % 4) + 1);
                    if (mask & bits[try - 1]) { next = try; break; }
                }
                aacp_set_noise_mode(next);
            }
            break;
        default:
            break;
    }
    printf("[AACP] stem type=0x%02X bud=0x%02X\n", type, bud);
}

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
            else h = level;
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
        uint8_t v2 = (n >= 3) ? p[2] : 0;
        switch (id) {
            case 0x06: aacp_owns = v; break;
            case 0x0A: aacp_ear_en = v; break;
            case 0x0D:
                if (v >= 1 && v <= 4 && v != aacp_noise_mode) {
                    aacp_noise_mode = v;
                    printf("[AACP] noise-control %u (1=off 2=anc 3=trans 4=adaptive)\n", (unsigned) v);
                }
                break;
            case 0x16: aacp_click_hold = v; (void) v2; break;
            case 0x1A: aacp_listen_mask = v; break;
            case 0x1C: aacp_crown_dir = v; break;
            case 0x1E: aacp_auto_ans = v; break;
            case 0x1F: aacp_chime = v; break;
            case 0x20: aacp_auto_conn = v; break;
            case 0x26: aacp_adapt_vol = v; break;
            case 0x28:
                if ((v == 1 || v == 2) && v != aacp_ca) {
                    aacp_ca = v;
                    printf("[AACP] conversation-awareness %s\n", v == 1 ? "on" : "off");
                }
                break;
            case 0x35: aacp_sleep_det = v; break;
            case 0x36: aacp_allow_auto = v; break;
            case 0x39: aacp_gestures = v; break;
            default:
                break;
        }
        return;
    }

    if (opcode == 0x0006 && n >= 1) {
        uint8_t el = p[0];
        uint8_t er = (n >= 2) ? p[1] : p[0];
        if (el != aacp_ear_left || er != aacp_ear_right) {
            aacp_ear_left = el;
            aacp_ear_right = er;
            printf("[AACP] ear L=%u R=%u (0=on-head 1=out 2=case) off-head=%u\n",
                   (unsigned) el, (unsigned) er,
                   (unsigned) aacp_ear_is_off_head(el, er));
            aacp_ear_consider(el, er);
        }
        return;
    }

    if (opcode == 0x0019) {
        if (size == 8) {
            uint8_t type = pkt[6];
            uint8_t bud  = pkt[7];
            if (type >= 0x05 && type <= 0x08 && (bud == 0x01 || bud == 0x02)) {
                aacp_handle_stem(type, bud);
                return;
            }
        }
        aacp_dump_hex("0x0019 unknown", pkt, size);
        snprintf(aacp_last19_hex, sizeof(aacp_last19_hex), "??");
        return;
    }

    if (opcode == 0x004B) {
        uint8_t level = 0;
        // `04 00 04 00 4B 00 02 00 01 [level]` — level at offset 9.
        if (size >= 10) level = pkt[9];
        else if (n >= 1) level = p[n - 1];
        aacp_ca_level = level;
        aacp_ca_duck_q8 = aacp_ca_duck_from_level(level);
        printf("[AACP] CA speaking level=0x%02X duck=%u/255 (speaker only)\n",
               (unsigned) level, (unsigned) aacp_ca_duck_q8);
        return;
    }

    if (opcode == 0x001D) {
        aacp_parse_device_info(p, n);
        return;
    }

    if (opcode == 0x0017) {
        // AAP Definitions.md "Received Head Tracking Sensor Data":
        // orient at packet offsets 43/45/47, accel at 51/53 (2 bytes each).
        // Incoming stream already runs; do not hex-dump (CDC drops).
        if (size >= 55) {
            int16_t o1 = (int16_t) little_endian_read_16(pkt, 43);
            int16_t o2 = (int16_t) little_endian_read_16(pkt, 45);
            int16_t o3 = (int16_t) little_endian_read_16(pkt, 47);
            int16_t ax = (int16_t) little_endian_read_16(pkt, 51);
            int16_t ay = (int16_t) little_endian_read_16(pkt, 53);
            uint32_t now = btstack_run_loop_get_time_ms();
            if (now - aacp_imu_last_log_ms >= 250) {
                aacp_imu_last_log_ms = now;
                printf("[AACP] IMU o=%d,%d,%d a=%d,%d\n",
                       (int) o1, (int) o2, (int) o3, (int) ax, (int) ay);
            }
        }
        return;
    }

    if (opcode == 0x0030 || opcode == 0x0031) {
        // BLE keys — Pico cannot join iCloud. Do not print key material.
        return;
    }

    if (opcode == 0x000D || opcode == 0x000E || opcode == 0x0010 || opcode == 0x0011) {
        // Audio source / smart routing. LibrePods hijack uses MAC-specific
        // blobs (sendMediaInformation + sendHijackRequest). Do not invent a reply.
        aacp_dump_hex(opcode == 0x000D ? "0x000D audio-src-req" :
                      opcode == 0x000E ? "0x000E audio-src-resp" :
                      opcode == 0x0010 ? "0x0010 smart-routing" :
                                         "0x0011 smart-routing-resp",
                      pkt, size);
        return;
    }

    if (opcode == 0x002D || opcode == 0x002E) {
        // LibrePods opcodes.md: 0x2D req / 0x2E list of connected devices.
        // Live dual-connect steal shows the iPhone MAC here. Dump only.
        aacp_dump_hex(opcode == 0x002E ? "0x002E connected-devices" :
                                         "0x002D connected-dev-req",
                      pkt, size);
        return;
    }

    if (opcode == 0x0058) {
        return; // non-audio 0x58 already filtered; ignore leftovers
    }

    aacp_dump_hex("unhandled", pkt, size);
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
uint8_t aacp_get_owns(void)       { return aacp_owns; }
uint8_t aacp_get_auto_conn(void)  { return aacp_auto_conn; }
uint8_t aacp_get_allow_auto(void) { return aacp_allow_auto; }
uint8_t aacp_get_ear_en(void)     { return aacp_ear_en; }
uint8_t aacp_get_gestures(void)   { return aacp_gestures; }
uint8_t aacp_get_click_hold(void) { return aacp_click_hold; }
uint8_t aacp_get_crown_dir(void)  { return aacp_crown_dir; }
uint8_t aacp_get_auto_ans(void)   { return aacp_auto_ans; }
uint8_t aacp_get_chime(void)      { return aacp_chime; }
uint8_t aacp_get_adapt_vol(void)  { return aacp_adapt_vol; }
uint8_t aacp_get_sleep_det(void)  { return aacp_sleep_det; }
uint8_t aacp_get_listen_mask(void){ return aacp_listen_mask; }
uint8_t aacp_get_ca_duck_q8(void) { return aacp_ca_duck_q8; }
uint8_t aacp_get_ca_speak_level(void) { return aacp_ca_level; }

void aacp_get_last19(uint8_t *type, uint8_t *bud) {
    if (type) *type = aacp_last19_type;
    if (bud) *bud = aacp_last19_bud;
}
const char *aacp_get_last19_hex(void) { return aacp_last19_hex; }
const char *aacp_get_dev_name(void)   { return aacp_dev_name; }
const char *aacp_get_dev_model(void)  { return aacp_dev_model; }
const char *aacp_get_dev_serial(void) { return aacp_dev_serial; }
const char *aacp_get_dev_fw(void)     { return aacp_dev_fw; }

void aacp_reassert_ownership(void) {
    if (aacp_cid == 0 || !aacp_connected) {
        printf("[AACP] reassert owns skipped (channel down)\n");
        return;
    }
    uint8_t auto_conn = aacp_auto_conn ? aacp_auto_conn : 0x01;
    aacp_send_control_cmd(0x06, 0x01, 0x00);
    aacp_send_control_cmd(0x20, auto_conn, 0x00);
    aacp_owns = 1;
    printf("[AACP] reassert owns=1 auto-conn=0x%02x (LibrePods takeOver)\n",
           (unsigned) auto_conn);
}

bool aacp_set_noise_mode(uint8_t mode) {
    if (mode < 1 || mode > 4) return false;
    if (!aacp_send_control_cmd(0x0D, mode, 0x00)) return false;
    aacp_noise_mode = mode;
    return true;
}

bool aacp_set_ca(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    if (!aacp_send_control_cmd(0x28, enable, 0x00)) return false;
    aacp_ca = enable;
    return true;
}

bool aacp_set_auto_conn(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    aacp_auto_conn = enable;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x20, enable, 0x00);
}

bool aacp_set_ear_en(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    aacp_ear_en = enable;
    return aacp_send_control_cmd(0x0A, enable, 0x00);
}

bool aacp_set_gestures(uint8_t mask) {
    aacp_gestures = mask;
    return aacp_send_control_cmd(0x39, mask, 0x00);
}

bool aacp_set_click_hold(uint8_t mode) {
    if (mode != 0x01 && mode != 0x05) return false;
    aacp_click_hold = mode;
    return aacp_send_control_cmd(0x16, mode, mode);
}

bool aacp_set_crown_dir(uint8_t dir) {
    if (dir != 1 && dir != 2) return false;
    aacp_crown_dir = dir;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x1C, dir, 0x00);
}

bool aacp_set_auto_ans(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    aacp_auto_ans = enable;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x1E, enable, 0x00);
}

bool aacp_set_chime(uint8_t vol) {
    if (vol > 100) vol = 100;
    aacp_chime = vol;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x1F, vol, 0x00);
}

bool aacp_set_adapt_vol(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    aacp_adapt_vol = enable;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x26, enable, 0x00);
}

bool aacp_set_sleep_det(uint8_t enable) {
    if (enable != 1 && enable != 2) return false;
    aacp_sleep_det = enable;
    aacp_mark_prefs();
    return aacp_send_control_cmd(0x35, enable, 0x00);
}

bool aacp_set_listen_mask(uint8_t mask) {
    aacp_listen_mask = mask;
    aacp_mark_prefs();
    bool ok = aacp_send_control_cmd(0x1A, mask, 0x00);
    aacp_send_control_cmd(0x34, (mask & 0x01) ? 0x01 : 0x02, 0x00);
    return ok;
}

bool aacp_rename(const char *name) {
    // LibrePods send path (Linux Rename::getPacket / Android createRenamePacket):
    // opcode 0x001A (not 0x001E — that id is AutoAnswer).
    // Packet: 04 00 04 00 1A 00 01 [size] 00 [name]
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n > 32) return false;
    if (aacp_cid == 0 || !aacp_connected) return false;
    uint8_t pkt[9 + 32];
    pkt[0] = 0x04; pkt[1] = 0x00; pkt[2] = 0x04; pkt[3] = 0x00;
    pkt[4] = 0x1A; pkt[5] = 0x00;
    pkt[6] = 0x01;
    pkt[7] = (uint8_t) n;
    pkt[8] = 0x00;
    memcpy(&pkt[9], name, n);
    aacp_copy_cstr(aacp_dev_name, sizeof(aacp_dev_name), name);
    return aacp_tx_enqueue(pkt, (uint16_t)(9 + n));
}

bool aacp_prefs_dirty(void) { return aacp_prefs_need_persist; }
void aacp_prefs_clear_dirty(void) { aacp_prefs_need_persist = false; }

void aacp_prefs_fill(uint8_t *auto_ans, uint8_t *chime, uint8_t *adapt_vol,
                     uint8_t *sleep_det, uint8_t *crown_dir, uint8_t *listen_mask) {
    if (auto_ans) *auto_ans = aacp_auto_ans;
    if (chime) *chime = aacp_chime;
    if (adapt_vol) *adapt_vol = aacp_adapt_vol;
    if (sleep_det) *sleep_det = aacp_sleep_det;
    if (crown_dir) *crown_dir = aacp_crown_dir;
    if (listen_mask) *listen_mask = aacp_listen_mask;
}

void aacp_prefs_apply_loaded(uint8_t auto_ans, uint8_t chime, uint8_t adapt_vol,
                             uint8_t sleep_det, uint8_t crown_dir, uint8_t listen_mask) {
    aacp_auto_ans = auto_ans;
    aacp_chime = chime;
    aacp_adapt_vol = adapt_vol;
    aacp_sleep_det = sleep_det;
    aacp_crown_dir = crown_dir;
    aacp_listen_mask = listen_mask;
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
