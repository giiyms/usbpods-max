// Host-side proof of Max ear-off: pause when EITHER cup is not on-head.
// gcc -I. -O2 -o /tmp/aacp_ear_test tests/aacp_ear_test.c && /tmp/aacp_ear_test

#include <stdio.h>
#include <stdlib.h>
#include "src/btstack/aacp_ear.h"

#define EQ(a, b, msg) do { if ((int)(a) != (int)(b)) { \
    fprintf(stderr, "FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    exit(1); } } while (0)

int main(void) {
    // Worn: L=0 R=0
    EQ(aacp_ear_is_off_head(0x00, 0x00), 0, "both on-head");

    // Live Max 2 take-off: L=1 R=0 — old BOTH-out gate skipped Pause.
    EQ(aacp_ear_is_off_head(0x01, 0x00), 1, "asymmetric L out");
    EQ(aacp_ear_is_off_head(0x00, 0x01), 1, "asymmetric R out");

    // Both out / in case
    EQ(aacp_ear_is_off_head(0x01, 0x01), 1, "both out");
    EQ(aacp_ear_is_off_head(0x02, 0x02), 1, "both in case");
    EQ(aacp_ear_is_off_head(0x01, 0x02), 1, "mixed out/case");

    // Unknown: do not pause
    EQ(aacp_ear_is_off_head(0xFF, 0x00), 0, "unknown L");
    EQ(aacp_ear_is_off_head(0x00, 0xFF), 0, "unknown R");
    EQ(aacp_ear_is_off_head(0xFF, 0xFF), 0, "unknown both");

    // Old gate (BOTH out) vs new (either out) on the live log.
    int old_off = (0x01 != 0x00) && (0x00 != 0x00);
    int new_off = aacp_ear_is_off_head(0x01, 0x00);
    EQ(old_off, 0, "old BOTH-out misses L=1 R=0");
    EQ(new_off, 1, "new either-out catches L=1 R=0");

    printf("aacp_ear_test: PASS (either-cup-out is off-head; L=1 R=0 pauses)\n");
    return 0;
}
