/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jerzy Kasenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

 #include <stdio.h>
 #include <string.h>
 
 #include "bsp/board_api.h"
 #include "tusb.h"
 #include "usb_descriptors.h"
 #include "debug_cdc.h"
 #include "spk_frame_align.h"

 #include "../btstack/btstack_avdtp_source.h"
 #include "../btstack/aacp_mic_dec.h"
 #include "../btstack/aacp_status.h"
 #include "../mic_gain.h"
#include "../control.h"
#include "pico/flash.h"
#include "pico/time.h"


 
 //--------------------------------------------------------------------+
 // MACRO CONSTANT TYPEDEF PROTOTYPES
 //--------------------------------------------------------------------+
 
 // List of supported sample rates
 const uint32_t sample_rates[] = {48000};

 uint32_t current_sample_rate  = 48000;

 #define BT_VOL_MAX   127
#define USB_ATT_MAX  25600
#define BT_VOL_ECHO_IGNORE_MS 400

static volatile bool need_change_bt_volume = false;
static volatile uint32_t bt_vol_echo_until_ms = 0;
static volatile int16_t  bt_vol_published_usb = 0;
 
 #define N_SAMPLE_RATES  TU_ARRAY_SIZE(sample_rates)
 
 /* Blink pattern
  * - 25 ms   : streaming data
  * - 250 ms  : device not mounted
  * - 1000 ms : device mounted
  * - 2500 ms : device is suspended
  */
 enum
 {
   BLINK_STREAMING = 25,
   BLINK_NOT_MOUNTED = 250,
   BLINK_MOUNTED = 1000,
   BLINK_SUSPENDED = 2500,
 };
 
 enum
 {
   VOLUME_CTRL_0_DB = 0,
   VOLUME_CTRL_10_DB = 2560,
   VOLUME_CTRL_20_DB = 5120,
   VOLUME_CTRL_30_DB = 7680,
   VOLUME_CTRL_40_DB = 10240,
   VOLUME_CTRL_50_DB = 12800,
   VOLUME_CTRL_60_DB = 15360,
   VOLUME_CTRL_70_DB = 17920,
   VOLUME_CTRL_80_DB = 20480,
   VOLUME_CTRL_90_DB = 23040,
   VOLUME_CTRL_100_DB = 25600,
   VOLUME_CTRL_SILENCE = 0x8000,
 };

 static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

static uint32_t uac_now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

static bool uac_in_bt_vol_echo_window(void) {
  return (int32_t)(uac_now_ms() - bt_vol_echo_until_ms) < 0;
}

static bool uac_host_vol_is_echo(int16_t host_vol) {
  if (uac_in_bt_vol_echo_window()) return true;
  if (host_vol == bt_vol_published_usb) return true;
  return false;
}

static void uac_note_bt_vol_published(int16_t usb_vol) {
  bt_vol_published_usb = usb_vol;
  bt_vol_echo_until_ms = uac_now_ms() + BT_VOL_ECHO_IGNORE_MS;
}
 
 // Audio controls
 // Current states
int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];    // +1 for master channel 0

 // Buffer for microphone data
 //int32_t mic_buf[CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 4];

 // Buffer for speaker data
 int32_t spk_buf[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 4];
 // Speaker data size received in the last frame
 int spk_data_size;
 static spk_frame_align_t spk_align;
 static uint32_t spk_misalign_last_log;

 static void spk_stream_reset(void) {
   spk_data_size = 0;
   spk_frame_align_reset(&spk_align);
   audio_slot_reset_filling();
 }

 uint32_t usb_spk_misalign_count(void) {
   return spk_align.misalign;
 }
 // Resolution per format
 const uint8_t resolutions_per_format[CFG_TUD_AUDIO_FUNC_1_N_FORMATS] = {CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                                                         CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX};
 // Current resolution, update on format change
 uint8_t current_resolution;
 
 //void led_blinking_task(void);
 void audio_task(void);
 void audio_control_task(void);
 
 /*------------- MAIN -------------*/
 void tinyusb_main(void)
 {


  flash_safe_execute_core_init();

  //board_init();
 
   // init device stack on configured roothub port
   tusb_rhport_init_t dev_init = {
     .role = TUSB_ROLE_DEVICE,
     .speed = TUSB_SPEED_AUTO
   };
   tusb_init(BOARD_TUD_RHPORT, &dev_init);


 }


 void tinyusb_task(void){
    tud_task(); // TinyUSB device task
    debug_cdc_task(); // drain buffered stdout to the CDC console (same context as tud_task)
    control_usb_task();
    audio_task();
 }
 

