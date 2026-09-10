// Host-side proof of post-reclaim USB session wake (Play delay/retry, unmute, iso nudge).
// gcc -I. -O2 -o /tmp/host_session_wake_test tests/host_session_wake_test.c && /tmp/host_session_wake_test

#include <stdio.h>
#include <stdlib.h>
#include "src/btstack/host_session_wake.h"
#include "src/btstack/dual_connect_policy.h"

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

int main(void) {
    if (HOST_WAKE_PLAY_DELAY_MS < 50 || HOST_WAKE_PLAY_DELAY_MS > 300) {
        fprintf(stderr, "FAIL: Play delay should be 50–300 ms, got %u\n",
                (unsigned) HOST_WAKE_PLAY_DELAY_MS);
        return 1;
    }
    if (HOST_WAKE_RETRY_MS <= HOST_WAKE_PLAY_DELAY_MS) {
        fprintf(stderr, "FAIL: retry must be later than first Play\n");
        return 1;
    }
    EQ(HOST_WAKE_MAX_PLAYS, 2, "at most two Plays (no HID spray)");

    // Gate matches dual_connect_allow_play_after_reclaim.
    EQ(host_wake_should_arm(false, true), 0, "no arm without steal pause");
    EQ(host_wake_should_arm(true, false), 0, "no arm before A2DP streaming");
    EQ(host_wake_should_arm(true, true), 1, "arm after reclaim STREAM");
    EQ(dual_connect_allow_play_after_reclaim(true), 1, "Play gate still steal-only");

    // USB stream drop while A2DP + owns + speaker alt open (live Quiet case).
    EQ(host_wake_should_arm_on_usb_drop(false, true, true, true), 0,
       "no arm without falling edge");
    EQ(host_wake_should_arm_on_usb_drop(true, false, true, true), 0,
       "no arm without A2DP streaming");
    EQ(host_wake_should_arm_on_usb_drop(true, true, false, true), 0,
       "no arm without owns");
    EQ(host_wake_should_arm_on_usb_drop(true, true, true, false), 0,
       "no arm when speaker alt closed");
    EQ(host_wake_should_arm_on_usb_drop(true, true, true, true), 1,
       "arm on USB drop while A2DP/owns/spk open");

    host_wake_t w;
    host_wake_reset(&w);

    // Too early: signaling-up must not Play (arm requires a2dp_streaming).
    EQ(host_wake_arm(&w, 0, true, false), HOST_WAKE_ACT_NONE, "signaling-up no arm");
    EQ(w.phase, HOST_WAKE_IDLE, "still idle");

    uint8_t acts = host_wake_arm(&w, 1000, true, true);
    EQ(acts, HOST_WAKE_ACT_UNMUTE, "unmute on arm");
    EQ(w.phase, HOST_WAKE_WAIT_PLAY, "wait Play");
    EQ(w.due_ms, 1000 + HOST_WAKE_PLAY_DELAY_MS, "Play not immediate");
    EQ(host_wake_arm(&w, 1001, true, true), HOST_WAKE_ACT_NONE, "no double-arm");

    EQ(host_wake_poll(&w, 1000 + HOST_WAKE_PLAY_DELAY_MS - 1, true, false),
       HOST_WAKE_ACT_NONE, "Play not before delay");
    acts = host_wake_poll(&w, 1000 + HOST_WAKE_PLAY_DELAY_MS, true, false);
    EQ(acts, HOST_WAKE_ACT_PLAY, "first Play after delay");
    EQ(acts & HOST_WAKE_ACT_ISO_NUDGE, 0, "no iso nudge on first Play");
    EQ(w.plays_sent, 1, "one Play");
    EQ(w.phase, HOST_WAKE_WAIT_PCM, "wait USB PCM");

    // USB PCM resumes → done, no retry.
    host_wake_t ok = w;
    EQ(host_wake_poll(&ok, ok.due_ms - 1, true, true), HOST_WAKE_ACT_NONE,
       "PCM up cancels retry");
    EQ(ok.phase, HOST_WAKE_IDLE, "idle after PCM");

    // Alt open, still silent → retry Play + iso nudge (volume int + local reset).
    acts = host_wake_poll(&w, w.due_ms, true, false);
    EQ(acts & HOST_WAKE_ACT_PLAY, HOST_WAKE_ACT_PLAY, "retry Play");
    EQ(acts & HOST_WAKE_ACT_ISO_NUDGE, HOST_WAKE_ACT_ISO_NUDGE,
       "iso nudge when alt open and no PCM");
    EQ(w.plays_sent, 2, "second Play is last");
    EQ(host_wake_poll(&w, w.due_ms, true, false), HOST_WAKE_ACT_NONE,
       "no third Play");
    EQ(w.phase, HOST_WAKE_IDLE, "idle after max Plays");

    // Alt closed: retry Play but do not fake SET_INTERFACE / iso nudge.
    host_wake_reset(&w);
    host_wake_arm(&w, 0, true, true);
    host_wake_poll(&w, HOST_WAKE_PLAY_DELAY_MS, false, false);
    acts = host_wake_poll(&w, HOST_WAKE_PLAY_DELAY_MS + HOST_WAKE_RETRY_MS,
                          false, false);
    EQ(acts & HOST_WAKE_ACT_PLAY, HOST_WAKE_ACT_PLAY, "Play even if alt 0");
    EQ(acts & HOST_WAKE_ACT_ISO_NUDGE, 0, "no iso nudge when alt closed");

    host_wake_reset(&w);
    host_wake_arm(&w, 50, true, true);
    host_wake_cancel(&w);
    EQ(w.phase, HOST_WAKE_IDLE, "cancel");
    EQ(host_wake_poll(&w, 9999, true, false), HOST_WAKE_ACT_NONE, "cancelled");

    EQ(host_wake_next_delay_ms(&w, 0), 0, "idle delay 0");
    host_wake_arm(&w, 200, true, true);
    EQ(host_wake_next_delay_ms(&w, 200), HOST_WAKE_PLAY_DELAY_MS, "delay to Play");
    EQ(host_wake_next_delay_ms(&w, 200 + HOST_WAKE_PLAY_DELAY_MS), 1, "overdue → 1");

    // USB-drop arm reuses the same unmute → delayed Play → retry path.
    host_wake_reset(&w);
    acts = host_wake_arm_usb_drop(&w, 5000, true, true, true, true);
    EQ(acts, HOST_WAKE_ACT_UNMUTE, "usb-drop unmute on arm");
    EQ(w.phase, HOST_WAKE_WAIT_PLAY, "usb-drop wait Play");
    EQ(host_wake_arm_usb_drop(&w, 5001, true, true, true, true),
       HOST_WAKE_ACT_NONE, "usb-drop no double-arm");
    EQ(host_wake_arm(&w, 5001, true, true), HOST_WAKE_ACT_NONE,
       "reclaim arm blocked while usb-drop wake active");
    acts = host_wake_poll(&w, 5000 + HOST_WAKE_PLAY_DELAY_MS, true, false);
    EQ(acts, HOST_WAKE_ACT_PLAY, "usb-drop first Play");
    acts = host_wake_poll(&w, w.due_ms, true, false);
    EQ(acts & HOST_WAKE_ACT_PLAY, HOST_WAKE_ACT_PLAY, "usb-drop retry Play");
    EQ(acts & HOST_WAKE_ACT_ISO_NUDGE, HOST_WAKE_ACT_ISO_NUDGE,
       "usb-drop iso nudge when alt open and no PCM");

    // Negative: falling but no owns / no A2DP / alt closed → no arm.
    host_wake_reset(&w);
    EQ(host_wake_arm_usb_drop(&w, 0, true, true, false, true),
       HOST_WAKE_ACT_NONE, "usb-drop no arm without owns");
    EQ(w.phase, HOST_WAKE_IDLE, "still idle without owns");

    printf("host_session_wake_test: PASS "
           "(reclaim + usb-drop gates, unmute, Play +%u ms, "
           "one retry + iso nudge, no spray)\n",
           (unsigned) HOST_WAKE_PLAY_DELAY_MS);
    return 0;
}
