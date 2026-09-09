// SPDX-License-Identifier: GPL-3.0-only
//
// CDC AACP hex-dump policy. Dual-connect / smart-routing opcodes always
// print complete frames (cheap: they are rare). Other packets stay at a
// 24-byte preview unless `aacpdump on` (Quiet CDC console / Mac screen).
// No invented opcodes. Host-testable (no BTstack).
//

#ifndef USBPODS_AACP_CDC_DUMP_H
#define USBPODS_AACP_CDC_DUMP_H

#include <stdint.h>
#include <stdbool.h>

#define AACP_CDC_DUMP_PREVIEW  24u

// RX/TX opcodes that dump-replay / dual-connect steal needs in full.
static inline bool aacp_cdc_dump_is_dual_opcode(uint8_t op) {
    return op == 0x0Eu || op == 0x10u || op == 0x11u || op == 0x2Eu ||
           op == 0x0Du || op == 0x2Du;
}

// Control-command ids (opcode 0x0009) that mark ownership fights.
static inline bool aacp_cdc_dump_is_dual_ctrl_id(uint8_t id) {
    return id == 0x06u || id == 0x20u;
}

// LibrePods names from AACP-FEATURES.md — do not invent.
static inline const char *aacp_cdc_opcode_name(uint8_t op) {
    switch (op) {
        case 0x04u: return "battery";
        case 0x06u: return "ear-detect";
        case 0x09u: return "control";
        case 0x0Du: return "audio-src-req";
        case 0x0Eu: return "audio-src";
        case 0x0Fu: return "request-notifications";
        case 0x10u: return "smart-routing";
        case 0x11u: return "SetOwnershipToFalse";
        case 0x19u: return "stem";
        case 0x1Au: return "rename";
        case 0x1Du: return "device-info";
        case 0x2Du: return "connected-dev-req";
        case 0x2Eu: return "connected-devices";
        case 0x4Bu: return "CA-speaking";
        case 0x4Du: return "SET_FEATURE_FLAGS";
        default:    return NULL;
    }
}

static inline const char *aacp_cdc_ctrl_id_name(uint8_t id) {
    switch (id) {
        case 0x06u: return "OWNS";
        case 0x0Au: return "ear-detect-en";
        case 0x0Du: return "noise";
        case 0x16u: return "click-hold";
        case 0x1Au: return "listen-mask";
        case 0x1Cu: return "crown";
        case 0x1Eu: return "auto-answer";
        case 0x1Fu: return "chime";
        case 0x20u: return "autocon";
        case 0x26u: return "adapt-vol";
        case 0x28u: return "CA";
        case 0x34u: return "allow-off";
        case 0x35u: return "sleep";
        case 0x39u: return "gestures";
        default:    return NULL;
    }
}

// dump_all: CDC verb `aacpdump on` — full frames for every hex dump.
// Dual-connect opcodes (and 0x09 OWNS/0x20) are always full even when dump_all
// is false, so a steal capture works without remembering the verb.
static inline bool aacp_cdc_dump_should_full(const uint8_t *p, uint16_t n,
                                               bool dump_all) {
    if (dump_all) return true;
    if (!p || n < 6u) return false;
    if (p[0] != 0x04u || p[1] != 0x00u || p[2] != 0x04u || p[3] != 0x00u)
        return aacp_cdc_dump_is_dual_opcode(p[4]);
    if (p[4] == 0x09u && n >= 7u && aacp_cdc_dump_is_dual_ctrl_id(p[6]))
        return true;
    return aacp_cdc_dump_is_dual_opcode(p[4]);
}

static inline bool aacp_cdc_line_is_truncated(const char *s) {
    if (!s) return false;
    for (const char *p = s; *p; p++) {
        if (p[0] == '.' && p[1] == '.' && p[2] == '.') return true;
        /* UTF-8 ellipsis U+2026 = E2 80 A6 */
        if ((unsigned char)p[0] == 0xE2u &&
            (unsigned char)p[1] == 0x80u &&
            (unsigned char)p[2] == 0xA6u) return true;
    }
    return false;
}

#endif /* USBPODS_AACP_CDC_DUMP_H */
