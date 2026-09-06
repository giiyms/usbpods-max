// Host-side proof of A2DP reclaim backoff after dual-connect steal / status 129.
// gcc -I. -O2 -o /tmp/avdtp_reclaim_test tests/avdtp_reclaim_test.c && /tmp/avdtp_reclaim_test

#include <stdio.h>
#include <stdlib.h>
#include "src/btstack/avdtp_reclaim.h"

#define EQ(a, b, msg) do { if ((unsigned)(a) != (unsigned)(b)) { \
    fprintf(stderr, "FAIL: %s (%u != %u)\n", msg, (unsigned)(a), (unsigned)(b)); \
    exit(1); } } while (0)

int main(void) {
    EQ(avdtp_reclaim_arm_delay_ms(false), AVDTP_RECLAIM_DELAY_MS, "normal release 400 ms");
    EQ(avdtp_reclaim_arm_delay_ms(true), AVDTP_RECLAIM_STEAL_DELAY_MS, "steal first wait 2 s");
    if (AVDTP_RECLAIM_STEAL_DELAY_MS <= AVDTP_RECLAIM_DELAY_MS) {
        fprintf(stderr, "FAIL: steal delay must exceed normal release delay\n");
        return 1;
    }

    EQ(avdtp_reclaim_backoff_ms(0, 1), AVDTP_RECLAIM_RETRY_MS, "generic retry 2 s");
    uint32_t d129_0 = avdtp_reclaim_backoff_ms((uint8_t) AVDTP_RECLAIM_STATUS_RESOURCE, 0);
    uint32_t d129_4 = avdtp_reclaim_backoff_ms((uint8_t) AVDTP_RECLAIM_STATUS_RESOURCE, 4);
    uint32_t d129_20 = avdtp_reclaim_backoff_ms((uint8_t) AVDTP_RECLAIM_STATUS_RESOURCE, 20);
    EQ(d129_0, AVDTP_RECLAIM_STATUS129_MS, "status 129 first backoff 5 s");
    if (d129_4 <= d129_0) {
        fprintf(stderr, "FAIL: status 129 backoff should grow with tries\n");
        return 1;
    }
    EQ(d129_20, 10000, "status 129 backoff cap 10 s");
    if (d129_0 <= AVDTP_RECLAIM_RETRY_MS) {
        fprintf(stderr, "FAIL: 129 backoff must exceed stacked 2 s retries\n");
        return 1;
    }

    EQ(AVDTP_RECLAIM_STATUS_RESOURCE, 129, "live HCI/AVDTP status 129");
    if (AVDTP_RECLAIM_MAX_TRIES < 30) {
        fprintf(stderr, "FAIL: max tries should not shrink below 30\n");
        return 1;
    }

    printf("avdtp_reclaim_test: PASS (steal %u ms, status 129 backoff %u..%u ms, max %u tries)\n",
           (unsigned) AVDTP_RECLAIM_STEAL_DELAY_MS,
           (unsigned) d129_0, (unsigned) d129_20,
           (unsigned) AVDTP_RECLAIM_MAX_TRIES);
    return 0;
}