void tinyusb_control_task(void){
  audio_control_task();
}

 //--------------------------------------------------------------------+
 // Device callbacks
 //--------------------------------------------------------------------+
 
 // Invoked when device is mounted
 void tud_mount_cb(void)
 {
   //blink_interval_ms = BLINK_MOUNTED;
 }
 
 // Invoked when device is unmounted
 void tud_umount_cb(void)
 {
   //blink_interval_ms = BLINK_NOT_MOUNTED;
 }
 
 // Invoked when usb bus is suspended
 // remote_wakeup_en : if host allow us  to perform remote wakeup
 // Within 7ms, device must draw an average of current less than 2.5 mA from bus
 void tud_suspend_cb(bool remote_wakeup_en)
 {
   (void)remote_wakeup_en;
   printf("tud_suspend_cb\n");
 }
 
 // Invoked when usb bus is resumed
 void tud_resume_cb(void)
 {
  printf("tud_resume_cb\n");
 }
 
 // Helper for the mic clock (fixed 64 kHz — see USB_MIC_SAMPLE_RATE)
 static bool tud_audio_mic_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_MIC_CLOCK);

   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_4_t curf = { (int32_t) tu_htole32(USB_MIC_SAMPLE_RATE) };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
     }
     else if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_4_n_t(1) rangef =
       {
         .wNumSubRanges = tu_htole16(1),
         .subrange[0] = { .bMin = (int32_t) USB_MIC_SAMPLE_RATE,
                          .bMax = (int32_t) USB_MIC_SAMPLE_RATE, .bRes = 0 }
       };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
     }
   }
   else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
            request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t cur_valid = { .bCur = 1 };
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
   }
   return false;
 }

 // Helper for clock get requests
 static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);
 
       audio_control_cur_4_t curf = { (int32_t) tu_htole32(current_sample_rate) };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
     }
     else if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_4_n_t(N_SAMPLE_RATES) rangef =
       {
         .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
       };
       TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
       for(uint8_t i = 0; i < N_SAMPLE_RATES; i++)
       {
         rangef.subrange[i].bMin = (int32_t) sample_rates[i];
         rangef.subrange[i].bMax = (int32_t) sample_rates[i];
         rangef.subrange[i].bRes = 0;
         TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int)rangef.subrange[i].bMin, (int)rangef.subrange[i].bMax, (int)rangef.subrange[i].bRes);
       }
 
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
     }
   }
   else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
            request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t cur_valid = { .bCur = 1 };
     TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
   }
   TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
   return false;
 }
 
 // Helper for clock set requests
 static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;
 
   TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
 
     current_sample_rate = (uint32_t) ((audio_control_cur_4_t const *)buf)->bCur;
 
     TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);
 
     return true;
   }
   else
   {
     TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 // Helper for feature unit get requests
 static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request)
 {
   if (request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT)
   {
     if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_1_t mute1 = { .bCur = mic_gain_get_mute() ? 1 : 0 };
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
     }
     else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
     {
       if (request->bRequest == AUDIO_CS_REQ_RANGE)
       {
         audio_control_range_2_n_t(1) range_vol = {
           .wNumSubRanges = tu_htole16(1),
           .subrange[0] = { .bMin = tu_htole16(0),
                            .bMax = tu_htole16(MIC_GAIN_DB_MAX * 256),
                            .bRes = tu_htole16(256) }
         };
         return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
       }
       else if (request->bRequest == AUDIO_CS_REQ_CUR)
       {
         int16_t cur = (int16_t) (mic_gain_get_db() * 256);
         audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(cur) };
         return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
       }
     }
     return false;
   }

   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR)
   {
     audio_control_cur_1_t mute1 = { .bCur = mute[request->bChannelNumber] };
     TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
     return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     if (request->bRequest == AUDIO_CS_REQ_RANGE)
     {
       audio_control_range_2_n_t(1) range_vol = {
         .wNumSubRanges = tu_htole16(1),
         .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
       };
       TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
               range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
     }
     else if (request->bRequest == AUDIO_CS_REQ_CUR)
     {
       audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[request->bChannelNumber]) };
       TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
       return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
     }
   }
   TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 // Helper for feature unit set requests
 static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf)
 {
   (void)rhport;

   if (request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT)
   {
     TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
     if (request->bControlSelector == AUDIO_FU_CTRL_MUTE)
     {
       TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
       mic_gain_set_mute(((audio_control_cur_1_t const *)buf)->bCur != 0);
       return true;
     }
     else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
     {
       TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
       int16_t v = (int16_t) tu_le16toh(((audio_control_cur_2_t const *)buf)->bCur);
       // UAC2 silence is 0x8000 — mute, not wrapped 0 dB.
       if ((uint16_t) v == VOLUME_CTRL_SILENCE) {
         mic_gain_set_mute(true);
         return true;
       }
       if (v < 0) v = 0;
       int db = (v + 128) / 256;
       if (db > MIC_GAIN_DB_MAX) db = MIC_GAIN_DB_MAX;
       mic_gain_set_mute(false);
       mic_gain_set_db((uint8_t) db);
       return true;
     }
     return false;
   }

   TU_ASSERT(request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT);
   TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
 
   if (request->bControlSelector == AUDIO_FU_CTRL_MUTE)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
 
     int8_t host_mute = ((audio_control_cur_1_t const *)buf)->bCur;
     TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, host_mute);

     if (request->bChannelNumber == 0 && uac_in_bt_vol_echo_window()) {
       // Crown/UAC interrupt echo — do not latch Mute or bounce AVRCP to 0.
       return true;
     }
     mute[request->bChannelNumber] = host_mute;
     // Host mute zeros the USB stream locally. Do not set_bt_volume(-50).
     return true;
   }
   else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME)
   {
     TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
 
     int16_t host_vol = ((audio_control_cur_2_t const *)buf)->bCur;
     TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, host_vol/256);

     if (request->bChannelNumber == 0) {
       if (uac_host_vol_is_echo(host_vol)) {
         if (uac_in_bt_vol_echo_window()) {
           volume[0] = bt_vol_published_usb;
         }
         return true;
       }
       volume[0] = host_vol;
       need_change_bt_volume = true;
     } else {
       volume[request->bChannelNumber] = host_vol;
     }
     return true;
   }
   else
   {
     TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
     return false;
   }
 }
 
 //--------------------------------------------------------------------+
 // Application Callback API Implementations
 //--------------------------------------------------------------------+
 
 // Invoked when audio class specific get request received for an entity
 bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_get_request(rhport, request);
   if (request->bEntityID == UAC2_ENTITY_MIC_CLOCK)
     return tud_audio_mic_clock_get_request(rhport, request);
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT ||
       request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT)
     return tud_audio_feature_unit_get_request(rhport, request);
   else
   {
     TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
             request->bEntityID, request->bControlSelector, request->bRequest);
   }
   return false;
 }
 
 // Invoked when audio class specific set request received for an entity
 bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
 {
   audio_control_request_t const *request = (audio_control_request_t const *)p_request;
 
   if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT ||
       request->bEntityID == UAC2_ENTITY_MIC_FEATURE_UNIT)
     return tud_audio_feature_unit_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_CLOCK)
     return tud_audio_clock_set_request(rhport, request, buf);
   if (request->bEntityID == UAC2_ENTITY_MIC_CLOCK)
   {
     // Fixed 64 kHz clock — accept a redundant SET of the only valid rate.
     (void)buf;
     return (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ &&
             request->bRequest == AUDIO_CS_REQ_CUR);
   }
   TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
           request->bEntityID, request->bControlSelector, request->bRequest);
 
   return false;
 }
 
 // --- Mic lifecycle requests (the USB alt setting is the only trigger) ---
 // These callbacks run in USB (timer IRQ) context; BTstack must not be called
 // from here (same rule as the buttons in main.c). Latch a request; the main
 // loop executes aacp_mic_start/stop with the async-context lock held.
 static volatile bool mic_start_pending = false;
 static volatile bool mic_stop_pending  = false;

 bool usb_mic_take_start_request(void) {
   if (!mic_start_pending) return false;
   mic_start_pending = false;
   return true;
 }

 bool usb_mic_take_stop_request(void) {
   if (!mic_stop_pending) return false;
   mic_stop_pending = false;
   return true;
 }

 bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;

   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt == 0) {
       blink_interval_ms = BLINK_MOUNTED;
       spk_stream_reset();
   }

   if (ITF_NUM_AUDIO_STREAMING_MIC == itf && alt == 0)
   {
       printf("[USB] mic interface closed (alt 0)\n");
       mic_stop_pending = true;
   }

   return true;
 }

 bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const * p_request)
 {
   (void)rhport;
   uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
   uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

   TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0)
       blink_interval_ms = BLINK_STREAMING;

   // Mac Apple Music via USB should steal Max 2 back from the iPhone.
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf && alt != 0) {
       if (!get_a2dp_connected_flag()) {
           control_request_reconnect();
       }
   }

   // Host opened/closed the recording stream (e.g. an app starts capturing).
   if (ITF_NUM_AUDIO_STREAMING_MIC == itf)
   {
       printf("[USB] mic interface alt %d\n", alt);
       if (alt != 0) mic_start_pending = true;
       else          mic_stop_pending  = true;
       return true;
   }

   // Speaker alt-set / stream start: drop sticky remainder and any
   // half-filled A2DP slot so L/R cannot stick across a format change.
   if (ITF_NUM_AUDIO_STREAMING_SPK == itf) {
     spk_stream_reset();
     printf("[USB] speaker alt %d (remainder+slot fill reset, spk_misalign=%lu)\n",
            alt, (unsigned long) spk_align.misalign);
   } else {
     spk_data_size = 0;
   }
   if(alt != 0)
   {
     current_resolution = resolutions_per_format[alt-1];
   }

   return true;
 }

 uint16_t usb_stop_delay = 0;
 bool is_usb_audio_running = false;

 bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
 {
   (void)rhport;
   (void)func_id;
   (void)ep_out;
   (void)cur_alt_setting;
 
   spk_data_size = tud_audio_read(spk_buf, n_bytes_received);

   if (spk_data_size)
   {
    usb_stop_delay = 0;
    set_usb_streaming(true);
    if (current_resolution == 16)
    {
      uint8_t *bytes = (uint8_t *)spk_buf;
      uint16_t n = (uint16_t)spk_data_size;
      uint16_t cap = (uint16_t)sizeof(spk_buf);
      uint32_t mis_before = spk_align.misalign;
      uint16_t aligned = spk_frame_align_ingest(&spk_align, bytes, n, cap);
      uint16_t sample_count = aligned / SPK_STEREO_FRAME_BYTES;

      if (spk_align.misalign != mis_before) {
        uint32_t now = uac_now_ms();
        if (spk_align.misalign == 1 ||
            (int32_t)(now - spk_misalign_last_log) >= 1000) {
          printf("[USB] spk_misalign=%lu leftover=%u (sticky remainder, not pushed)\n",
                 (unsigned long) spk_align.misalign,
                 (unsigned) spk_align.rem_len);
          spk_misalign_last_log = now;
        }
      }

      if (sample_count) {
        int16_t *src = (int16_t *)bytes;

        // Conversation Awareness speaking ducks A2DP out only — never the mic.
        uint8_t duck = aacp_get_ca_duck_q8();
        if (duck < 255) {
          int16_t *s = src;
          uint32_t ns = (uint32_t) sample_count * 2;
          for (uint32_t i = 0; i < ns; i++) {
            s[i] = (int16_t)(((int32_t) s[i] * (int32_t) duck) >> 8);
          }
        }

        audio_slot_push_samples(src, sample_count);
      }

      is_usb_audio_running = true;
      spk_data_size = 0;
    }
   } 

   return true;
 }
 
 // Feed the mic EP IN: called by the audio driver before each IN transfer is
 // loaded. Pull 1ms of samples (64 @ 64 kHz) from the decoder's PCM ring and
 // zero-fill any shortfall so the host always gets a full frame.
 bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting)
 {
   (void)rhport;
   (void)itf;
   (void)ep_in;
   (void)cur_alt_setting;

   int16_t buf[USB_MIC_SAMPLE_RATE / 1000];   // 64 samples / ms, mono
   uint32_t got = aacp_mic_pcm_read(buf, TU_ARRAY_SIZE(buf));
   for (uint32_t i = got; i < TU_ARRAY_SIZE(buf); i++) buf[i] = 0;
   mic_gain_apply(buf, TU_ARRAY_SIZE(buf));

   tud_audio_write(buf, sizeof(buf));
   return true;
 }
 
 //--------------------------------------------------------------------+
 // AUDIO Task
 //--------------------------------------------------------------------+


 void audio_task(void)
 {
  if (is_usb_audio_running){
    usb_stop_delay = 0;
  }else{
    usb_stop_delay++;
    if (usb_stop_delay > 100){
      set_usb_streaming(false);
    }
  }
  is_usb_audio_running = false;
 }
 

