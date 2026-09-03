// SPDX-License-Identifier: GPL-3.0-only
//
// USBPods Max control plane — see control.h and web/PROTOCOL.md.
//
// CDC RX and HID SET_REPORT run in the USB timer IRQ: they only parse and
// latch. BTstack / flash run from control_main_task() in the main loop.
//

#include "control.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "pico/cyw43_arch.h"

#include "mic_gain.h"
#include "pico_w_led.h"
#include "btstack/btstack_avdtp_source.h"
#include "btstack/btstack_hci.h"
#include "btstack/btstack_aacp.h"

// ---- latched requests (USB IRQ → main loop) ----
static volatile bool req_pair = false;
static volatile bool req_disconnect = false;
static volatile bool req_reconnect = false;
static volatile uint8_t req_slot = 0;     // 1 or 2, 0 = none
static volatile uint8_t req_noise = 0;    // 1-4, 0 = none
static volatile uint8_t req_ca = 0;       // 1 on, 2 off, 0 = none

static bool cdc_was_connected = false;
static char cdc_line[80];
static uint8_t cdc_line_len = 0;

static void latch_pair(void)       { req_pair = true; }
static void latch_disconnect(void) { req_disconnect = true; }
static void latch_reconnect(void)  { req_reconnect = true; }
static void latch_slot(uint8_t s)  { if (s == 1 || s == 2) req_slot = s; }
static void latch_noise(uint8_t m) { if (m >= 1 && m <= 4) req_noise = m; }
static void latch_ca(uint8_t v)    { if (v == 1 || v == 2) req_ca = v; }

void control_fill_status(uint8_t *buf, uint16_t len) {
    if (len < CTRL_REPORT_LEN) return;
    memset(buf, 0, len);

    uint8_t bat_l = 0xFF, bat_r = 0xFF, bat_c = 0xFF, bat_h = 0xFF;
    uint8_t ear_l = 0xFF, ear_r = 0xFF;
    aacp_get_battery(&bat_l, &bat_r, &bat_c, &bat_h);
    aacp_get_ear(&ear_l, &ear_r);

    uint8_t flags = 0;
    if (get_a2dp_connected_flag()) flags |= 0x01;
    if (aacp_is_connected())       flags |= 0x02;
    if (aacp_mic_active())         flags |= 0x04;

    buf[0]  = CTRL_RSP_STATUS;
    buf[1]  = flags;
    buf[2]  = mic_gain_get_db();
    buf[3]  = read_uint8_last_flash();
    buf[4]  = bat_l;
    buf[5]  = bat_r;
    buf[6]  = bat_c;
    buf[7]  = bat_h;
    buf[8]  = aacp_get_noise_mode();
    buf[9]  = ear_l;
    buf[10] = ear_r;
    buf[11] = aacp_get_ca();
    buf[12] = 0;     // fw minor of this control protocol
    buf[13] = 1;     // fw major
    buf[14] = mic_gain_get_mute() ? 1 : 0;

    bd_addr_t *addr = get_device_addr();
    if (addr) memcpy(&buf[16], *addr, 6);
}

static void print_status_human(void) {
    uint8_t buf[CTRL_REPORT_LEN];
    control_fill_status(buf, sizeof(buf));

    const char *noise = "unknown";
    switch (buf[8]) {
        case 1: noise = "off"; break;
        case 2: noise = "anc"; break;
        case 3: noise = "transparency"; break;
        case 4: noise = "adaptive"; break;
        default: break;
    }
    const char *ca = "unknown";
    if (buf[11] == 1) ca = "on";
    else if (buf[11] == 2) ca = "off";

    printf("USBPods Max status\n");
    printf("  A2DP %s  AACP %s  mic %s  slot %u\n",
           (buf[1] & 0x01) ? "up" : "down",
           (buf[1] & 0x02) ? "up" : "down",
           (buf[1] & 0x04) ? "on" : "off",
           (unsigned) buf[3]);
    printf("  mic gain %u dB%s\n", (unsigned) buf[2], buf[14] ? " (UAC mute)" : "");
    printf("  battery L=%u R=%u case=%u headset=%u  (255=unknown)\n",
           (unsigned) buf[4], (unsigned) buf[5], (unsigned) buf[6], (unsigned) buf[7]);
    printf("  noise-control %s  CA %s  ear L=%u R=%u\n",
           noise, ca, (unsigned) buf[9], (unsigned) buf[10]);

    // Machine line for the web page (Web Serial).
    printf("@STATUS a2dp=%u aacp=%u mic=%u gain=%u slot=%u mute=%u "
           "bat_l=%u bat_r=%u bat_c=%u bat_h=%u noise=%u ear_l=%u ear_r=%u ca=%u "
           "addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
           (buf[1] & 0x01) ? 1 : 0,
           (buf[1] & 0x02) ? 1 : 0,
           (buf[1] & 0x04) ? 1 : 0,
           (unsigned) buf[2], (unsigned) buf[3], (unsigned) buf[14],
           (unsigned) buf[4], (unsigned) buf[5], (unsigned) buf[6], (unsigned) buf[7],
           (unsigned) buf[8], (unsigned) buf[9], (unsigned) buf[10], (unsigned) buf[11],
           buf[16], buf[17], buf[18], buf[19], buf[20], buf[21]);
}

