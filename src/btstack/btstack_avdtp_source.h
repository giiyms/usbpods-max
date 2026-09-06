//
// Created by Sean on 8/7/23.
//

#ifndef PICOW_USB_BT_AUDIO_SSP_COUNTER_H
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H

#include <stdint.h>
#include <stdbool.h>

// Slot queue constants
// NOTE (hires-mic): the pool was 24 slots = 96KB of static RAM. The AAC-ELD
// mic decoder needs ~260KB of heap and the encoder opens on top of that, so
// the pool is trimmed to 8 slots (32KB) to make room. ELD (the AirPods path)
// uses only 6 slots; SBC/AAC/LDAC sinks get a shallower queue (8) which
// shortens their jitter buffer but keeps them functional.
#define AUDIO_SLOT_COUNT_SBC   8    // SBC: 128 samples/slot, 8*2.67ms = 21ms buffer
#define AUDIO_SLOT_COUNT_AAC   8    // AAC-LC: 1024 samples/slot, 8*21.3ms = 171ms buffer
#define AUDIO_SLOT_COUNT_ELD   6    // AAC-ELD: 480 samples/slot, 6*10ms = 60ms buffer
#define AUDIO_SLOT_COUNT_LDAC  8    // LDAC: 256 samples/slot, 8*5.3ms = 43ms buffer
#define AUDIO_SLOT_COUNT_MAX   8    // pool size = max of all above
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define AUDIO_SLOT_MAX_INT16   (AUDIO_SLOT_MAX_SAMPLES * 2)  // stereo

// Slot queue API (multicore-safe)
void audio_slot_queue_init(void);
void audio_slot_queue_configure(uint16_t samples_per_slot);
void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count);
// Drop a half-filled slot (USB speaker alt-set / stream start). Keeps L/R
// pairs aligned; does not invent samples.
void audio_slot_reset_filling(void);

bool check_is_streaming();

void set_bt_volume(int16_t);

uint8_t get_bt_volume();

bool get_bt_mute();

void set_usb_streaming(bool flag);

bool * get_is_bt_sink_volume_changed_ptr();

int btstack_main(int argc, const char * argv[]);

void avdtp_disconnect_and_scan(void);

// Drop A2DP/AVRCP without wiping the slot MAC or link key (web/serial Disconnect).
void avdtp_disconnect_keep_pairing(void);

// Drop A2DP without wiping the slot. Hold auto-reclaim (explicit disconnect / pair).
void avdtp_reclaim_hold_set(bool hold);

void gap_start_scanning(void);

bool get_a2dp_connected_flag();

void a2dp_source_reconnect();

void avdtp_source_establish_stream();

void set_next_capablity_and_start_stream();

void start_led_blink();

static int setup_aac_configuration();

static int setup_sbc_configuration();

static int set_ldac_configuration();

bool get_allow_switch_slot();

void core1_aaceld_encoder_loop(void);

void increase_vol_by_key();

void decrease_vol_by_key();

#endif //PICOW_USB_BT_AUDIO_SSP_COUNTER_H
