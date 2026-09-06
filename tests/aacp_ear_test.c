// Host-side proof of Max ear-off Pause and bounce-safe resume.
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

    // Pause stays snappy; Play after we paused waits for stable both-on-head.
    EQ(aacp_ear_commit_delay_ms(true, false), AACP_EAR_OFF_DEBOUNCE_MS, "off no resume");
    EQ(aacp_ear_commit_delay_ms(true, true), AACP_EAR_OFF_DEBOUNCE_MS, "off with resume");
    EQ(aacp_ear_commit_delay_ms(false, false), AACP_EAR_OFF_DEBOUNCE_MS, "on no resume");
    EQ(aacp_ear_commit_delay_ms(false, true), AACP_EAR_RESUME_STABLE_MS, "on after Pause");
    if (AACP_EAR_RESUME_STABLE_MS < 1000 || AACP_EAR_RESUME_STABLE_MS > 2000) {
        fprintf(stderr, "FAIL: resume stable ms should be 1–2 s, got %u\n",
                (unsigned) AACP_EAR_RESUME_STABLE_MS);
        return 1;
    }

    // Live bounce 2026-09-05: Pause, then L=0 R=1 (still off), then L=0 R=0.
    // 200 ms on-head delay would HID Play; 1500 ms does not on a short flap.
    EQ(aacp_ear_is_off_head(0x00, 0x01), 1, "bounce L=0 R=1 still off-head");
    EQ(aacp_ear_is_off_head(0x00, 0x00), 0, "bounce L=0 R=0 looks on-head");
    uint32_t bounce_play_delay = aacp_ear_commit_delay_ms(false, true);
    EQ(bounce_play_delay > AACP_EAR_OFF_DEBOUNCE_MS, 1, "resume delay longer than Pause debounce");

    printf("aacp_ear_test: PASS (either-cup-out pauses; resume waits %u ms stable on-head)\n",
           (unsigned) AACP_EAR_RESUME_STABLE_MS);
    return 0;
}
