// Host-side AACP dump replay: dual-connect policy over sequenced frames.
// Policy only — not Apple radio / DID / Windows UAC.
// gcc -I. -O2 -o /tmp/aacp_dump_replay_test tests/aacp_dump_replay_test.c && /tmp/aacp_dump_replay_test
// gcc -I. -O2 -o /tmp/aacp_dump_replay_test tests/aacp_dump_replay_test.c && /tmp/aacp_dump_replay_test tests/fixtures/dual_connect_iphone_steal.aacp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "src/btstack/dual_connect_policy.h"
#include "src/btstack/aacp_smart_routing.h"

#define PKT_MAX 512
#define LINE_MAX 4096

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

#define EQS(a, b, n, msg) do { if (memcmp((a), (b), (n)) != 0) { \
    fprintf(stderr, "FAIL: %s\n", msg); exit(1); } } while (0)

/* Built-in copy of tests/fixtures/dual_connect_iphone_steal.aacp (fallback). */
static const char k_builtin_iphone_steal[] =
    "OUR_MAC 11:22:33:44:55:66\n"
    "USB_SPK_OPEN 1\n"
    "USB_STREAMING 1\n"
    "EXPECT reclaim=1 hijack=1 paused=0 state=WE_OWN_STREAMING\n"
    "HEX 04 00 04 00 0E 00 FF EE DD CC BB AA 02\n"
    "EXPECT 0e_they_own=1 reclaim=1 hijack=1 paused=0 state=WE_OWN_STREAMING\n"
    "HEX 04 00 04 00 2E 00 00 00 02 AA BB CC DD EE FF 01 02 11 22 33 44 55 66 03 04\n"
    "EXPECT 0x2e_count=2 reclaim=1 hijack=1 paused=0\n"
    "HEX 04 00 04 00 11 00 FF EE DD CC BB AA 61 75 64 69 6F 52 6F 75 74 69 6E 67 53 65 74 4F 77 6E 65 72 73 68 69 70 54 6F 46 61 6C 73 65\n"
    "EXPECT 0x11 recognized=1 pause=1 giveup=1 hijack=0 reclaim=0 paused=1 state=THEY_OWN sender=AA:BB:CC:DD:EE:FF\n"
    "EXPECT reclaim=0 hijack=0 paused=1 state=THEY_OWN\n"
    "USB_STREAMING 0\n"
    "EXPECT paused=1 reclaim=0 hijack=0 state=THEY_OWN\n"
    "USB_STREAMING 1\n"
    "EXPECT paused=0 reclaim=1 hijack=1 state=WE_OWN_STREAMING\n"
    "STEAL\n"
    "EXPECT state=RECLAIMING reclaim=1 hijack=1 paused=0\n";

static const char *k_default_fixture =
    "tests/fixtures/dual_connect_iphone_steal.aacp";

typedef struct {
    const char *source;
    int line_no;
    uint8_t our_mac[6];
    bool usb_spk_open;
    bool is_usb_streaming;
    bool we_paused;
    dual_connect_state_t state;
    bool last_0e_ok;
    bool last_0e_they_own;
    int last_0x2e_count;
    dual_connect_0x11_decision_t last_0x11;
    uint8_t last_0x11_sender[6];
    bool last_0x11_sender_ok;
    uint8_t last_pkt[PKT_MAX];
    size_t last_pkt_len;
} replay_t;

