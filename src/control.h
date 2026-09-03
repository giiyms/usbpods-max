// SPDX-License-Identifier: GPL-3.0-only
//
// USBPods Max control plane: CDC serial menu (Mac `screen`) and vendor HID
// (Windows Chrome/Edge WebHID). Same commands, no HTTP stack on the Pico.
// See web/PROTOCOL.md.
//

#ifndef USBPODS_CONTROL_H
#define USBPODS_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

#define CTRL_REPORT_LEN  32

// HID vendor usage page for the WebHID picker (must match the report descriptor).
#define CTRL_HID_USAGE_PAGE  0xFFA0

enum {
    CTRL_CMD_GET_STATUS = 0x01,
    CTRL_CMD_SET_GAIN   = 0x02,
    CTRL_CMD_PAIR       = 0x03,
    CTRL_CMD_DISCONNECT = 0x04,
    CTRL_CMD_RECONNECT  = 0x05,
    CTRL_CMD_SET_SLOT   = 0x06,
    CTRL_CMD_SET_NOISE  = 0x07,
    CTRL_CMD_SET_CA     = 0x08,
};

enum {
    CTRL_RSP_STATUS     = 0x01,
    CTRL_RSP_ACK        = 0x02,
};

void control_init(void);

// Drain CDC RX / print connect banner. Call from the USB task (same context
// as tud_task / debug_cdc_task).
void control_usb_task(void);

// Execute latched pair/disconnect/slot/AACP commands with the BTstack lock
// held by the caller, plus mic-gain flash persist. Call from the main loop.
void control_main_task(void);

// Fill a 32-byte HID/status report (CTRL_RSP_STATUS).
void control_fill_status(uint8_t *buf, uint16_t len);

#endif // USBPODS_CONTROL_H
