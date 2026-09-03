// SPDX-License-Identifier: GPL-3.0-only
//
// USB HID Consumer Control (play/pause, next/prev, volume, mute).
// Header is TinyUSB- and BTstack-free so AACP/AVRCP can latch keys.
// Reports are sent from hid_consumer_task() in the USB task context.
// Crown volume is UAC absolute, not HID Vol Up/Down.
//

#ifndef USBPODS_HID_CONSUMER_H
#define USBPODS_HID_CONSUMER_H

#include <stdint.h>
#include <stdbool.h>

// TinyUSB HID instance index (order in the config descriptor).
#define HID_INST_VENDOR    0
#define HID_INST_CONSUMER  1

// 1-byte report bits — must match desc_hid_consumer_report[] in usb_descriptors.c.
#define HID_CC_VOL_UP     (1u << 0)
#define HID_CC_VOL_DOWN   (1u << 1)
#define HID_CC_MUTE       (1u << 2)
#define HID_CC_PLAY_PAUSE (1u << 3)
#define HID_CC_NEXT       (1u << 4)
#define HID_CC_PREV       (1u << 5)
#define HID_CC_PLAY       (1u << 6)
#define HID_CC_PAUSE      (1u << 7)

// IRQ-safe: latch a press. The USB task sends press then release (0).
void hid_consumer_press(uint8_t bits);
void hid_consumer_play_pause(void);
void hid_consumer_play(void);
void hid_consumer_pause(void);
void hid_consumer_next(void);
void hid_consumer_prev(void);
void hid_consumer_vol_up(void);
void hid_consumer_vol_down(void);
void hid_consumer_mute(void);

// Drain the latch into TinyUSB. Call from tinyusb_task() only (tud_task context).
void hid_consumer_task(void);

#endif // USBPODS_HID_CONSUMER_H
