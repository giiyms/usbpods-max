/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jerzy Kasenbreg
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

 #ifndef _USB_DESCRIPTORS_H_
 #define _USB_DESCRIPTORS_H_

 // #include "tusb.h"

 // Unit numbers are arbitrary selected
 #define UAC2_ENTITY_CLOCK               0x04
 // Speaker path
 #define UAC2_ENTITY_SPK_INPUT_TERMINAL  0x01
 #define UAC2_ENTITY_SPK_FEATURE_UNIT    0x02
 #define UAC2_ENTITY_SPK_OUTPUT_TERMINAL 0x03
 // Microphone path (AirPods hi-res mic). The mic runs at 64 kHz
 // (the AirPods deliver 133.3 AUs/s x 480 samples — librepods exposes its
 // virtual mic at 64000 Hz too) while the speaker runs at 48 kHz, so the mic
 // terminals reference their own fixed clock entity.
 #define UAC2_ENTITY_MIC_INPUT_TERMINAL  0x11
 #define UAC2_ENTITY_MIC_OUTPUT_TERMINAL 0x13
 #define UAC2_ENTITY_MIC_CLOCK           0x14

 #define USB_MIC_SAMPLE_RATE             64000

 enum
 {
   ITF_NUM_AUDIO_CONTROL = 0,
   ITF_NUM_AUDIO_STREAMING_SPK,
   ITF_NUM_AUDIO_STREAMING_MIC,
   ITF_NUM_CDC,        // CDC-ACM control interface (debug console)
   ITF_NUM_CDC_DATA,   // CDC-ACM data interface
   ITF_NUM_TOTAL
 };

 // Number of interfaces spanned by the audio function's IAD (control + 2 streaming).
 #define ITF_NUM_AUDIO_COUNT  3

 #define TUD_AUDIO_HEADSET_STEREO_DESC_LEN (TUD_AUDIO_DESC_IAD_LEN\
     + TUD_AUDIO_DESC_STD_AC_LEN\
     + TUD_AUDIO_DESC_CS_AC_LEN\
     + TUD_AUDIO_DESC_CLK_SRC_LEN\
     + TUD_AUDIO_DESC_INPUT_TERM_LEN\
     + TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN\
     + TUD_AUDIO_DESC_OUTPUT_TERM_LEN\
     /* Mic path entities */\
     + TUD_AUDIO_DESC_CLK_SRC_LEN\
     + TUD_AUDIO_DESC_INPUT_TERM_LEN\
     + TUD_AUDIO_DESC_OUTPUT_TERM_LEN\
     + TUD_AUDIO_DESC_STD_AC_INT_EP_LEN\
     /* Speaker AS interface: Alternate 0 + Alternate 1 */\
     + TUD_AUDIO_DESC_STD_AS_INT_LEN\
     + TUD_AUDIO_DESC_STD_AS_INT_LEN\
     + TUD_AUDIO_DESC_CS_AS_INT_LEN\
     + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN\
     + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN\
     + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN\
     /* Mic AS interface: Alternate 0 + Alternate 1 */\
     + TUD_AUDIO_DESC_STD_AS_INT_LEN\
     + TUD_AUDIO_DESC_STD_AS_INT_LEN\
     + TUD_AUDIO_DESC_CS_AS_INT_LEN\
     + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN\
     + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN\
     + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)

 #define TUD_AUDIO_HEADSET_STEREO_DESCRIPTOR(_stridx, _epout, _epin, _epint) \
     /* Standard Interface Association Descriptor (IAD) */\
     TUD_AUDIO_DESC_IAD(/*_firstitf*/ ITF_NUM_AUDIO_CONTROL, /*_nitfs*/ ITF_NUM_AUDIO_COUNT, /*_stridx*/ 0x00),\
     /* Standard AC Interface Descriptor(4.7.1) */\
     TUD_AUDIO_DESC_STD_AC(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
     /* Class-Specific AC Interface Header Descriptor(4.7.2) */\
     TUD_AUDIO_DESC_CS_AC(/*_bcdADC*/ 0x0200, /*_category*/ AUDIO_FUNC_HEADSET, \
         /*_totallen*/ TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + \
                      TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN + \
                      TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + \
                      TUD_AUDIO_DESC_OUTPUT_TERM_LEN, \
         /*_ctrl*/ AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),\
     /* Clock Source Descriptor(4.7.2.1) — speaker, 48 kHz domain */\
     TUD_AUDIO_DESC_CLK_SRC(/*_clkid*/ UAC2_ENTITY_CLOCK, /*_attr*/ 1, /*_ctrl*/ 1, \
                            /*_assocTerm*/ 0x00,  /*_stridx*/ 0x00),\
     /* Input Terminal Descriptor(4.7.2.4) */\
     TUD_AUDIO_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, \
         /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00, /*_clkid*/ UAC2_ENTITY_CLOCK, \
         /*_nchannelslogical*/ 0x02, /*_channelcfg*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, \
         /*_idxchannelnames*/ 0x00, /*_ctrl*/ 0, /*_stridx*/ 0x00),\
     /* Feature Unit Descriptor(4.7.2.8) */\
     TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(/*_unitid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, \
         /*_srcid*/ UAC2_ENTITY_SPK_INPUT_TERMINAL, \
         /*_ctrlch0master*/ (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS | \
                             AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS), \
         /*_ctrlch1*/ (AUDIO_CTRL_NONE), \
         /*_ctrlch2*/ (AUDIO_CTRL_NONE), /*_stridx*/ 0x00),\
     /* Output Terminal Descriptor(4.7.2.5) */\
     TUD_AUDIO_DESC_OUTPUT_TERM(/*_termid*/ UAC2_ENTITY_SPK_OUTPUT_TERMINAL, \
         /*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES, /*_assocTerm*/ 0x00, \
         /*_srcid*/ UAC2_ENTITY_SPK_FEATURE_UNIT, /*_clkid*/ UAC2_ENTITY_CLOCK, \
         /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),\
     /* Clock Source Descriptor(4.7.2.1) — mic, fixed 64 kHz domain */\
     TUD_AUDIO_DESC_CLK_SRC(/*_clkid*/ UAC2_ENTITY_MIC_CLOCK, /*_attr*/ 1, /*_ctrl*/ 1, \
                            /*_assocTerm*/ 0x00,  /*_stridx*/ 0x00),\
     /* Input Terminal Descriptor(4.7.2.4) — physical microphone */\
     TUD_AUDIO_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL, \
         /*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC, /*_assocTerm*/ 0x00, /*_clkid*/ UAC2_ENTITY_MIC_CLOCK, \
         /*_nchannelslogical*/ 0x01, /*_channelcfg*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, \
         /*_idxchannelnames*/ 0x00, /*_ctrl*/ 0, /*_stridx*/ 0x00),\
     /* Output Terminal Descriptor(4.7.2.5) — mic to USB host */\
     TUD_AUDIO_DESC_OUTPUT_TERM(/*_termid*/ UAC2_ENTITY_MIC_OUTPUT_TERMINAL, \
         /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ 0x00, \
         /*_srcid*/ UAC2_ENTITY_MIC_INPUT_TERMINAL, /*_clkid*/ UAC2_ENTITY_MIC_CLOCK, \
         /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),\
     /* Standard AC Interrupt Endpoint Descriptor(4.8.2.1) */\
     TUD_AUDIO_DESC_STD_AC_INT_EP(/*_ep*/ _epint, /*_interval*/ 0x01), \
     /* Speaker: Interface 1, Alternate 0 (zero-bandwidth) */\
     TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x00, 0x00, 0x05),\
     /* Speaker: Interface 1, Alternate 1 (data streaming format 1) */\
     TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x01, 0x01, 0x05),\
     TUD_AUDIO_DESC_CS_AS_INT(UAC2_ENTITY_SPK_INPUT_TERMINAL, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, \
         AUDIO_DATA_FORMAT_TYPE_I_PCM, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),\
     TUD_AUDIO_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, \
         CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),\
     TUD_AUDIO_DESC_STD_AS_ISO_EP(_epout, TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA, \
         TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, \
         CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX), 0x01),\
     TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE, \
         AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),\
     /* Mic: Interface 2, Alternate 0 (zero-bandwidth) */\
     TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x00, 0x00, 0x05),\
     /* Mic: Interface 2, Alternate 1 (data streaming, 16-bit 64 kHz mono) */\
     TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x01, 0x01, 0x05),\
     TUD_AUDIO_DESC_CS_AS_INT(UAC2_ENTITY_MIC_OUTPUT_TERMINAL, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I, \
         AUDIO_DATA_FORMAT_TYPE_I_PCM, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),\
     TUD_AUDIO_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, \
         CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX),\
     TUD_AUDIO_DESC_STD_AS_ISO_EP(_epin, TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA, \
         TUD_AUDIO_EP_SIZE(USB_MIC_SAMPLE_RATE, \
         CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX), 0x01),\
     TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE, \
         AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000)

 #endif
