// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
//
// Debug logging over USB CDC — see debug_cdc.h.
//
// printf() may be called from both thread context (BTstack run loop on Core 0)
// and IRQ context (the 500us USB timer that drives tud_task and its callbacks).
// To keep the TinyUSB CDC FIFO single-writer, all printf output first lands in
// an interrupt-safe ring buffer (producers briefly disable IRQs), and only the
// USB task context (debug_cdc_task) ever touches tud_cdc_*.
//

#include <stdint.h>
#include <stdio.h>

#include "pico/stdio/driver.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "debug_cdc.h"

#define DBG_BUF_SIZE 8192u                 // must be a power of two
#define DBG_BUF_MASK (DBG_BUF_SIZE - 1u)

static uint8_t           dbg_buf[DBG_BUF_SIZE];
static volatile uint32_t dbg_head = 0;     // producer index
static volatile uint32_t dbg_tail = 0;     // consumer index (USB task only)
static volatile uint32_t dbg_dropped = 0;  // bytes discarded while the ring was full

// Producer side. Runs with IRQs disabled so thread- and IRQ-context printf
// calls can't interleave, and so the USB-task consumer can't preempt a push.
static void dbg_push_locked(uint8_t c) {
    uint32_t next = (dbg_head + 1u) & DBG_BUF_MASK;
    if (next == dbg_tail) {                // full: drop (debug output is best-effort)
        dbg_dropped++;                     // ...but make the gap self-evident later
        return;
    }
    dbg_buf[dbg_head] = c;
    dbg_head = next;
}

static void cdc_out_chars(const char *buf, int len) {
    uint32_t save = save_and_disable_interrupts();
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') dbg_push_locked('\r');  // make output terminal-friendly
        dbg_push_locked((uint8_t) c);
    }
    restore_interrupts(save);
}

static stdio_driver_t debug_cdc_driver = {
    .out_chars = cdc_out_chars,
    .out_flush = NULL,
    .in_chars  = NULL,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false,   // we insert CR ourselves above
#endif
};

void debug_cdc_init(void) {
    stdio_set_driver_enabled(&debug_cdc_driver, true);
}

void debug_cdc_task(void) {
    // Drain only while a terminal is actually attached (DTR asserted). Until
    // then, output accumulates in the 8KB ring, so boot/pairing logs are still
    // delivered when the user opens the terminal late.
    if (!tud_cdc_connected()) return;

    // If bytes were discarded while the ring was full, report the gap once the
    // ring has room again, so a log gap is never silent. (We run in the USB
    // timer IRQ; producers push with IRQs disabled, so guard the same way.)
    if (dbg_dropped != 0 && ((dbg_head - dbg_tail) & DBG_BUF_MASK) < DBG_BUF_SIZE / 2u) {
        char msg[48];
        int n = snprintf(msg, sizeof(msg), "\r\n[log dropped %lu bytes]\r\n",
                         (unsigned long) dbg_dropped);
        uint32_t save = save_and_disable_interrupts();
        dbg_dropped = 0;
        for (int i = 0; i < n; i++) dbg_push_locked((uint8_t) msg[i]);
        restore_interrupts(save);
    }

    if (dbg_tail == dbg_head) return;

    // Bounded work per call: at most ONE 64-byte chunk per tick. This runs in
    // the 500us USB timer IRQ; an unbounded drain loop under heavy printf
    // traffic could hog the IRQ.
    uint32_t avail = tud_cdc_write_available();
    if (avail == 0) return;

    uint8_t  tmp[64];
    uint32_t n = 0;
    while (n < avail && n < sizeof(tmp) && dbg_tail != dbg_head) {
        tmp[n++] = dbg_buf[dbg_tail];
        dbg_tail = (dbg_tail + 1u) & DBG_BUF_MASK;
    }
    if (n == 0) return;
    tud_cdc_write(tmp, n);
    tud_cdc_write_flush();
}