static void fail_at(const replay_t *r, const char *fmt, ...) {
    fprintf(stderr, "FAIL: %s:%d: ", r->source, r->line_no);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static const char *state_name(dual_connect_state_t s) {
    switch (s) {
        case DUAL_USB_IDLE: return "USB_IDLE";
        case DUAL_WE_OWN_STREAMING: return "WE_OWN_STREAMING";
        case DUAL_THEY_OWN: return "THEY_OWN";
        case DUAL_RECLAIMING: return "RECLAIMING";
        case DUAL_GIVE_UP: return "GIVE_UP";
        default: return "?";
    }
}

static bool parse_state_name(const char *s, dual_connect_state_t *out) {
    if (!s || !out) return false;
    if (!strncmp(s, "DUAL_", 5)) s += 5;
    if (!strcmp(s, "USB_IDLE")) { *out = DUAL_USB_IDLE; return true; }
    if (!strcmp(s, "WE_OWN_STREAMING")) { *out = DUAL_WE_OWN_STREAMING; return true; }
    if (!strcmp(s, "THEY_OWN")) { *out = DUAL_THEY_OWN; return true; }
    if (!strcmp(s, "RECLAIMING")) { *out = DUAL_RECLAIMING; return true; }
    if (!strcmp(s, "GIVE_UP")) { *out = DUAL_GIVE_UP; return true; }
    return false;
}

static void replay_init(replay_t *r, const char *source) {
    memset(r, 0, sizeof(*r));
    r->source = source;
    r->last_0x2e_count = -1;
    static const uint8_t k_self[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    memcpy(r->our_mac, k_self, 6);
}

static void refresh_state_from_usb(replay_t *r) {
    if (r->we_paused) {
        if (r->state != DUAL_THEY_OWN && r->state != DUAL_GIVE_UP)
            r->state = DUAL_THEY_OWN;
        return;
    }
    if (dual_connect_usb_wants_sink(r->usb_spk_open, r->is_usb_streaming))
        r->state = DUAL_WE_OWN_STREAMING;
    else
        r->state = DUAL_USB_IDLE;
}

static void maybe_clear_ppp(replay_t *r, bool stream_rising, bool spk_rising) {
    if (dual_connect_should_clear_anti_ping_pong(r->we_paused, stream_rising,
                                                 spk_rising)) {
        r->we_paused = false;
    }
    refresh_state_from_usb(r);
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_hex_token(const char *t, size_t n) {
    if (n >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t += 2;
        n -= 2;
    }
    if (n == 0 || n > 2) return false;
    for (size_t i = 0; i < n; i++) {
        if (hex_nibble((unsigned char)t[i]) < 0) return false;
    }
    return true;
}

static int parse_hex_byte(const char *t, size_t n) {
    if (n >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t += 2;
        n -= 2;
    }
    if (n == 1) return hex_nibble((unsigned char)t[0]);
    if (n == 2) {
        int hi = hex_nibble((unsigned char)t[0]);
        int lo = hex_nibble((unsigned char)t[1]);
        if (hi < 0 || lo < 0) return -1;
        return (hi << 4) | lo;
    }
    return -1;
}

static bool parse_bool_token(const char *t, bool *out) {
    if (!t || !out) return false;
    if (t[0] == '1' && t[1] == '\0') { *out = true; return true; }
    if (t[0] == '0' && t[1] == '\0') { *out = false; return true; }
    return false;
}

static bool parse_mac_text(const char *s, uint8_t mac[6]) {
    unsigned a[6];
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6 ||
        sscanf(s, "%02x %02x %02x %02x %02x %02x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)a[i];
        return true;
    }
    return false;
}

static void strip_comment(char *line) {
    char *h = strchr(line, '#');
    if (h) *h = '\0';
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
}

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Strip optional CDC prefix "[AACP] tag n=N:" — require full frames (no …). */
static const char *strip_cdc_prefix(replay_t *r, const char *s) {
    s = skip_ws(s);
    if (strncmp(s, "[AACP]", 6) != 0) return s;
    const char *ellipsis = strstr(s, "…");
    const char *dots = strstr(s, "...");
    if (ellipsis || dots) {
        fail_at(r, "truncated CDC preview — paste the full AACP frame");
    }
    const char *colon = strrchr(s, ':');
    if (!colon) fail_at(r, "CDC line missing ':' hex payload");
    return colon + 1;
}

static size_t parse_hex_bytes(replay_t *r, const char *s, uint8_t *out, size_t max) {
    s = strip_cdc_prefix(r, s);
    size_t n = 0;
    bool started = false;
    while (*s) {
        s = skip_ws(s);
        if (!*s) break;
        const char *tok = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        size_t tlen = (size_t)(s - tok);
        if (!is_hex_token(tok, tlen)) {
            if (started) fail_at(r, "non-hex token in frame");
            continue; /* label before hex */
        }
        if (n >= max) fail_at(r, "frame longer than %zu bytes", max);
        int b = parse_hex_byte(tok, tlen);
        if (b < 0) fail_at(r, "bad hex token");
        out[n++] = (uint8_t)b;
        started = true;
    }
    return n;
}

static bool reclaim_now(const replay_t *r) {
    return dual_connect_should_reclaim_on_steal(r->usb_spk_open,
                                                r->is_usb_streaming,
                                                r->we_paused);
}

static bool hijack_now(const replay_t *r) {
    return aacp_sr_should_send_hijack(r->usb_spk_open, r->is_usb_streaming,
                                      r->we_paused);
}

static void apply_pkt(replay_t *r, const uint8_t *pkt, size_t len) {
    if (len < 6u) fail_at(r, "AACP frame too short (%zu)", len);
    if (pkt[0] != 0x04 || pkt[1] != 0x00 || pkt[2] != 0x04 || pkt[3] != 0x00)
        fail_at(r, "frame is not an AACP header 04 00 04 00");
    memcpy(r->last_pkt, pkt, len);
    r->last_pkt_len = len;
    uint8_t op = pkt[4];

    if (op == 0x0Eu) {
        dual_audio_source_t src;
        r->last_0e_ok = dual_connect_parse_0e(pkt, len, &src);
        r->last_0e_they_own = r->last_0e_ok &&
            dual_connect_0e_means_they_own(&src, r->our_mac);
        /* Firmware treats 0x0E as documented noise here — no give-up. */
        return;
    }
    if (op == 0x2Eu) {
        dual_connected_device_t devs[DUAL_CONN_DEV_MAX];
        r->last_0x2e_count = dual_connect_parse_0x2e(pkt, len, devs,
                                                     DUAL_CONN_DEV_MAX);
        return;
    }
    if (op == DUAL_SR_RESP_OPCODE) {
        r->last_0x11 = dual_connect_decide_0x11(pkt, len, r->usb_spk_open,
                                                r->is_usb_streaming);
        r->last_0x11_sender_ok =
            dual_connect_parse_0x11_sender(pkt, len, r->last_0x11_sender);
        if (r->last_0x11.recognized) {
            if (r->last_0x11.set_we_paused) r->we_paused = true;
            r->state = r->last_0x11.state;
        }
        return;
    }
    /* 0x10 TX shapes, 0x09 OWNS, etc. — recorded, no invented handling. */
    (void)op;
}

static void apply_expect(replay_t *r, const char *rest) {
    const char *s = rest;
    while (*s) {
        s = skip_ws(s);
        if (!*s) break;
        const char *tok = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        size_t tlen = (size_t)(s - tok);
        char key[64], val[64];
        if (tlen >= sizeof(key)) fail_at(r, "EXPECT token too long");
        memcpy(key, tok, tlen);
        key[tlen] = '\0';
        char *eq = strchr(key, '=');
        if (!eq) continue; /* label such as "0x11" */
        *eq = '\0';
        snprintf(val, sizeof(val), "%s", eq + 1);

        if (!strcmp(key, "reclaim")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad reclaim= value");
            if (reclaim_now(r) != want)
                fail_at(r, "reclaim=%u want %u", (unsigned)reclaim_now(r),
                        (unsigned)want);
        } else if (!strcmp(key, "hijack")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad hijack= value");
            if (hijack_now(r) != want)
                fail_at(r, "aacp_sr_should_send_hijack=%u want %u",
                        (unsigned)hijack_now(r), (unsigned)want);
        } else if (!strcmp(key, "paused") || !strcmp(key, "we_paused") ||
                   !strcmp(key, "anti_ping_pong")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad paused= value");
            if (r->we_paused != want)
                fail_at(r, "paused=%u want %u", (unsigned)r->we_paused,
                        (unsigned)want);
            if (want && !dual_connect_anti_ping_pong_blocks_reclaim(r->we_paused))
                fail_at(r, "anti-ping-pong not armed while paused");
        } else if (!strcmp(key, "state")) {
            dual_connect_state_t want;
            if (!parse_state_name(val, &want))
                fail_at(r, "unknown state %s", val);
            if (r->state != want)
                fail_at(r, "state=%s want %s", state_name(r->state), val);
        } else if (!strcmp(key, "giveup")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad giveup= value");
            if ((bool)r->last_0x11.send_owns_giveup != want)
                fail_at(r, "giveup=%u want %u",
                        (unsigned)r->last_0x11.send_owns_giveup, (unsigned)want);
            if (want) {
                uint8_t giveup[DUAL_CTRL_FRAME_LEN];
                static const uint8_t k_giveup[11] = {
                    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00
                };
                dual_connect_build_owns_giveup(giveup);
                if (memcmp(giveup, k_giveup, 11) != 0)
                    fail_at(r, "OWNS give-up bytes mismatch");
            }
        } else if (!strcmp(key, "pause")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad pause= value");
            if ((bool)r->last_0x11.pause_media != want)
                fail_at(r, "pause_media=%u want %u",
                        (unsigned)r->last_0x11.pause_media, (unsigned)want);
        } else if (!strcmp(key, "recognized")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad recognized= value");
            if ((bool)r->last_0x11.recognized != want)
                fail_at(r, "recognized=%u want %u",
                        (unsigned)r->last_0x11.recognized, (unsigned)want);
        } else if (!strcmp(key, "0e_they_own") || !strcmp(key, "they_own")) {
            bool want;
            if (!parse_bool_token(val, &want)) fail_at(r, "bad 0e_they_own= value");
            if (r->last_0e_they_own != want)
                fail_at(r, "0e_they_own=%u want %u",
                        (unsigned)r->last_0e_they_own, (unsigned)want);
        } else if (!strcmp(key, "0x2e_count") || !strcmp(key, "2e_count")) {
            int want = atoi(val);
            if (r->last_0x2e_count != want)
                fail_at(r, "0x2e_count=%d want %d", r->last_0x2e_count, want);
        } else if (!strcmp(key, "sender")) {
            uint8_t mac[6];
            if (!parse_mac_text(val, mac)) fail_at(r, "bad sender MAC");
            if (!r->last_0x11_sender_ok)
                fail_at(r, "no 0x11 sender parsed");
            if (memcmp(r->last_0x11_sender, mac, 6) != 0)
                fail_at(r, "sender MAC mismatch");
        } else {
            fail_at(r, "unknown EXPECT key '%s'", key);
        }
    }

    /* Optional reclaim beat: OWNS claim + 0x20 only when hijack/reclaim gates allow. */
    if (hijack_now(r) && reclaim_now(r) && !r->we_paused) {
        uint8_t owns[DUAL_CTRL_FRAME_LEN];
        uint8_t ac[DUAL_CTRL_FRAME_LEN];
        dual_connect_build_owns_claim(owns);
        dual_connect_build_autocon(ac, 1);
        static const uint8_t k_claim[11] = {
            0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00
        };
        if (memcmp(owns, k_claim, 11) != 0)
            fail_at(r, "OWNS claim bytes mismatch on reclaim path");
        if (ac[6] != DUAL_AUTOCON_ID || ac[7] != DUAL_AUTOCON_ON)
            fail_at(r, "0x20 autocon bytes mismatch on reclaim path");
    }
}

static void replay_line(replay_t *r, char *line) {
    strip_comment(line);
    const char *s = skip_ws(line);
    if (!*s) return;

    if (!strncmp(s, "OUR_MAC", 7) && isspace((unsigned char)s[7])) {
        if (!parse_mac_text(skip_ws(s + 7), r->our_mac))
            fail_at(r, "bad OUR_MAC");
        return;
    }
    if (!strncmp(s, "USB_SPK_OPEN", 12) && isspace((unsigned char)s[12])) {
        bool v;
        const char *vs = skip_ws(s + 12);
        char vtok[8];
        size_t vn = 0;
        while (vs[vn] && !isspace((unsigned char)vs[vn]) && vn + 1 < sizeof vtok)
            vn++;
        memcpy(vtok, vs, vn);
        vtok[vn] = '\0';
        if (!parse_bool_token(vtok, &v))
            fail_at(r, "bad USB_SPK_OPEN");
        bool rising = !r->usb_spk_open && v;
        r->usb_spk_open = v;
        maybe_clear_ppp(r, false, rising);
        return;
    }
    if (!strncmp(s, "USB_STREAMING", 13) && isspace((unsigned char)s[13])) {
        bool v;
        const char *vs = skip_ws(s + 13);
        char vtok[8];
        size_t vn = 0;
        while (vs[vn] && !isspace((unsigned char)vs[vn]) && vn + 1 < sizeof vtok)
            vn++;
        memcpy(vtok, vs, vn);
        vtok[vn] = '\0';
        if (!parse_bool_token(vtok, &v))
            fail_at(r, "bad USB_STREAMING");
        bool rising = !r->is_usb_streaming && v;
        r->is_usb_streaming = v;
        maybe_clear_ppp(r, rising, false);
        return;
    }
    if (!strncmp(s, "STEAL", 5) && (s[5] == '\0' || isspace((unsigned char)s[5]))) {
        r->state = dual_connect_state_on_unexpected_pause(
            r->usb_spk_open, r->is_usb_streaming, r->we_paused);
        return;
    }
    if (!strncmp(s, "EXPECT", 6) && isspace((unsigned char)s[6])) {
        apply_expect(r, s + 6);
        return;
    }

    const char *hexsrc = s;
    if (!strncmp(s, "HEX", 3) && (s[3] == '\0' || isspace((unsigned char)s[3]) ||
                                  s[3] == ':')) {
        hexsrc = s + 3;
        if (*hexsrc == ':') hexsrc++;
    } else if (strncmp(s, "[AACP]", 6) != 0) {
        /* Raw hex line: first token must be hex. */
        const char *t = s;
        size_t n = 0;
        while (t[n] && !isspace((unsigned char)t[n])) n++;
        if (!is_hex_token(t, n))
            fail_at(r, "unknown directive");
    }

    uint8_t pkt[PKT_MAX];
    size_t n = parse_hex_bytes(r, hexsrc, pkt, sizeof pkt);
    if (n == 0) fail_at(r, "empty hex frame");
    apply_pkt(r, pkt, n);
}

static void replay_text(const char *source, const char *text) {
    replay_t r;
    replay_init(&r, source);
    const char *p = text;
    char line[LINE_MAX];
    while (*p) {
        r.line_no++;
        size_t i = 0;
        while (*p && *p != '\n' && i + 1 < sizeof(line)) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        replay_line(&r, line);
    }
}

static void replay_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        exit(1);
    }
    replay_t r;
    replay_init(&r, path);
    char line[LINE_MAX];
    while (fgets(line, sizeof line, f)) {
        r.line_no++;
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(f)) {
            fclose(f);
            fail_at(&r, "line too long");
        }
        replay_line(&r, line);
    }
    fclose(f);
}

