#ifndef USBPODS_PICO_W_LED_H
#define USBPODS_PICO_W_LED_H

#include <stdint.h>
#include <stdbool.h>

#define MAC_LEN               6

void set_led_mode_pairing();

void set_led_mode_playing_sbc();

void set_led_mode_playing_ldac();

void set_led_mode_playing_aac();

void set_led_mode_off();

void set_led_mode_triple_blink();

void set_led_mode_double_blink();

void stop_triple_blink();

void stop_double_blink();

uint8_t read_uint8_last_flash();

bool write_uint8_last_flash(uint8_t value);

void read_slot1_mac(uint8_t mac[MAC_LEN]);

bool write_slot1_mac(const uint8_t mac[MAC_LEN]);

void read_slot2_mac(uint8_t mac[MAC_LEN]);

bool write_slot2_mac(const uint8_t mac[MAC_LEN]);

// Mic gain dB (0-24) in the same flash page as the slot MACs. 0xFF / no magic → 0.
uint8_t read_mic_gain_flash(void);
bool write_mic_gain_flash(uint8_t gain_db);

// Host AACP prefs packed next to gain (same flash page). extra magic 0xA5.
typedef struct {
    uint8_t auto_ans;     // 1 on, 2 off
    uint8_t chime;        // 0..100
    uint8_t adapt_vol;    // 1 on, 2 off
    uint8_t sleep_det;    // 1 on, 2 off
    uint8_t crown_dir;    // 1 reversed, 2 default
    uint8_t listen_mask;  // 0x1A bitmask
} host_prefs_t;

void read_host_prefs_flash(host_prefs_t *out);
bool write_host_settings_flash(uint8_t gain_db, const host_prefs_t *prefs);

#endif