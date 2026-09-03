#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack_run_loop.h"

#include "btstack/btstack_avdtp_source.h"
#include "btstack/btstack_hci.h"
#include "btstack/btstack_aacp.h"
#include "btstack/aacp_mic_dec.h"

#include <stdio.h>
#include "pico/stdlib.h"

#include "pico/multicore.h"

#include "btstack_event.h"

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

#include "tinyusb/uac.h"
#include "tinyusb/debug_cdc.h"
#include "pico_w_led.h"
#include "mic_gain.h"
#include "control.h"
#include "pico/flash.h"


static bool __no_inline_not_in_flash_func(get_bootsel_button)() {
    const uint CS_PIN_INDEX = 1;

    // Must disable interrupts, as interrupt handlers may be in flash, and we
    // are about to temporarily disable flash access!
    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Note we can't call into any sleep functions in flash right now
    for (volatile int i = 0; i < 1000; ++i);

    // The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
    // Note the button pulls the pin *low* when pressed.
#if PICO_RP2040
    #define CS_BIT (1u << 1)
#else
    #define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool button_state = !(sio_hw->gpio_hi_in & CS_BIT);

    // Need to restore the state of chip select, else we are going to have a
    // bad time when we return to code in flash!
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);

    return button_state;
}

// Button events are detected in the 20ms alarm-pool timer IRQ, but BTstack
// APIs (and flash writes) must not be called from there: BTstack runs in the
// cyw43 async context and a call from a foreign IRQ can deadlock on its lock
// — observed as a watchdog reset when pressing the reconnect button. The IRQ
// handlers only latch a request; the main loop executes it with the
// async-context lock held.
static volatile bool btn_single_pending = false;
static volatile bool btn_double_pending = false;
static volatile bool btn_long_pending   = false;

void on_single_press(void){
    printf("key pressed short (single tap)!\n");
    btn_single_pending = true;
}

void on_double_press(void){
    printf("key pressed double!\n");
    btn_double_pending = true;
}

void on_long_press(void){
    printf("key pressed long!\n");
    btn_long_pending = true;
}

// Runs in main-loop (thread) context.
static void process_button_actions(void){
    async_context_t *ctx = cyw43_arch_async_context();

    if (btn_single_pending) {
        btn_single_pending = false;
        async_context_acquire_lock_blocking(ctx);
        if (! get_a2dp_connected_flag()) {
            a2dp_source_reconnect();
        } else {
            increase_vol_by_key();
        }
        async_context_release_lock(ctx);
    }

    if (btn_double_pending) {
        btn_double_pending = false;
        async_context_acquire_lock_blocking(ctx);
        bool allow = get_allow_switch_slot();
        if (!allow) decrease_vol_by_key();
        async_context_release_lock(ctx);

        if (allow) {
            uint8_t currect_slot = read_uint8_last_flash();
            if (currect_slot == 0x1){
                printf("switch to slot 2!\n");
                set_led_mode_off();
                set_led_mode_triple_blink();
                write_uint8_last_flash(0x2);
            } else{
                printf("switch to slot 1!\n");
                set_led_mode_off();
                set_led_mode_double_blink();
                write_uint8_last_flash(0x1);
            }
            async_context_acquire_lock_blocking(ctx);
            get_link_keys();
            async_context_release_lock(ctx);
        }
    }

    if (btn_long_pending) {
        btn_long_pending = false;
        async_context_acquire_lock_blocking(ctx);
        avdtp_disconnect_and_scan();
        async_context_release_lock(ctx);
    }
}

// ---------- parameters ----------
#define DEBOUNCE_US          5000          // 5 ms
#define SHORT_PRESS_MIN_US  100000         // 100 ms
#define LONG_PRESS_MIN_US  1000000         // 1  s
#define DOUBLE_WINDOW_US    500000         // 500 ms

// ---------- state ----------
static bool     debounced_state  = false;   // last stable level
static uint64_t last_edge_us     = 0;       // last time level changed
static uint64_t press_start_us   = 0;       // when current press began
static uint64_t last_release_us  = 0;       // when previous press ended
static uint8_t  tap_counter      = 0;