static int file_readable(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    int nscripts = 0;

    /* Always prove the built-in sequenced steal (same packets as the golden file). */
    replay_text("builtin:dual_connect_iphone_steal", k_builtin_iphone_steal);
    nscripts++;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            replay_file(argv[i]);
            nscripts++;
        }
    } else if (file_readable(k_default_fixture)) {
        replay_file(k_default_fixture);
        nscripts++;
    }

    /* Builders used on the reclaim path after gates allow (OWNS + 0x20 + 0x10). */
    {
        uint8_t claim[DUAL_CTRL_FRAME_LEN];
        uint8_t giveup[DUAL_CTRL_FRAME_LEN];
        uint8_t ac[DUAL_CTRL_FRAME_LEN];
        dual_connect_build_owns_claim(claim);
        dual_connect_build_owns_giveup(giveup);
        dual_connect_build_autocon(ac, 1);
        static const uint8_t k_claim[11] = {
            0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00
        };
        static const uint8_t k_giveup[11] = {
            0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00
        };
        EQS(claim, k_claim, 11, "replay OWNS claim");
        EQS(giveup, k_giveup, 11, "replay OWNS giveup");
        EQ(ac[6], DUAL_AUTOCON_ID, "replay 0x20 id");
        EQ(ac[7], DUAL_AUTOCON_ON, "replay 0x20 on");
        EQ(aacp_sr_should_send_hijack(true, true, false), 1, "hijack after clear");
        EQ(aacp_sr_should_send_hijack(true, true, true), 0, "hijack blocked paused");
    }

    printf("aacp_dump_replay_test: PASS (%d script(s); policy replay, "
           "not air/DID/Windows)\n", nscripts);
    return 0;
}