static void print_help(void) {
    printf("USBPods Max console\n");
    printf("  h / help              this help\n");
    printf("  s / status            connection, gain, battery, noise control\n");
    printf("  g / gain [0-24]       get or set mic boost dB (persisted)\n");
    printf("  p / pair              pair (long-press BOOTSEL equivalent)\n");
    printf("  x / disconnect        drop A2DP, keep pairing\n");
    printf("  r / reconnect         reconnect last device\n");
    printf("  1 / 2 / slot N        pairing slot\n");
    printf("  n / anc off|anc|trans|adaptive\n");
    printf("  ca on|off             conversation awareness (AACP 0x09/0x28)\n");
    printf("Host Bluetooth must NOT own the headset or A2DP goes silent.\n");
}

static int icmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned ca = (unsigned char) *a++;
        unsigned cb = (unsigned char) *b++;
        if (tolower(ca) != tolower(cb)) return 1;
    }
    return (*a || *b) ? 1 : 0;
}

static uint8_t parse_noise_token(const char *t) {
    if (!t) return 0;
    if (!icmp(t, "off") || !icmp(t, "1")) return 1;
    if (!icmp(t, "anc") || !icmp(t, "nc") || !icmp(t, "2")) return 2;
    if (!icmp(t, "trans") || !icmp(t, "transparency") || !icmp(t, "3")) return 3;
    if (!icmp(t, "adaptive") || !icmp(t, "adapt") || !icmp(t, "4")) return 4;
    return 0;
}

static void handle_line(char *line) {
    // trim
    while (*line == ' ' || *line == '\t') line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    if (*line == 0) return;

    char *arg = line;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) {
        *arg++ = 0;
        while (*arg == ' ' || *arg == '\t') arg++;
    }

    if (!icmp(line, "h") || !icmp(line, "help") || !icmp(line, "?")) {
        print_help();
        return;
    }
    if (!icmp(line, "s") || !icmp(line, "status")) {
        print_status_human();
        return;
    }
    if (!icmp(line, "g") || !icmp(line, "gain")) {
        if (*arg) {
            int v = 0;
            for (const char *p = arg; *p; p++) {
                if (*p < '0' || *p > '9') { v = -1; break; }
                v = v * 10 + (*p - '0');
            }
            if (v < 0 || v > MIC_GAIN_DB_MAX) {
                printf("gain: use 0..%u dB\n", (unsigned) MIC_GAIN_DB_MAX);
            } else {
                mic_gain_set_db((uint8_t) v);
                printf("[GAIN] %u dB (soft-clip limiter on)\n", (unsigned) v);
                printf("@GAIN %u\n", (unsigned) v);
            }
        } else {
            printf("gain %u dB\n", (unsigned) mic_gain_get_db());
            printf("@GAIN %u\n", (unsigned) mic_gain_get_db());
        }
        return;
    }
    if (!icmp(line, "p") || !icmp(line, "pair")) {
        printf("pairing: put the headset in pairing mode\n");
        latch_pair();
        return;
    }
    if (!icmp(line, "x") || !icmp(line, "disconnect")) {
        latch_disconnect();
        printf("disconnect requested\n");
        return;
    }
    if (!icmp(line, "r") || !icmp(line, "reconnect")) {
        latch_reconnect();
        printf("reconnect requested\n");
        return;
    }
    if (!icmp(line, "1") || (!icmp(line, "slot") && *arg == '1' && arg[1] == 0)) {
        latch_slot(1);
        printf("slot 1\n");
        return;
    }
    if (!icmp(line, "2") || (!icmp(line, "slot") && *arg == '2' && arg[1] == 0)) {
        latch_slot(2);
        printf("slot 2\n");
        return;
    }
    if (!icmp(line, "slot")) {
        if (*arg == '1' || *arg == '2') {
            latch_slot((uint8_t)(*arg - '0'));
            printf("slot %c\n", *arg);
        } else {
            printf("slot %u\n", (unsigned) read_uint8_last_flash());
        }
        return;
    }
    if (!icmp(line, "n") || !icmp(line, "anc") || !icmp(line, "noise")) {
        uint8_t m = parse_noise_token(arg);
        if (!m) {
            printf("anc: off | anc | trans | adaptive\n");
        } else {
            latch_noise(m);
            printf("noise-control %s\n", arg);
        }
        return;
    }
    if (!icmp(line, "ca")) {
        if (!icmp(arg, "on") || !icmp(arg, "1")) {
            latch_ca(1);
            printf("conversation awareness on\n");
        } else if (!icmp(arg, "off") || !icmp(arg, "2")) {
            latch_ca(2);
            printf("conversation awareness off\n");
        } else {
            printf("ca: on | off\n");
        }
        return;
    }
    printf("unknown command. h for help.\n");
}

