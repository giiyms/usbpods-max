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

#define CTRL_PROTO_MAJOR  1
#define CTRL_PROTO_MINOR  1

enum {
    CTRL_CMD_GET_STATUS   = 0x01,
    CTRL_CMD_SET_GAIN     = 0x02,
    CTRL_CMD_PAIR         = 0x03,
    CTRL_CMD_DISCONNECT   = 0x04,
    CTRL_CMD_RECONNECT    = 0x05,
    CTRL_CMD_SET_SLOT     = 0x06,
    CTRL_CMD_SET_NOISE    = 0x07,
    CTRL_CMD_SET_CA       = 0x08,
    CTRL_CMD_SET_CROWN    = 0x09,
    CTRL_CMD_SET_AUTOANS  = 0x0A,
    CTRL_CMD_SET_CHIME    = 0x0B,
    CTRL_CMD_SET_ADAPT    = 0x0C,
    CTRL_CMD_SET_SLEEP    = 0x0D,
    CTRL_CMD_SET_LISTEN   = 0x0E,
    CTRL_CMD_RENAME       = 0x0F,
    CTRL_CMD_SET_EAR_DET  = 0x10,
    CTRL_CMD_SET_GESTURES = 0x11,
    CTRL_CMD_SET_HOLD     = 0x12,
    CTRL_CMD_SET_AUTOCONN = 0x13,
};

enum {
    CTRL_RSP_STATUS     = 0x01,
    CTRL_RSP_ACK        = 0x02,
};

void control_init(void);

// Drain CDC RX / print connect banner. Call from the USB task (same context
// as tud_task / debug_cdc_task). Implemented in control_usb.c (TinyUSB only).
void control_usb_task(void);

// Execute latched pair/disconnect/slot/AACP commands with the BTstack lock
// held by the caller, plus mic-gain flash persist. Call from the main loop.
void control_main_task(void);

// Fill a 32-byte HID/status report (CTRL_RSP_STATUS). Protocol minor 1
// adds extras in bytes 15 and 22–31; bytes 0–14 and 16–21 stay compatible.
void control_fill_status(uint8_t *buf, uint16_t len);

// CDC/HID helpers used by control_usb.c (no TinyUSB types).
void control_print_status_human(void);
void control_process_line(char *line);
bool control_handle_hid_cmd(uint8_t const *buf, uint16_t len);

// IRQ-safe latches. USB / UAC callbacks must use these, not BTstack APIs.
void control_request_pair(void);
void control_request_disconnect(void);
void control_request_reconnect(void);
void control_request_slot(uint8_t slot);
void control_request_noise(uint8_t mode);
void control_request_ca(uint8_t enable);

#endif // USBPODS_CONTROL_H
