//
// Debug logging over USB CDC.
//
// Routes stdio (printf) into a lock-free ring buffer, then drains it to the
// composite device's CDC-ACM IN endpoint from the USB task context. This lets
// us read firmware logs (e.g. AACP hex dumps) over the same USB cable as the
// audio device, with no UART adapter. Debug aid only — not a user feature.
//

#ifndef DEBUG_CDC_H
#define DEBUG_CDC_H

// Register the CDC stdio driver so printf() output is captured. Call once,
// after stdio_init_all() and tinyusb_main().
void debug_cdc_init(void);

// Drain buffered stdout to the CDC IN endpoint. Call from the USB task
// (same context as tud_task), after tud_task().
void debug_cdc_task(void);

#endif // DEBUG_CDC_H