void control_init(void) {
    cdc_was_connected = false;
    cdc_line_len = 0;
}

static void hid_send_status(void) {
    if (!tud_hid_ready()) return;
    uint8_t buf[CTRL_REPORT_LEN];
    control_fill_status(buf, sizeof(buf));
    tud_hid_report(0, buf, sizeof(buf));
}

static void handle_hid_cmd(uint8_t const *buf, uint16_t len) {
    if (len < 1) return;
    switch (buf[0]) {
        case CTRL_CMD_GET_STATUS:
            hid_send_status();
            break;
        case CTRL_CMD_SET_GAIN:
            if (len >= 2) {
                uint8_t db = buf[1];
                if (db > MIC_GAIN_DB_MAX) db = MIC_GAIN_DB_MAX;
                mic_gain_set_db(db);
                hid_send_status();
            }
            break;
        case CTRL_CMD_PAIR:
            latch_pair();
            hid_send_status();
            break;
        case CTRL_CMD_DISCONNECT:
            latch_disconnect();
            hid_send_status();
            break;
        case CTRL_CMD_RECONNECT:
            latch_reconnect();
            hid_send_status();
            break;
        case CTRL_CMD_SET_SLOT:
            if (len >= 2) {
                latch_slot(buf[1]);
                hid_send_status();
            }
            break;
        case CTRL_CMD_SET_NOISE:
            if (len >= 2) {
                latch_noise(buf[1]);
                hid_send_status();
            }
            break;
        case CTRL_CMD_SET_CA:
            if (len >= 2) {
                latch_ca(buf[1]);
                hid_send_status();
            }
            break;
        default:
            break;
    }
}

void control_usb_task(void) {
    bool now = tud_cdc_connected();
    if (now && !cdc_was_connected) {
        printf("\nUSBPods Max console. Type h for help.\n");
        print_status_human();
        cdc_line_len = 0;
    }
    cdc_was_connected = now;
    if (!now) return;

    while (tud_cdc_available()) {
        char c;
        if (tud_cdc_read(&c, 1) != 1) break;
        if (c == '\r') continue;
        if (c == '\b' || c == 127) {
            if (cdc_line_len > 0) cdc_line_len--;
            continue;
        }
        if (c == '\n') {
            cdc_line[cdc_line_len] = 0;
            cdc_line_len = 0;
            handle_line(cdc_line);
            continue;
        }
        if (cdc_line_len + 1 < sizeof(cdc_line)) {
            cdc_line[cdc_line_len++] = c;
        }
    }
}

void control_main_task(void) {
    mic_gain_persist_task();

    async_context_t *ctx = cyw43_arch_async_context();

    if (req_pair) {
        req_pair = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_disconnect_and_scan();
        async_context_release_lock(ctx);
    }
    if (req_disconnect) {
        req_disconnect = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_disconnect_keep_pairing();
        aacp_disconnect();
        async_context_release_lock(ctx);
    }
    if (req_reconnect) {
        req_reconnect = false;
        async_context_acquire_lock_blocking(ctx);
        a2dp_source_reconnect();
        async_context_release_lock(ctx);
    }
    if (req_slot) {
        uint8_t s = req_slot;
        req_slot = 0;
        uint8_t cur = read_uint8_last_flash();
        if (s != cur) {
            write_uint8_last_flash(s);
            if (s == 2) {
                set_led_mode_off();
                set_led_mode_triple_blink();
            } else {
                set_led_mode_off();
                set_led_mode_double_blink();
            }
            async_context_acquire_lock_blocking(ctx);
            get_link_keys();
            async_context_release_lock(ctx);
        }
    }
    if (req_noise) {
        uint8_t m = req_noise;
        req_noise = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_noise_mode(m);
        async_context_release_lock(ctx);
        printf("[AACP] noise-control set %u %s\n", (unsigned) m, ok ? "queued" : "FAIL (AACP down?)");
    }
    if (req_ca) {
        uint8_t v = req_ca;
        req_ca = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_ca(v);
        async_context_release_lock(ctx);
        printf("[AACP] conversation-awareness %s %s\n",
               v == 1 ? "on" : "off", ok ? "queued" : "FAIL (AACP down?)");
    }
}

//--------------------------------------------------------------------+
// TinyUSB HID callbacks
//--------------------------------------------------------------------+

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    if (reqlen < CTRL_REPORT_LEN) return 0;
    control_fill_status(buffer, CTRL_REPORT_LEN);
    return CTRL_REPORT_LEN;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    handle_hid_cmd(buffer, bufsize);
}
