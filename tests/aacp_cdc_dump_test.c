// Host-side CDC dump policy: dual-connect opcodes always full-frame.
// gcc -I. -O2 -o /tmp/aacp_cdc_dump_test tests/aacp_cdc_dump_test.c && /tmp/aacp_cdc_dump_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/btstack/aacp_cdc_dump.h"

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

int main(void) {
    uint8_t pkt_0e[13] = {
        0x04, 0x00, 0x04, 0x00, 0x0E, 0x00,
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x02
    };
    uint8_t pkt_11[40] = {
        0x04, 0x00, 0x04, 0x00, 0x11, 0x00,
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA
    };
    uint8_t pkt_2e[9] = {
        0x04, 0x00, 0x04, 0x00, 0x2E, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t pkt_10[8] = {
        0x04, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00
    };
    uint8_t owns[11] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00
    };
    uint8_t autocon[11] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00
    };
    uint8_t noise[11] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00
    };
    uint8_t bat[8] = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x02
    };

    EQ(aacp_cdc_dump_should_full(pkt_0e, sizeof pkt_0e, false), 1, "0x0E always full");
    EQ(aacp_cdc_dump_should_full(pkt_11, sizeof pkt_11, false), 1, "0x11 always full");
    EQ(aacp_cdc_dump_should_full(pkt_2e, sizeof pkt_2e, false), 1, "0x2E always full");
    EQ(aacp_cdc_dump_should_full(pkt_10, sizeof pkt_10, false), 1, "0x10 always full");
    EQ(aacp_cdc_dump_should_full(owns, sizeof owns, false), 1, "OWNS 0x06 always full");
    EQ(aacp_cdc_dump_should_full(autocon, sizeof autocon, false), 1, "0x20 always full");
    EQ(aacp_cdc_dump_should_full(noise, sizeof noise, false), 0, "noise preview unless aacpdump");
    EQ(aacp_cdc_dump_should_full(bat, sizeof bat, false), 0, "battery preview unless aacpdump");
    EQ(aacp_cdc_dump_should_full(bat, sizeof bat, true), 1, "aacpdump on → battery full");
    EQ(aacp_cdc_dump_should_full(noise, sizeof noise, true), 1, "aacpdump on → noise full");

    EQ(strcmp(aacp_cdc_opcode_name(0x0E), "audio-src") == 0, 1, "0x0E name");
    EQ(strcmp(aacp_cdc_opcode_name(0x10), "smart-routing") == 0, 1, "0x10 name");
    EQ(strcmp(aacp_cdc_opcode_name(0x11), "SetOwnershipToFalse") == 0, 1, "0x11 name");
    EQ(strcmp(aacp_cdc_opcode_name(0x2E), "connected-devices") == 0, 1, "0x2E name");
    EQ(strcmp(aacp_cdc_ctrl_id_name(0x06), "OWNS") == 0, 1, "OWNS name");
    EQ(strcmp(aacp_cdc_ctrl_id_name(0x20), "autocon") == 0, 1, "0x20 name");
    EQ(aacp_cdc_opcode_name(0x58) == NULL, 1, "no invented 0x58 name");

    EQ(aacp_cdc_line_is_truncated("[AACP] x n=40: 04 00 …"), 1, "ellipsis");
    EQ(aacp_cdc_line_is_truncated("[AACP] x n=40: 04 00 ..."), 1, "dotdotdot");
    EQ(aacp_cdc_line_is_truncated("[AACP] x n=13: 04 00 04 00 0E 00"), 0, "full line");

    printf("aacp_cdc_dump_test: PASS\n");
    return 0;
}
