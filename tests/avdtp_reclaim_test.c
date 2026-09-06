// Host-side proof of steal/reclaim backoff after iPhone dual-connect.
// gcc -I. -O2 -o /tmp/avdtp_reclaim_test tests/avdtp_reclaim_test.c && /tmp/avdtp_reclaim_test

#include <stdio.h>
#include <stdlib.h>
#include "src/btstack/avdtp_reclaim.h"

#define EQ(a, b, msg) do { if ((int)(a) != (int)(b)) { \
    fprintf(stderr, "FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    exit(1); } } while (0)

int main(void) {
    EQ(avdtp_should_autostart_after_suspend(true), 1, "local suspend recovery starts");
    EQ(avdtp_should_autostart_after_suspend(false), 0, "remote steal does not auto-START");

    EQ(avdtp_reclaim_next_delay_ms(0, false, 0), AVDTP_RECLAIM_DELAY_MS,
       "clean drop 400ms");
    EQ(avdtp_reclaim_next_delay_ms(0, true, 0), AVDTP_RECLAIM_STEAL_DELAY_MS,
       "steal first wait 1500ms");
    EQ(AVDTP_RECLAIM_STEAL_DELAY_MS > AVDTP_RECLAIM_DELAY_MS, 1,
       "steal slower than clean drop");

    EQ(avdtp_reclaim_status_busy(129), 1, "status 129 is busy");
    EQ(avdtp_reclaim_status_busy(0x0C), 1, "0x0C COMMAND_DISALLOWED is busy");
    EQ(avdtp_reclaim_status_busy(0), 0, "success is not busy");

    EQ(avdtp_reclaim_next_delay_ms(1, true, 129), AVDTP_RECLAIM_BUSY_RETRY_MS,
       "129 → 4s retry");
    EQ(avdtp_reclaim_next_delay_ms(1, true, 0), AVDTP_RECLAIM_STEAL_DELAY_MS,
       "steal retry without busy");
    EQ(avdtp_reclaim_next_delay_ms(1, false, 0), AVDTP_RECLAIM_RETRY_MS,
       "clean retry 2s");

    EQ(avdtp_reclaim_should_drop_stale(129, 1), 0, "first busy: do not drop yet");
    EQ(avdtp_reclaim_should_drop_stale(129, 2), 1, "second busy: drop stale AVDTP");
    EQ(avdtp_reclaim_should_drop_stale(0, 2), 0, "success: do not drop");
    EQ(avdtp_reclaim_should_drop_stale(129, 4), 1, "even tries drop");
    EQ(avdtp_reclaim_should_drop_stale(129, 3), 0, "odd tries keep");

    EQ(AVDTP_RECLAIM_MAX_TRIES > 30, 1, "more tries than pre-steal 30");

    printf("avdtp_reclaim_test: PASS (no auto-START on steal; 129 backs off)\n");
    return 0;
}
