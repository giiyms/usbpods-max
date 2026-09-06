#include <stdbool.h>
#include <stdint.h>

void tinyusb_main(void);
void tinyusb_task(void);
void tinyusb_control_task(void);

// Mic lifecycle requests latched by the USB alt-setting callbacks
// (IRQ context). The main loop polls these and drives aacp_mic_start/stop
// with the async-context lock held. Each call consumes the request.
bool usb_mic_take_start_request(void);
bool usb_mic_take_stop_request(void);

// USB speaker ISO leftover 1–3 byte events (sticky remainder; CDC / @STATUS).
uint32_t usb_spk_misalign_count(void);
