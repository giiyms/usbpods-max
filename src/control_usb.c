// SPDX-License-Identifier: GPL-3.0-only
//
// TinyUSB side of the control plane: CDC menu drain + vendor HID callbacks.
// Include tusb.h here only. Do not include btstack.h — TinyUSB hid.h and
// BTstack btstack_hid.h both typedef hid_report_type_t; they will not link
// as forward tud_* inlines either. BTstack lives in control.c.
//

#include "control.h"
#include "hid_consumer.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

static bool cdc_was_connected = false;
static char cdc_line[80];
static uint8_t cdc_line_len = 0;

void control_init(void) {
    cdc_was_connected = false;
    cdc_line_len = 0;
}

static void hid_vendor_send_status(void) {
    if (!tud_hid_n_ready(HID_INST_VENDOR)) return;
    uint8_t buf[CTRL_REPORT_LEN];
    control_fill_status(buf, sizeof(buf));
    tud_hid_n_report(HID_INST_VENDOR, 0, buf, sizeof(buf));
}

void control_usb_task(void) {
    hid_consumer_task();

    bool now = tud_cdc_connected();
    if (now && !cdc_was_connected) {
        printf("\nUSBPods Max console. Type h for help.\n");
        control_print_status_human();
        cdc_line_len = 0;
    }
    cdc_was_connected = now;
    if (!now) return;

    while (tud_cdc_available()) {
        char c;
        if (tud_cdc_read(&c, 1) != 1) break;
        if (c == '\b' || c == 127) {
            if (cdc_line_len > 0) cdc_line_len--;
            continue;
        }
        // Mac `screen` often sends CR-only; also accept LF. Skip leftover LF after CR.
        if (c == '\r' || c == '\n') {
            if (c == '\n' && cdc_line_len == 0) continue; // leftover LF after CR
            cdc_line[cdc_line_len] = 0;
            cdc_line_len = 0;
            control_process_line(cdc_line);
            continue;
        }
        if (cdc_line_len + 1 < sizeof(cdc_line)) {
            cdc_line[cdc_line_len++] = c;
        }
    }
}

//--------------------------------------------------------------------+
// TinyUSB HID callbacks — vendor instance 0, consumer instance 1
//--------------------------------------------------------------------+

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void) report_id;
    (void) report_type;
    if (instance == HID_INST_CONSUMER) return 0;
    if (reqlen < CTRL_REPORT_LEN) return 0;
    control_fill_status(buffer, CTRL_REPORT_LEN);
    return CTRL_REPORT_LEN;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void) report_id;
    (void) report_type;
    if (instance != HID_INST_VENDOR) return;
    if (control_handle_hid_cmd(buffer, bufsize)) {
        hid_vendor_send_status();
    }
}
