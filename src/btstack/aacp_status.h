// SPDX-License-Identifier: BSD-3-Clause
//
// TinyUSB-safe AACP status getters. Do not include btstack.h here —
// uac.c already includes tusb.h and cannot share hid_report_type_t.
//

#ifndef AACP_STATUS_H
#define AACP_STATUS_H

#include <stdint.h>
#include <stdbool.h>

// Conversation Awareness speaking duck for USB speaker (A2DP out) only.
// 255 = full volume (no duck). Never applied to the mic path.
uint8_t aacp_get_ca_duck_q8(void);
uint8_t aacp_get_ca_speak_level(void);

#endif // AACP_STATUS_H
