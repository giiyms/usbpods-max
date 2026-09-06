// Host-side proof of Max ear-off Pause + bounce hold-off.
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

    EQ(aacp_ear_delay_ms(true, false), AACP_EAR_PAUSE_DEBOUNCE_MS, "off debounce");
    EQ(aacp_ear_delay_ms(false, false), AACP_EAR_PAUSE_DEBOUNCE_MS, "on no-resume");
    EQ(aacp_ear_delay_ms(false, true), AACP_EAR_RESUME_HOLD_MS, "on after Pause");
    EQ(AACP_EAR_RESUME_HOLD_MS >= 2000, 1, "hold >= 2s");

    // --- Live 2026-09-05 bounce: Pause then L=0 R=0 must NOT Play at 200 ms.
    aacp_ear_sm_t s;
    aacp_ear_sm_reset(&s);
    s.known = true;
    s.off_head = false;

    uint32_t d = aacp_ear_consider_delay(&s, 0x01, 0x00); // L=1 R=0
    EQ(d, AACP_EAR_PAUSE_DEBOUNCE_MS, "take-off debounce 200");
    EQ(s.pending_off, 1, "pending off");

    d = aacp_ear_consider_delay(&s, 0x01, 0x01); // still off
    EQ(d, 0, "same off: no re-arm");

    aacp_ear_act_t act = aacp_ear_fire(&s, false, true);
    EQ(act, AACP_EAR_ACT_PAUSE, "HID Pause");
    EQ(s.resume_pending, 1, "resume pending");
    EQ(s.off_head, 1, "now off-head");

    d = aacp_ear_consider_delay(&s, 0x00, 0x01); // L=0 R=1 still off
    EQ(d, 0, "either-cup-out still off");

    d = aacp_ear_consider_delay(&s, 0x00, 0x00); // bounce / table
    EQ(d, AACP_EAR_RESUME_HOLD_MS, "bounce must wait 2s not 200ms");
    EQ(d != AACP_EAR_PAUSE_DEBOUNCE_MS, 1, "not the Pause debounce");

    // If we fired at 200 ms we would Play — that was the bug.
    EQ(AACP_EAR_RESUME_HOLD_MS > 200, 1, "hold longer than old debounce");

    act = aacp_ear_fire(&s, false, true);
    EQ(act, AACP_EAR_ACT_PLAY, "Play only after stable on-head hold");
    EQ(s.resume_pending, 0, "resume consumed");

    // Bounce then real off again: cancel Play, stay paused.
    aacp_ear_sm_reset(&s);
    s.known = true;
    s.off_head = false;
    (void) aacp_ear_consider_delay(&s, 0x01, 0x01);
    EQ(aacp_ear_fire(&s, false, true), AACP_EAR_ACT_PAUSE, "pause again");
    d = aacp_ear_consider_delay(&s, 0x00, 0x00);
    EQ(d, AACP_EAR_RESUME_HOLD_MS, "on-head starts hold");
    d = aacp_ear_consider_delay(&s, 0x01, 0x01); // flap back to out
    EQ(d, AACP_EAR_PAUSE_DEBOUNCE_MS, "off re-arms pause debounce");
    act = aacp_ear_fire(&s, false, true);
    EQ(act, AACP_EAR_ACT_SKIP_NOCHANGE, "still off-head: no Play");
    EQ(s.resume_pending, 1, "stay resume-pending");

    // Put on while already paused by user: no Play.
    aacp_ear_sm_reset(&s);
    s.known = true;
    s.off_head = true;
    s.resume_pending = false;
    (void) aacp_ear_consider_delay(&s, 0x00, 0x00);
    EQ(aacp_ear_fire(&s, false, true), AACP_EAR_ACT_SKIP_STAY_PAUSED, "stay paused");

    // First packet after connect: no HID (do not start Music).
    aacp_ear_sm_reset(&s);
    (void) aacp_ear_consider_delay(&s, 0x00, 0x00);
    EQ(aacp_ear_fire(&s, false, true), AACP_EAR_ACT_SKIP_FIRST, "first packet");

    // Never a Play/Pause toggle action — only discrete Pause or Play.
    printf("aacp_ear_test: PASS (either-cup-out; 2s resume hold skips bounce Play)\n");
    return 0;
}