void audio_control_task(void)
 {
   if (*get_is_bt_sink_volume_changed_ptr())
   {
    uint8_t bt_level = get_bt_volume();

    // Crown volume is silence at 0, not Feature Unit mute.
    mute[0] = 0;

    uint16_t usb_level = (bt_level > BT_VOL_MAX)
                       ? USB_ATT_MAX
                       : (uint16_t)(( (uint32_t)(BT_VOL_MAX - bt_level)
                                     * USB_ATT_MAX
                                     + (BT_VOL_MAX/2) )
                                   / BT_VOL_MAX);

    volume[0] = (int16_t)(-1 * usb_level / 2);
    uac_note_bt_vol_published(volume[0]);

     const audio_interrupt_data_t data = {
       .bInfo = 0,
       .bAttribute = AUDIO_CS_REQ_CUR,
       .wValue_cn_or_mcn = 0,
       .wValue_cs = AUDIO_FU_CTRL_VOLUME,
       .wIndex_ep_or_int = 0,
       .wIndex_entity_id = UAC2_ENTITY_SPK_FEATURE_UNIT,
     };
 
     tud_audio_int_write(&data);
     *get_is_bt_sink_volume_changed_ptr() = false;
   }

   if (need_change_bt_volume){
    // Host slider only. Do not echo mute→AVRCP 0. Do not interrupt back
    // (that re-triggers SET_CUR).
    set_bt_volume(volume[0]/256);
    uac_note_bt_vol_published(volume[0]);
    need_change_bt_volume = false;
   }
  }
 
