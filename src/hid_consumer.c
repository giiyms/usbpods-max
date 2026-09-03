// SPDX-License-Identifier: GPL-3.0-only
//
// TinyUSB HID Consumer Control — include tusb.h here only, never btstack.h.
//

#include "hid_consumer.h"

#include "tusb.h"

#define HID_CC_Q_LEN 8

static volatile uint8_t  q_bits[HID_CC_Q_LEN];
static volatile uint8_t  q_head = 0;
static volatile uint8_t  q_count = 0;
static bool              need_release = false;

void hid_consumer_press(uint8_t bits) {
    if (bits == 0) return;
    if (q_count >= HID_CC_Q_LEN) return;
    uint8_t slot = (uint8_t)((q_head + q_count) % HID_CC_Q_LEN);
    q_bits[slot] = bits;
    q_count++;
}

void hid_consumer_play_pause(void) { hid_consumer_press(HID_CC_PLAY_PAUSE); }
void hid_consumer_next(void)       { hid_consumer_press(HID_CC_NEXT); }
void hid_consumer_prev(void)       { hid_consumer_press(HID_CC_PREV); }
void hid_consumer_vol_up(void)     { hid_consumer_press(HID_CC_VOL_UP); }
void hid_consumer_vol_down(void)   { hid_consumer_press(HID_CC_VOL_DOWN); }
void hid_consumer_mute(void)       { hid_consumer_press(HID_CC_MUTE); }

void hid_consumer_task(void) {
    if (!tud_hid_n_ready(HID_INST_CONSUMER)) return;

    if (need_release) {
        uint8_t z = 0;
        tud_hid_n_report(HID_INST_CONSUMER, 0, &z, 1);
        need_release = false;
        return;
    }

    if (q_count == 0) return;
    uint8_t bits = q_bits[q_head];
    q_head = (uint8_t)((q_head + 1) % HID_CC_Q_LEN);
    q_count--;
    tud_hid_n_report(HID_INST_CONSUMER, 0, &bits, 1);
    need_release = true;
}
