// SPDX-License-Identifier: GPL-3.0-only
//
// USBPods Max control plane — see control.h and web/PROTOCOL.md.
//
// BTstack / flash run from control_main_task() in the main loop.
// TinyUSB HID/CDC live in control_usb.c (tusb.h must not appear here:
// TinyUSB hid.h and BTstack btstack_hid.h both define hid_report_type_t).
//

#include "control.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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
static volatile uint8_t req_crown = 0;
static volatile uint8_t req_autoans = 0;
static volatile uint8_t req_chime = 0xFF;
static volatile uint8_t req_adapt = 0;
static volatile uint8_t req_sleep = 0;
static volatile uint8_t req_listen = 0;
static volatile uint8_t req_ear_en = 0;
static volatile uint8_t req_gestures = 0xFF;
static volatile uint8_t req_hold = 0;
static volatile uint8_t req_autoconn = 0;
static char req_rename[33];
static volatile bool req_rename_pending = false;

void control_request_pair(void)       { req_pair = true; }
void control_request_disconnect(void) { req_disconnect = true; }
void control_request_reconnect(void)  { req_reconnect = true; }
void control_request_slot(uint8_t s)  { if (s == 1 || s == 2) req_slot = s; }
void control_request_noise(uint8_t m) { if (m >= 1 && m <= 4) req_noise = m; }
void control_request_ca(uint8_t v)    { if (v == 1 || v == 2) req_ca = v; }

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

    uint8_t flags2 = 0;
    if (aacp_get_owns() == 1)        flags2 |= 0x01;
    if (aacp_get_ca_duck_q8() < 200) flags2 |= 0x02;
    if (aacp_get_auto_conn() == 1)   flags2 |= 0x04;
    if (aacp_get_ear_en() == 1)      flags2 |= 0x08;

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
    buf[12] = CTRL_PROTO_MINOR;
    buf[13] = CTRL_PROTO_MAJOR;
    buf[14] = mic_gain_get_mute() ? 1 : 0;
    buf[15] = flags2;

    bd_addr_t *addr = get_device_addr();
    if (addr) memcpy(&buf[16], *addr, 6);

    uint8_t t19 = 0, b19 = 0;
    aacp_get_last19(&t19, &b19);
    buf[22] = aacp_get_chime();
    buf[23] = aacp_get_crown_dir();
    buf[24] = aacp_get_listen_mask();
    buf[25] = aacp_get_gestures();
    buf[26] = aacp_get_click_hold();
    buf[27] = t19;
    buf[28] = b19;
    buf[29] = aacp_get_auto_ans();
    buf[30] = aacp_get_adapt_vol();
    buf[31] = aacp_get_sleep_det();
}