// call every 1 ms
void check_bootsel_state(void)
{
    bool raw = get_bootsel_button();            // true == pressed
    uint64_t now = time_us_64();

    // --- debounce ---
    if (raw != debounced_state) {
        if (now - last_edge_us >= DEBOUNCE_US) {
            debounced_state = raw;
            last_edge_us = now;

            if (debounced_state) {              // -------- press --------
                press_start_us = now;
            } else {                            // -------- release ------
                uint64_t press_len = now - press_start_us;

                // classify type of this press
                if (press_len >= LONG_PRESS_MIN_US) {
                    on_long_press();
                    tap_counter = 0;            // long press stands alone
                } else if (press_len >= SHORT_PRESS_MIN_US) {
                    tap_counter++;
                    if (tap_counter == 1) {
                        last_release_us = now;  // start double-tap window
                    } else if (tap_counter == 2 &&
                               (now - last_release_us) <= DOUBLE_WINDOW_US) {
                        on_double_press();
                        tap_counter = 0;
                    }
                }
            }
        }
    }

    // timeout: single tap
    if (tap_counter == 1 && (now - last_release_us) > DOUBLE_WINDOW_US) {
        on_single_press();
        tap_counter = 0;
    }
}


bool usb_timer_callback(repeating_timer_t *rt){
    tinyusb_task();
    return true;
}

bool bootsel_timer_callback(repeating_timer_t *rt) {
    (void)rt;
    check_bootsel_state();
    return true;    // keep repeating
}

int main() {

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(100);
    set_sys_clock_khz(250000, true);

    // stdio: UART stdout is disabled at the CMake level (pico_enable_stdio_uart 0)
    // because its blocking writes from IRQs / the BTstack run loop stalled audio.
    // All printf() goes into the CDC ring only (non-blocking producer).
    stdio_init_all();
    debug_cdc_init();   // route printf() into the CDC ring from the very start
                        // (drained over USB once tusb is up and a terminal opens)

    flash_safe_execute_core_init();

    // Distinguish watchdog resets from clean power-on boots in the debug log.
    if (watchdog_caused_reboot()) {
        printf("!!! BOOT REASON: WATCHDOG RESET (previous run hung)\n");
    } else {
        printf("boot reason: normal power-on/reset\n");
    }

    uint8_t currect_slot = read_uint8_last_flash();

    printf("init slot is %d\n", currect_slot);

    if (currect_slot != 0x1 && currect_slot != 0x2){
        printf("Doesn't has slot, set the slot to 1\n");
        write_uint8_last_flash(0x1);
        currect_slot = read_uint8_last_flash();
    }

    tinyusb_main();

    audio_slot_queue_init();
    mic_gain_init();
    control_init();

    printf("init ctw43.\n");

    // initialize CYW43 driver
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init() failed.\n");
        return -1;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    btstack_main(0, NULL);
    sleep_ms(200);

    // NOTE: FDK-AAC hangs on Core 1 (likely internal global state / malloc contention)
    // Keep encoding on Core 0 for now
    // multicore_launch_core1_with_stack(core1_aaceld_encoder_loop, core1_stack, sizeof(core1_stack));

    static repeating_timer_t usb_timer;
    // NEGATIVE interval (hard period): fire every 500us measured from the
    // previous START, so tinyusb_task() runs at a steady cadence regardless of
    // how long each call takes. USB audio is isochronous — the OUT endpoint
    // must be serviced on a fixed rhythm or samples drift/underrun.
    add_repeating_timer_us(-500, usb_timer_callback, NULL, &usb_timer);

    static repeating_timer_t bootsel_timer;
    add_repeating_timer_us(20000, bootsel_timer_callback, NULL, &bootsel_timer);

    // Enable watchdog — auto-resets if the main loop stops feeding it.
    // 8s (upstream used 2s): the mic decoder init below allocates ~258KB in
    // many chunks from this loop, and long-term testing was done at this value.
    watchdog_enable(8000, true);

    bool dec_init_done = false;
    while (1) {
        watchdog_update();  // feed the watchdog
        process_button_actions();
        tinyusb_control_task();
        control_main_task();

        // Mic lifecycle: USB alt-setting callbacks (IRQ context) may not call
        // BTstack directly, so they only latch requests; execute them here
        // with the async-context lock — same pattern as the buttons above.
        if (usb_mic_take_start_request()) {
            async_context_t *ctx = cyw43_arch_async_context();
            async_context_acquire_lock_blocking(ctx);
            aacp_mic_start();
            async_context_release_lock(ctx);
        }
        if (usb_mic_take_stop_request()) {
            async_context_t *ctx = cyw43_arch_async_context();
            async_context_acquire_lock_blocking(ctx);
            aacp_mic_stop();
            async_context_release_lock(ctx);
        }

        // Open the AAC-ELD mic decoder from the main loop (plain thread
        // context — opening it from a BTstack callback hung on hardware),
        // delayed until well after boot so the ~258KB of decoder mallocs
        // can't stall USB enumeration.
        if (!dec_init_done && to_ms_since_boot(get_absolute_time()) > 8000) {
            dec_init_done = true;
            aacp_mic_dec_init();
        }
        sleep_ms(50);
    }

}