void control_print_status_human(void) {
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
    printf("  A2DP %s  AACP %s  mic %s  slot %u  owns %s  duck %s\n",
           (buf[1] & 0x01) ? "up" : "down",
           (buf[1] & 0x02) ? "up" : "down",
           (buf[1] & 0x04) ? "on" : "off",
           (unsigned) buf[3],
           (buf[15] & 0x01) ? "yes" : "no",
           (buf[15] & 0x02) ? "active" : "idle");
    printf("  mic gain %u dB%s\n", (unsigned) buf[2], buf[14] ? " (UAC mute)" : "");
    printf("  battery headset=%u  L=%u R=%u case=%u  (255=unknown; Max 2 uses headset)\n",
           (unsigned) buf[7], (unsigned) buf[4], (unsigned) buf[5], (unsigned) buf[6]);
    printf("  noise-control %s  CA %s  ear L=%u R=%u\n",
           noise, ca, (unsigned) buf[9], (unsigned) buf[10]);
    printf("  name '%s' model '%s' serial '%s' fw '%s'\n",
           aacp_get_dev_name(), aacp_get_dev_model(),
           aacp_get_dev_serial(), aacp_get_dev_fw());
    printf("  last 0x0019 %s\n", aacp_get_last19_hex()[0] ? aacp_get_last19_hex() : "(none)");

    printf("@STATUS a2dp=%u aacp=%u mic=%u gain=%u slot=%u mute=%u "
           "bat_l=%u bat_r=%u bat_c=%u bat_h=%u noise=%u ear_l=%u ear_r=%u ca=%u "
           "owns=%u duck=%u autocon=%u allowauto=%u earen=%u gestures=%u hold=%u "
           "crown=%u autoans=%u chime=%u adaptvol=%u sleep=%u listen=%u "
           "last19=%s vol=%u name=%s model=%s serial=%s fw=%s findmy=unsupported "
           "addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
           (buf[1] & 0x01) ? 1 : 0,
           (buf[1] & 0x02) ? 1 : 0,
           (buf[1] & 0x04) ? 1 : 0,
           (unsigned) buf[2], (unsigned) buf[3], (unsigned) buf[14],
           (unsigned) buf[4], (unsigned) buf[5], (unsigned) buf[6], (unsigned) buf[7],
           (unsigned) buf[8], (unsigned) buf[9], (unsigned) buf[10], (unsigned) buf[11],
           aacp_get_owns(), aacp_get_ca_duck_q8() < 200 ? 1 : 0,
           aacp_get_auto_conn(), aacp_get_allow_auto(),
           aacp_get_ear_en(), aacp_get_gestures(), aacp_get_click_hold(),
           aacp_get_crown_dir(), aacp_get_auto_ans(), aacp_get_chime(),
           aacp_get_adapt_vol(), aacp_get_sleep_det(), aacp_get_listen_mask(),
           aacp_get_last19_hex()[0] ? aacp_get_last19_hex() : "-",
           (unsigned) get_bt_volume(),
           aacp_get_dev_name()[0] ? aacp_get_dev_name() : "-",
           aacp_get_dev_model()[0] ? aacp_get_dev_model() : "-",
           aacp_get_dev_serial()[0] ? aacp_get_dev_serial() : "-",
           aacp_get_dev_fw()[0] ? aacp_get_dev_fw() : "-",
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
    printf("  rename <str>          AACP opcode 0x001A (LibrePods send path)\n");
    printf("  crown default|reverse\n");
    printf("  autoans on|off        AutoAnswer 0x1E\n");
    printf("  chime [0-100]\n");
    printf("  adaptvol on|off       Adaptive Volume 0x26\n");
    printf("  sleep on|off          Sleep Detection 0x35\n");
    printf("  listen <mask>         ListeningModeConfigs 0x1A hex/dec\n");
    printf("  ear on|off            Ear Detection 0x0A\n");
    printf("  gestures <mask>       Raw Gestures 0x39\n");
    printf("  hold noise|siri       ClickHoldMode 0x16\n");
    printf("  autocon on|off        Connect Automatically 0x20\n");
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

static int parse_onoff(const char *t) {
    if (!t) return 0;
    if (!icmp(t, "on") || !icmp(t, "1")) return 1;
    if (!icmp(t, "off") || !icmp(t, "2")) return 2;
    return 0;
}

static int parse_u8(const char *t, int maxv) {
    if (!t || !*t) return -1;
    int v = 0;
    int hex = (t[0] == '0' && (t[1] == 'x' || t[1] == 'X'));
    const char *p = hex ? t + 2 : t;
    if (!*p) return -1;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return -1;
        v = hex ? (v * 16 + d) : (v * 10 + d);
        if (v > maxv) return -1;
    }
    return v;
}

void control_process_line(char *line) {
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
        control_print_status_human();
        return;
    }
    if (!icmp(line, "g") || !icmp(line, "gain")) {
        if (*arg) {
            int v = parse_u8(arg, MIC_GAIN_DB_MAX);
            if (v < 0) {
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
        control_request_pair();
        return;
    }
    if (!icmp(line, "x") || !icmp(line, "disconnect")) {
        control_request_disconnect();
        printf("disconnect requested\n");
        return;
    }
    if (!icmp(line, "r") || !icmp(line, "reconnect")) {
        control_request_reconnect();
        printf("reconnect requested\n");
        return;
    }
    if (!icmp(line, "1") || (!icmp(line, "slot") && *arg == '1' && arg[1] == 0)) {
        control_request_slot(1);
        printf("slot 1\n");
        return;
    }
    if (!icmp(line, "2") || (!icmp(line, "slot") && *arg == '2' && arg[1] == 0)) {
        control_request_slot(2);
        printf("slot 2\n");
        return;
    }
    if (!icmp(line, "slot")) {
        if (*arg == '1' || *arg == '2') {
            control_request_slot((uint8_t)(*arg - '0'));
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
            control_request_noise(m);
            printf("noise-control %s\n", arg);
        }
        return;
    }
    if (!icmp(line, "ca")) {
        int v = parse_onoff(arg);
        if (!v) printf("ca: on | off\n");
        else { control_request_ca((uint8_t) v); printf("conversation awareness %s\n", arg); }
        return;
    }
    if (!icmp(line, "rename")) {
        if (!*arg) { printf("rename <name>\n"); return; }
        strncpy(req_rename, arg, sizeof(req_rename) - 1);
        req_rename[sizeof(req_rename) - 1] = 0;
        req_rename_pending = true;
        printf("rename '%s'\n", req_rename);
        return;
    }
    if (!icmp(line, "crown")) {
        if (!icmp(arg, "reverse") || !icmp(arg, "1")) req_crown = 1;
        else if (!icmp(arg, "default") || !icmp(arg, "2")) req_crown = 2;
        else { printf("crown: default | reverse\n"); return; }
        printf("crown %s\n", req_crown == 1 ? "reverse" : "default");
        return;
    }
    if (!icmp(line, "autoans")) {
        int v = parse_onoff(arg);
        if (!v) printf("autoans: on | off\n");
        else req_autoans = (uint8_t) v;
        return;
    }
    if (!icmp(line, "chime")) {
        int v = parse_u8(arg, 100);
        if (v < 0) printf("chime 0..100\n");
        else req_chime = (uint8_t) v;
        return;
    }
    if (!icmp(line, "adaptvol")) {
        int v = parse_onoff(arg);
        if (!v) printf("adaptvol: on | off\n");
        else req_adapt = (uint8_t) v;
        return;
    }
    if (!icmp(line, "sleep")) {
        int v = parse_onoff(arg);
        if (!v) printf("sleep: on | off\n");
        else req_sleep = (uint8_t) v;
        return;
    }
    if (!icmp(line, "listen")) {
        int v = parse_u8(arg, 255);
        if (v < 0) printf("listen <mask> (0x0F = all four Max 2 modes)\n");
        else req_listen = (uint8_t) v;
        return;
    }
    if (!icmp(line, "ear")) {
        int v = parse_onoff(arg);
        if (!v) printf("ear: on | off\n");
        else req_ear_en = (uint8_t) v;
        return;
    }
    if (!icmp(line, "gestures")) {
        int v = parse_u8(arg, 255);
        if (v < 0) printf("gestures <mask> (0x0F = single|double|triple|long)\n");
        else req_gestures = (uint8_t) v;
        return;
    }
    if (!icmp(line, "hold")) {
        if (!icmp(arg, "noise") || !icmp(arg, "1")) req_hold = 0x01;
        else if (!icmp(arg, "siri") || !icmp(arg, "5")) req_hold = 0x05;
        else printf("hold: noise | siri\n");
        return;
    }
    if (!icmp(line, "autocon")) {
        int v = parse_onoff(arg);
        if (!v) printf("autocon: on | off  (LibrePods 0x20; 0x36 is not sent)\n");
        else req_autoconn = (uint8_t) v;
        return;
    }
    printf("unknown command. h for help.\n");
}

bool control_handle_hid_cmd(uint8_t const *buf, uint16_t len) {
    if (len < 1) return false;
    switch (buf[0]) {
        case CTRL_CMD_GET_STATUS:
            return true;
        case CTRL_CMD_SET_GAIN:
            if (len >= 2) {
                uint8_t db = buf[1];
                if (db > MIC_GAIN_DB_MAX) db = MIC_GAIN_DB_MAX;
                mic_gain_set_db(db);
                return true;
            }
            break;
        case CTRL_CMD_PAIR:
            control_request_pair();
            return true;
        case CTRL_CMD_DISCONNECT:
            control_request_disconnect();
            return true;
        case CTRL_CMD_RECONNECT:
            control_request_reconnect();
            return true;
        case CTRL_CMD_SET_SLOT:
            if (len >= 2) { control_request_slot(buf[1]); return true; }
            break;
        case CTRL_CMD_SET_NOISE:
            if (len >= 2) { control_request_noise(buf[1]); return true; }
            break;
        case CTRL_CMD_SET_CA:
            if (len >= 2) { control_request_ca(buf[1]); return true; }
            break;
        case CTRL_CMD_SET_CROWN:
            if (len >= 2) { req_crown = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_AUTOANS:
            if (len >= 2) { req_autoans = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_CHIME:
            if (len >= 2) { req_chime = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_ADAPT:
            if (len >= 2) { req_adapt = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_SLEEP:
            if (len >= 2) { req_sleep = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_LISTEN:
            if (len >= 2) { req_listen = buf[1]; return true; }
            break;
        case CTRL_CMD_RENAME:
            if (len >= 3) {
                uint8_t n = buf[1];
                if (n > 32) n = 32;
                if ((uint16_t)(2 + n) > len) n = (uint8_t)(len - 2);
                memcpy(req_rename, &buf[2], n);
                req_rename[n] = 0;
                req_rename_pending = true;
                return true;
            }
            break;
        case CTRL_CMD_SET_EAR_DET:
            if (len >= 2) { req_ear_en = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_GESTURES:
            if (len >= 2) { req_gestures = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_HOLD:
            if (len >= 2) { req_hold = buf[1]; return true; }
            break;
        case CTRL_CMD_SET_AUTOCONN:
            if (len >= 2) { req_autoconn = buf[1]; return true; }
            break;
        default:
            break;
    }
    return false;
}

static void persist_if_idle(void) {
    bool allow = !aacp_mic_active() && !aacp_is_connected();
    if (!allow) return;
    if (aacp_prefs_dirty()) {
        host_prefs_t prefs;
        aacp_prefs_fill(&prefs.auto_ans, &prefs.chime, &prefs.adapt_vol,
                        &prefs.sleep_det, &prefs.crown_dir, &prefs.listen_mask);
        if (write_host_settings_flash(mic_gain_get_db(), &prefs)) {
            printf("[NVS] persisted gain=%u autoans=%u chime=%u adapt=%u sleep=%u crown=%u listen=0x%02x\n",
                   (unsigned) mic_gain_get_db(), prefs.auto_ans, prefs.chime,
                   prefs.adapt_vol, prefs.sleep_det, prefs.crown_dir, prefs.listen_mask);
            aacp_prefs_clear_dirty();
            mic_gain_persist_ack();
        } else {
            printf("[NVS] persist FAILED\n");
        }
        return;
    }
    mic_gain_persist_task(true);
}

void control_main_task(void) {
    persist_if_idle();

    async_context_t *ctx = cyw43_arch_async_context();

    if (req_pair) {
        req_pair = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_reclaim_hold_set(true);
        avdtp_disconnect_and_scan();
        async_context_release_lock(ctx);
    }
    if (req_disconnect) {
        req_disconnect = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_reclaim_hold_set(true);
        avdtp_disconnect_keep_pairing();
        aacp_disconnect();
        async_context_release_lock(ctx);
    }
    if (req_reconnect) {
        req_reconnect = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_reclaim_hold_set(false);
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
        uint8_t m = req_noise; req_noise = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_noise_mode(m);
        async_context_release_lock(ctx);
        printf("[AACP] noise-control set %u %s\n", (unsigned) m, ok ? "queued" : "FAIL (AACP down?)");
    }
    if (req_ca) {
        uint8_t v = req_ca; req_ca = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_ca(v);
        async_context_release_lock(ctx);
        printf("[AACP] conversation-awareness %s %s\n",
               v == 1 ? "on" : "off", ok ? "queued" : "FAIL (AACP down?)");
    }
    if (req_crown) {
        uint8_t v = req_crown; req_crown = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_crown_dir(v);
        async_context_release_lock(ctx);
        printf("[AACP] crown %s %s\n", v == 1 ? "reverse" : "default", ok ? "queued" : "FAIL");
    }
    if (req_autoans) {
        uint8_t v = req_autoans; req_autoans = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_auto_ans(v);
        async_context_release_lock(ctx);
        printf("[AACP] auto-answer %s %s\n", v == 1 ? "on" : "off", ok ? "queued" : "FAIL");
    }
    if (req_chime != 0xFF) {
        uint8_t v = req_chime; req_chime = 0xFF;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_chime(v);
        async_context_release_lock(ctx);
        printf("[AACP] chime %u %s\n", (unsigned) v, ok ? "queued" : "FAIL");
    }
    if (req_adapt) {
        uint8_t v = req_adapt; req_adapt = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_adapt_vol(v);
        async_context_release_lock(ctx);
        printf("[AACP] adaptive-volume %s %s\n", v == 1 ? "on" : "off", ok ? "queued" : "FAIL");
    }
    if (req_sleep) {
        uint8_t v = req_sleep; req_sleep = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_sleep_det(v);
        async_context_release_lock(ctx);
        printf("[AACP] sleep-detect %s %s\n", v == 1 ? "on" : "off", ok ? "queued" : "FAIL");
    }
    if (req_listen) {
        uint8_t v = req_listen; req_listen = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_listen_mask(v);
        async_context_release_lock(ctx);
        printf("[AACP] listen-mask 0x%02x %s\n", (unsigned) v, ok ? "queued" : "FAIL");
    }
    if (req_ear_en) {
        uint8_t v = req_ear_en; req_ear_en = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_ear_en(v);
        async_context_release_lock(ctx);
        printf("[AACP] ear-detect %s %s\n", v == 1 ? "on" : "off", ok ? "queued" : "FAIL");
    }
    if (req_gestures != 0xFF) {
        uint8_t v = req_gestures; req_gestures = 0xFF;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_gestures(v);
        async_context_release_lock(ctx);
        printf("[AACP] gestures 0x%02x %s\n", (unsigned) v, ok ? "queued" : "FAIL");
    }
    if (req_hold) {
        uint8_t v = req_hold; req_hold = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_click_hold(v);
        async_context_release_lock(ctx);
        printf("[AACP] click-hold 0x%02x %s\n", (unsigned) v, ok ? "queued" : "FAIL");
    }
    if (req_autoconn) {
        uint8_t v = req_autoconn; req_autoconn = 0;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_set_auto_conn(v);
        async_context_release_lock(ctx);
        printf("[AACP] connect-automatically %s %s\n", v == 1 ? "on" : "off", ok ? "queued" : "FAIL");
    }
    if (req_rename_pending) {
        req_rename_pending = false;
        async_context_acquire_lock_blocking(ctx);
        bool ok = aacp_rename(req_rename);
        async_context_release_lock(ctx);
        printf("[AACP] rename '%s' %s\n", req_rename, ok ? "queued" : "FAIL");
    }
}